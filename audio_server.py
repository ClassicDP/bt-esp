#!/usr/bin/env python3
"""
Optimized audio server with improved packet control and jitter management:
- Single TCP client (ESP32).
- Binary packets with header (little-endian):
    uint32 magic      (0x48445541 'AUDH')
    uint32 seq        (32-bit packet sequence)
    uint64 timestamp_us (ESP32 capture timestamp)
    uint16 payload_len (PCM payload length)
    uint16 codec       (1 -> 8000 Hz, 2 -> 16000 Hz)
- Enhanced packet loss detection and concealment
- Adaptive jitter buffer with underrun prevention
- Real-time sequence validation
"""

import socket
import struct
import time
import argparse
import wave
import queue
import threading
import glob
import os
import sys
import atexit
import csv

# ---- Protocol constants ----
STREAM_HEADER_STRUCT = struct.Struct('<IIQHH')
STREAM_HEADER_SIZE = STREAM_HEADER_STRUCT.size
STREAM_MAGIC_NEW = 0x48445541
STREAM_MAGIC_OLD = 0x41554448
CODEC_CVSD = 1     # treat as raw 8 kHz PCM
CODEC_MSBC = 2     # treat as raw 16 kHz PCM
LOSS_IGNORE_HUGE_GAP = 100000  # ignore gaps >= this (assume reset)

class AudioServer:
    def __init__(self, host='0.0.0.0', port=8888, segment_seconds=5):
        self.host = host
        self.port = port
        self.segment_seconds = segment_seconds

        self.running = False
        self.accept_legacy_magic = True

        # Enhanced packet statistics
        self.total_packets = 0
        self.missed_packets = 0
        self.dropped_packets = 0
        self.last_seq = None

        # Packet control / analysis (enhanced)
        self.expected_seq = None
        self.dup_packets = 0
        self.reordered_packets = 0
        self.gap_events = 0
        self.max_gap = 0
        self.packet_times = []  # For jitter calculation
        self.arrival_times = []

        # CSV logging for detailed analysis
        self.packet_log_path = "packet_log.csv"
        self.packet_log_file = open(self.packet_log_path, "w", newline='')
        self.packet_csv = csv.writer(self.packet_log_file)
        self.packet_csv.writerow([
            "time_s","seq","expected","gap","event","delta_ms",
            "jitter_ms","lost_total","dup_total","reorder_total",
            "qsize","underrun","buffer_ms","esp32_ts_us"
        ])

        # Enhanced timing diagnostics
        self.prev_arrival = None
        self.avg_delta_ms = 0.0
        self.jitter_ms = 0.0
        self.underrun_events = 0
        self.buffer_health = 0.0

        # Edge / continuity diagnostics & concealment (added missing variables)
        self.prev_last_sample = None
        self.edge_jump_threshold = 1500   # int16 threshold for edge detection
        self._curr_mean = 0
        self._curr_edge_jump = 0
        self.inserted_conceal_frames = 0
        self._gap_conceal_pending = 0
        self.max_gap_conceal_frames = 20  # limit PLC frames per gap
        self._curr_delta_ms = 0.0
        self._curr_jitter_ms = 0.0
        self._curr_qsize = 0
        self._curr_underrun = 0

        # Audio params with adaptive sample rate
        self.sample_rate = 8000
        self.channels = 1
        self.bits_per_sample = 16

        # Buffers
        self.recv_buffer = b''
        self.header_done = False

        # WAV segment
        self.segment_start = time.time()
        self.segment_frames = []

        # Enhanced jitter buffer
        self.audio_queue = queue.Queue(maxsize=300)  # Уменьшили буфер
        self.prebuffer_ms = 50   # Быстрый старт
        self.min_buffer_ms = 25  # Низкий минимум
        self.max_buffer_ms = 200 # Умеренный максимум
        self.target_buffer_ms = 75  # Целевое значение буфера

        # Improved concealment
        self.silence_frame = None
        self.last_good_frame = None
        self.concealment_fade_samples = 10  # Плавный переход для уменьшения щелчков

        # Frame timing
        self.frame_samples = 60  # Default for CVSD 7.5ms frames
        self.bytes_per_sample = 2
        self.play_started = False
        self.buffered_ms = 0.0
        self.stream = None
        self.audio = None

        atexit.register(self._exit_summary)
        atexit.register(self._close_packet_log)

    def _close_packet_log(self):
        try:
            if hasattr(self, "packet_log_file") and self.packet_log_file:
                self.packet_log_file.flush()
                self.packet_log_file.close()
        except:
            pass

    # ---------- Internal helpers ----------
    def _init_audio(self):
        import pyaudio
        if self.audio is None:
            self.audio = pyaudio.PyAudio()
        if self.stream:
            try:
                self.stream.stop_stream()
                self.stream.close()
            except:
                pass
        format_map = {
            8: pyaudio.paInt8,
            16: pyaudio.paInt16,
            24: pyaudio.paInt24,
            32: pyaudio.paInt32
        }
        fmt = format_map.get(self.bits_per_sample, pyaudio.paInt16)
        self.stream = self.audio.open(
            format=fmt,
            channels=self.channels,
            rate=self.sample_rate,
            output=True,
            frames_per_buffer=120
        )
        print(f"🔊 Audio stream ready ({self.sample_rate} Hz)")

    def _packet_loss(self, seq, payload_len):
        event = ""
        gap = 0
        now_rel = time.time() - self.segment_start  # relative time since start
        if self.last_seq is None:
            # first packet
            self.expected_seq = seq
            event = "START"
        else:
            self.expected_seq = (self.last_seq + 1) & 0xFFFFFFFF
            if seq == self.last_seq:
                self.dup_packets += 1
                event = "DUP"
            elif seq == self.expected_seq:
                event = "CONT"
            else:
                # forward gap?
                forward_gap = (seq - self.expected_seq) & 0xFFFFFFFF
                if 0 < forward_gap < LOSS_IGNORE_HUGE_GAP:
                    self.missed_packets += forward_gap
                    gap = forward_gap
                    self.gap_events += 1
                    self.max_gap = max(self.max_gap, gap)
                    event = "GAP"
                    # Schedule concealment frames (simple PLC)
                    self._gap_conceal_pending = min(self.max_gap_conceal_frames, gap)
                else:
                    # maybe reorder / wrap
                    if seq < self.last_seq:
                        self.reordered_packets += 1
                        event = "REORDER"
                    else:
                        event = "RESET"
        # Write CSV row
        try:
            if not hasattr(self, "_curr_underrun"):
                self._curr_underrun = 0
            self.packet_csv.writerow([
                f"{now_rel:.6f}", seq, self.expected_seq, gap, event,
                self.missed_packets, self.dup_packets, self.reordered_packets,
                f"{self._curr_delta_ms:.3f}", self._curr_jitter_ms, self._curr_qsize, getattr(self, "_curr_underrun", 0),
                self.buffer_health, self._curr_mean, self._curr_edge_jump, self.inserted_conceal_frames
            ])
            if (self.total_packets % 100) == 0:
                self.packet_log_file.flush()
        except:
            pass
        # Console quick notice for gaps or reorders
        if event == "GAP":
            if gap < 10:
                print(f"⚠️ Gap {gap} packet(s): expected {self.expected_seq}, got {seq}")
            else:
                print(f"⚠️ Gap burst {gap} packets: expected {self.expected_seq}, got {seq}")
        elif event == "REORDER":
            print(f"↕️ Reorder: got {seq} after {self.last_seq}")
        self.last_seq = seq

    def _save_segment_if_due(self):
        now = time.time()
        if now - self.segment_start >= self.segment_seconds and self.segment_frames:
            filename = f"segment_{int(self.segment_start)}.wav"
            try:
                with wave.open(filename, 'wb') as wf:
                    wf.setnchannels(self.channels)
                    wf.setsampwidth(2)  # 16-bit
                    wf.setframerate(self.sample_rate)
                    wf.writeframes(b''.join(self.segment_frames))
                print(f"💾 Saved {filename}")
            except Exception as e:
                print(f"❌ Save error: {e}")
            self.segment_frames.clear()
            self.segment_start = now

    # New playback thread with packet aggregation
    def _playback_thread(self):
        import time
        print("🔊 Playback thread started")

        # Wait for initial buffer to fill
        print(f"⏳ Waiting for prebuffer ({self.prebuffer_ms}ms)...")
        while self.running and self.buffered_ms < self.prebuffer_ms:
            time.sleep(0.01)

        if not self.running:
            return

        print(f"▶️ Starting playback (buffer={self.buffered_ms:.1f}ms)")

        while self.running:
            try:
                # Check buffer health
                if self.buffered_ms < self.min_buffer_ms:
                    # Buffer too low, wait a bit
                    time.sleep(0.005)
                    continue

                # Get and play packet
                try:
                    packet = self.audio_queue.get(timeout=0.01)
                    if self.stream and len(packet) > 0:
                        self.stream.write(packet)
                        # Update last good frame for concealment
                        self.last_good_frame = packet
                except queue.Empty:
                    # No data available
                    time.sleep(0.005)

            except Exception as e:
                print(f"❌ Playback error: {e}")
                time.sleep(0.01)

    def _create_smooth_concealment(self, frame_size):
        """Создает плавный переход для маскировки пропущенных пакетов"""
        if self.last_good_frame and len(self.last_good_frame) >= frame_size:
            # Используем последний хороший кадр с затуханием
            samples = struct.unpack('<' + 'h' * (len(self.last_good_frame) // 2), self.last_good_frame)
            fade_samples = min(len(samples), self.concealment_fade_samples)

            # Создаем затухание для плавного перехода
            concealed_samples = list(samples)
            for i in range(fade_samples):
                fade_factor = (fade_samples - i) / fade_samples * 0.7  # Уменьшаем амплитуду
                concealed_samples[i] = int(concealed_samples[i] * fade_factor)

            # Добавляем небольшой шум для естественности
            import random
            for i in range(len(concealed_samples)):
                noise = random.randint(-50, 50)
                concealed_samples[i] = max(-32768, min(32767, concealed_samples[i] + noise))

            return struct.pack('<' + 'h' * len(concealed_samples), *concealed_samples)
        else:
            # Возвращаем тишину если нет предыдущего кадра
            return b'\x00' * frame_size

    def _adaptive_playback_thread(self):
        """Улучшенный поток воспроизведения с адаптивной буферизацией"""
        import time
        print("🔊 Enhanced playback thread started")

        # Динамические параметры
        target_chunk_ms = 15  # Меньшие чанки для лучшей отзывчивости

        while self.running:
            try:
                # Обновляем samples_per_ms каждый раз на случай изменения sample_rate
                samples_per_ms = self.sample_rate / 1000.0 if self.sample_rate > 0 else 8.0

                # Адаптивная проверка состояния буфера
                current_buffer_ms = self.buffered_ms

                # Если буфер слишком мал, ждем накопления
                if current_buffer_ms < self.min_buffer_ms and self.total_packets > 10:
                    time.sleep(0.005)  # 5ms ожидание
                    continue

                # Если буфер слишком большой, ускоряем воспроизведение
                if current_buffer_ms > self.max_buffer_ms:
                    target_chunk_ms = 20  # Больше чанки для быстрого опустошения
                else:
                    target_chunk_ms = 15  # Нормальный размер

                # Собираем данные для воспроизведения
                chunk_data = bytearray()
                packets_collected = 0
                max_packets = 3  # Ограничиваем количество пакетов за раз

                while packets_collected < max_packets and self.running:
                    try:
                        packet = self.audio_queue.get(timeout=0.01)
                        chunk_data.extend(packet)
                        packets_collected += 1

                        # Сохраняем последний хороший кадр для concealment
                        if len(packet) > 0:
                            self.last_good_frame = packet

                    except queue.Empty:
                        break

                # Воспроизводим собранный чанк
                if chunk_data and self.stream:
                    self.stream.write(bytes(chunk_data))
                else:
                    # Если нет данных, добавляем короткую паузу
                    time.sleep(0.005)

            except Exception as e:
                print(f"❌ Playback error: {e}")
                time.sleep(0.01)

    def _exit_summary(self):
        if self.total_packets:
            print(f"[Exit] received={self.total_packets} lost={self.missed_packets} dropped={self.dropped_packets}")
            print(f"[Exit] gaps={self.gap_events} max_gap={self.max_gap} dup={self.dup_packets} reorder={self.reordered_packets}")
            if self.underrun_events:
                print(f"[Exit] underruns={self.underrun_events}")

    # ---------- Main server loop ----------
    def start(self):
        # Clean old segment files
        for f in glob.glob("segment_*.wav"):
            try: os.remove(f)
            except: pass

        try:
            import pyaudio  # just to fail early if missing
        except ImportError:
            print("❌ PyAudio not installed. Install: pip install pyaudio")
            return

        server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server_sock.bind((self.host, self.port))
        server_sock.listen(1)
        print(f"🎵 Listening on {self.host}:{self.port}")
        print("Waiting for client...")

        try:
            client_sock, addr = server_sock.accept()
            print(f"📱 Client connected: {addr}")
        except Exception as e:
            print(f"❌ Accept failed: {e}")
            server_sock.close()
            return

        self.running = True
        threading.Thread(target=self._playback_thread, daemon=True).start()

        while self.running:
            try:
                chunk = client_sock.recv(4096)
                if not chunk:
                    print("🔌 Client disconnected")
                    break
                self.recv_buffer += chunk

                # Optional ASCII preamble until blank line
                if not self.header_done:
                    sep = self.recv_buffer.find(b"\n\n")
                    if sep != -1:
                        preamble = self.recv_buffer[:sep+2]
                        self.recv_buffer = self.recv_buffer[sep+2:]
                        text = preamble.decode(errors='ignore')
                        if text.startswith("AUDIO_STREAM"):
                            # parse very basic key=value
                            for line in text.splitlines():
                                if '=' in line:
                                    k,v = line.split('=',1)
                                    k=k.strip(); v=v.strip()
                                    if k == 'sample_rate':
                                        self.sample_rate = int(v)
                                    elif k == 'channels':
                                        self.channels = int(v)
                                    elif k == 'bits_per_sample':
                                        self.bits_per_sample = int(v)
                                    elif k == 'codec':
                                        if v.upper() == 'MSBC':
                                            self.sample_rate = 16000
                                        else:
                                            self.sample_rate = 8000
                            print(f"📄 Header: {self.sample_rate}Hz {self.channels}ch {self.bits_per_sample}bit")
                        self.header_done = True
                    else:
                        if len(self.recv_buffer) < 2048:
                            continue
                        # fallback to binary if too long
                        self.header_done = True

                # Sync to first magic
                if self.last_seq is None:
                    magic_new = STREAM_MAGIC_NEW.to_bytes(4, 'little')
                    magic_old = STREAM_MAGIC_OLD.to_bytes(4, 'little')
                    pos_new = self.recv_buffer.find(magic_new)
                    pos_old = self.recv_buffer.find(magic_old) if self.accept_legacy_magic else -1
                    pos = -1
                    chosen = None
                    if pos_new != -1 and (pos_old == -1 or pos_new < pos_old):
                        pos = pos_new; chosen = STREAM_MAGIC_NEW
                    elif pos_old != -1:
                        pos = pos_old; chosen = STREAM_MAGIC_OLD
                    if chosen is None:
                        if len(self.recv_buffer) > 8192:
                            self.recv_buffer = self.recv_buffer[-4096:]
                        continue
                    if pos > 0:
                        self.recv_buffer = self.recv_buffer[pos:]
                    if chosen == STREAM_MAGIC_OLD:
                        print("⚠️ Legacy magic detected.")
                    else:
                        print("✅ Magic synced (AUDH)")
                        self.accept_legacy_magic = False

                # Consume complete packets
                while True:
                    if len(self.recv_buffer) < STREAM_HEADER_SIZE:
                        break
                    header = self.recv_buffer[:STREAM_HEADER_SIZE]
                    magic, seq, ts_us, payload_len, codec = STREAM_HEADER_STRUCT.unpack(header)
                    if not (magic == STREAM_MAGIC_NEW or (self.accept_legacy_magic and magic == STREAM_MAGIC_OLD)):
                        # resync: drop one byte
                        self.recv_buffer = self.recv_buffer[1:]
                        break
                    if payload_len == 0 or payload_len > 4096:
                        # invalid length -> drop one byte
                        self.recv_buffer = self.recv_buffer[1:]
                        break
                    total_needed = STREAM_HEADER_SIZE + payload_len
                    if len(self.recv_buffer) < total_needed:
                        break
                    packet = self.recv_buffer[:total_needed]
                    self.recv_buffer = self.recv_buffer[total_needed:]
                    payload = packet[STREAM_HEADER_SIZE:]
                    # Infer frame size once
                    if self.frame_samples is None:
                        if payload_len % (self.channels * self.bytes_per_sample) == 0:
                            self.frame_samples = payload_len // (self.channels * self.bytes_per_sample)
                            print(f"ℹ️ Inferred frame_samples={self.frame_samples} ({(self.frame_samples / self.sample_rate)*1000:.2f} ms per packet)")

                    # Per-packet sample stats (s16le mono assumed)
                    if payload_len >= 2:
                        sample_count = payload_len // 2
                        # Fast unpack for small frame
                        frame_samples = struct.unpack('<' + 'h'*sample_count, payload)
                        mean_val = int(sum(frame_samples) / sample_count)
                        first_sample = frame_samples[0]
                        last_sample = frame_samples[-1]
                        edge_jump = 0
                        if self.prev_last_sample is not None:
                            edge_jump = abs(first_sample - self.prev_last_sample)
                            if edge_jump > self.edge_jump_threshold:
                                print(f"✨ Edge jump {edge_jump} at seq={seq}")
                        self.prev_last_sample = last_sample
                        self._curr_mean = mean_val
                        self._curr_edge_jump = edge_jump
                    else:
                        self._curr_mean = 0
                        self._curr_edge_jump = 0

                    # Arrival timing diagnostics
                    now = time.time()
                    if self.prev_arrival is None:
                        self._curr_delta_ms = 0.0
                        # Initialize average to zero to avoid None formatting issues
                        self.avg_delta_ms = 0.0
                    else:
                        self._curr_delta_ms = (now - self.prev_arrival) * 1000.0
                        if self.avg_delta_ms == 0.0:
                            self.avg_delta_ms = self._curr_delta_ms
                        else:
                            # exponential smoothing
                            self.avg_delta_ms = 0.9 * self.avg_delta_ms + 0.1 * self._curr_delta_ms
                        if self.avg_delta_ms > 0 and self._curr_delta_ms > self.avg_delta_ms * 2.5 and self._curr_delta_ms > 5:
                            print(f"⏱️  Inter-packet gap {self._curr_delta_ms:.2f} ms (avg ~{self.avg_delta_ms:.2f} ms) seq={seq}")
                    self.prev_arrival = now

                    # Stats
                    self.total_packets += 1
                    self._packet_loss(seq, payload_len)
                    # Insert concealment frames if a gap was detected
                    if self._gap_conceal_pending > 0 and self.frame_samples:
                        # Используем улучшенный concealment вместо простого копирования
                        frame_size = self.frame_samples * 2  # 16-bit samples
                        for _ in range(self._gap_conceal_pending):
                            try:
                                concealed_frame = self._create_smooth_concealment(frame_size)
                                self.audio_queue.put_nowait(concealed_frame)
                                self.inserted_conceal_frames += 1
                            except queue.Full:
                                break
                        self._gap_conceal_pending = 0

                    # Adjust sample rate on codec
                    desired_rate = 16000 if codec == CODEC_MSBC else 8000
                    if desired_rate != self.sample_rate or self.stream is None:
                        self.sample_rate = desired_rate
                        self._init_audio()

                    # Update buffered duration
                    if self.frame_samples:
                        self.buffered_ms = (self.audio_queue.qsize() * self.frame_samples * 1000.0) / self.sample_rate
                    else:
                        self.buffered_ms = 0.0
                    # Queue / underrun check
                    self._curr_qsize = self.audio_queue.qsize()
                    underrun_flag = 0
                    if self.stream and self._curr_qsize == 0 and self.total_packets > 1:
                        # Playback thread likely starved since previous packet
                        underrun_flag = 1
                        self.underrun_events += 1
                        print(f"🥾 Underrun (playback starved) before seq={seq}")
                    self._curr_underrun = underrun_flag

                    # Enqueue payload
                    try:
                        self.audio_queue.put_nowait(payload)
                    except queue.Full:
                        self.dropped_packets += 1

                    # Add to current segment
                    self.segment_frames.append(payload)
                    self._save_segment_if_due()

                    # Light periodic log
                    if (self.total_packets % 200) == 1:
                        avg_disp = self.avg_delta_ms if self.avg_delta_ms is not None else 0.0
                        buf_ms = getattr(self, 'buffered_ms', 0.0)
                        print(f"🕒 seq={seq} packets={self.total_packets} lost={self.missed_packets} gaps={self.gap_events} "
                              f"max_gap={self.max_gap} dup={self.dup_packets} reord={self.reordered_packets} dropped={self.dropped_packets} "
                              f"avgΔ={avg_disp:.2f}ms underruns={self.underrun_events} buf={buf_ms:.1f}ms conceal={self.inserted_conceal_frames}")

            except KeyboardInterrupt:
                print("🛑 Keyboard interrupt")
                break
            except Exception as e:
                print(f"❌ Receive error: {e}")
                break

        self.stop()
        client_sock.close()
        server_sock.close()

    def stop(self):
        if not self.running:
            return
        self.running = False
        print(f"\n📊 Stats: received={self.total_packets} lost={self.missed_packets} dropped={self.dropped_packets}")
        if hasattr(self, 'buffered_ms'):
            print(f"⌛ Final buffer={self.buffered_ms:.1f} ms (pre={self.prebuffer_ms} min={self.min_buffer_ms} max={self.max_buffer_ms})")
        if self.underrun_events:
            print(f"🪫 Total underruns detected: {self.underrun_events}")
        if self.stream:
            try:
                self.stream.stop_stream(); self.stream.close()
            except:
                pass
        if self.audio:
            try:
                self.audio.terminate()
            except:
                pass
        if self.inserted_conceal_frames:
            print(f"🩹 Concealment frames inserted: {self.inserted_conceal_frames}")
        self._close_packet_log()
        print(f"📝 Packet log saved to {self.packet_log_path}")
        print("✅ Server stopped")

def main():
    parser = argparse.ArgumentParser(description="Minimal audio server")
    parser.add_argument('--host', default='0.0.0.0')
    parser.add_argument('--port', type=int, default=8888)
    parser.add_argument('--prebuffer-ms', type=int, default=300, help='Prebuffer duration before playback (ms)')
    parser.add_argument('--min-buffer-ms', type=int, default=40, help='Lower watermark (ms) for PLC insertion')
    parser.add_argument('--max-buffer-ms', type=int, default=160, help='Upper watermark (ms) to clamp latency')
    args = parser.parse_args()
    server = AudioServer(args.host, args.port)
    server.prebuffer_ms = args.prebuffer_ms
    server.min_buffer_ms = args.min_buffer_ms
    server.max_buffer_ms = args.max_buffer_ms
    server.start()

if __name__ == '__main__':
    main()