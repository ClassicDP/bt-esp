#!/usr/bin/env python3
"""
Простой сервер для приема и воспроизведения аудиоданных от ESP32
Поддерживает CVSD и mSBC кодеки
"""


import socket
import threading
import struct
import time
import queue
import pyaudio
import wave
import argparse
import sys
import numpy as np
import subprocess
import tempfile
import os

import atexit

from collections import deque, Counter

# ---- Stream packet header (must match firmware) ----
# C struct (little-endian packed):
# typedef struct {
#     uint32_t magic;
#     uint32_t seq;
#     uint64_t timestamp_us;
#     uint16_t payload_len;
#     uint16_t codec;
# } stream_packet_header_t;
# NOTE: сервер теперь может автоматически переключаться в raw_frame_mode, если не находит magic
# RAW fallback disabled – now strict header mode with dual magic acceptance.
STREAM_HEADER_STRUCT = struct.Struct('<IIQHH')
STREAM_HEADER_SIZE = STREAM_HEADER_STRUCT.size  # 20 bytes
STREAM_MAGIC_NEW = 0x48445541  # 'AUDH' target magic (bytes: 41 55 44 48)
STREAM_MAGIC_OLD = 0x41554448  # legacy magic currently sent by ESP (bytes: 48 44 55 41)
STREAM_MAGIC = STREAM_MAGIC_NEW
STREAM_CODEC_CVSD = 1
STREAM_CODEC_MSBC = 2

PCM_FALLBACK_ENABLED = False  # отключаем raw fallback – хотим строгий заголовок

# Helper to pretty-print magic bytes in little-endian order
def magic_bytes_le(val: int):
    return ' '.join(f'{b:02X}' for b in val.to_bytes(4, 'little'))

# --- Packet sequence analysis tuning constants ---
# Предполагаемая частота пакетов: ~60 сэмплов при 8 кГц => ~133 пакета/сек
PACKETS_PER_SECOND_EST = 133
MAX_REAL_LOSS_WINDOW_SEC = 2          # считаем реальные потери только в окне 2 сек
MAX_REASONABLE_GAP = PACKETS_PER_SECOND_EST * MAX_REAL_LOSS_WINDOW_SEC  # ~266
MAX_ABSOLUTE_GAP = 5000               # жесткая отсечка
BURST_SUPPRESS_THRESHOLD = 1000       # после пропуска > этого отключаем учет пока не восстановится
RECOVER_MIN_CONSECUTIVE_OK = 50       # сколько нормальных инкрементов нужно для восстановления учета

class SequenceAnalyzer:
    """
    Анализирует «сырые» 4 байта счетчика для авто-определения формата.
    Пытается:
      1. Определить endianness (big / little).
      2. Понять, используются ли только отдельные байты (например 16-битный счетчик внутри 32-битного слова, остальные = 0xFF или 0x00).
      3. Выявить «шум» / непригодность поля.
    Пока надежный формат не найден – учет потерь отключен.
    """
    def __init__(self, sample_size=64):
        self.sample_size = sample_size
        self.samples = deque(maxlen=sample_size)
        self.format_determined = False
        self.use_masked_16 = False
        self.mask_bytes = None      # индексы байт, которые образуют счетчик
        self.endian = None          # 'big' / 'little'
        self.unreliable = False
        self.last_seq = None
        self.unreliable_warns = 0
        self.max_unreliable_warns = 5  # ограничим число повторяющихся сообщений

    def add(self, raw4: bytes):
        if len(raw4) != 4:
            return
        self.samples.append(raw4)
        if not self.format_determined and len(self.samples) >= self.sample_size//2:
            self._analyze()

    def _analyze(self):
        # Быстрая эвристика: если встречались значения вида ?? FF ?? FF или FF ?? FF ??,
        # попробуем сначала интерпретировать два непостоянных байта, игнорируя стабильно 0xFF.
        ff_pattern_counts = sum(1 for r in self.samples if r.count(0xFF) >= 2)
        # Если большинство выборок содержат >=2 байтов 0xFF, усилим гипотезу masked 16-bit
        if ff_pattern_counts > len(self.samples) * 0.6 and not self.format_determined:
            # Определим какие байты НЕ 0xFF чаще всего
            non_ff_positions = []
            for i in range(4):
                col = [r[i] for r in self.samples]
                # Частота не-0xFF
                non_ff_ratio = sum(1 for b in col if b != 0xFF) / len(col)
                if non_ff_ratio > 0.2:
                    non_ff_positions.append(i)
            if len(non_ff_positions) == 2:
                self.use_masked_16 = True
                self.mask_bytes = non_ff_positions
        # Подсчет разнообразия по каждому байту
        cols = list(zip(*self.samples))  # 4 списока байтов
        variances = [len(set(c)) for c in cols]

        # Если 2 байта почти константы (variance ==1) и 2 байта меняются - возможно 16-битный счетчик
        changing = [i for i,v in enumerate(variances) if v > 1]
        if len(changing) == 2:
            self.use_masked_16 = True
            self.mask_bytes = changing
        elif len(changing) in (3,4):
            self.use_masked_16 = False
        else:
            # 0 или 1 меняющийся байт -> малый диапазон или мусор
            self.unreliable = True
            self.format_determined = True
            return

        # Проверка endianness: построим последовательности для big и little и оценим «монотонность»
        def build_seq_big(raw):
            return int.from_bytes(raw, 'big')
        def build_seq_little(raw):
            return int.from_bytes(raw, 'little')

        def build_seq_masked(raw, order):
            b = raw
            part = bytes([b[self.mask_bytes[0]], b[self.mask_bytes[1]]])
            return int.from_bytes(part, order)

        seqs_big = []
        seqs_little = []

        if self.use_masked_16:
            for r in self.samples:
                seqs_big.append(build_seq_masked(r,'big'))
                seqs_little.append(build_seq_masked(r,'little'))
        else:
            for r in self.samples:
                seqs_big.append(build_seq_big(r))
                seqs_little.append(build_seq_little(r))

        def monotonic_score(seq_list):
            """
            Считает "качество" последовательности:
            +1 за нормальный маленький инкремент (1..4)
            0 за нулевой / отрицательный / слишком большой (подозрительный) скачок
            """
            good = 0
            prev = None
            for v in seq_list:
                if prev is not None:
                    d = (v - prev) & 0xFFFFFFFF
                    if 0 < d <= 4:
                        good += 1
                prev = v
            return good

        score_big = monotonic_score(seqs_big)
        score_little = monotonic_score(seqs_little)
        # debug: можно при необходимости вывести score_big/score_little
        if score_big == 0 and score_little == 0:
            # пока не сдаёмся — попробуем еще подкопить выборку
            if len(self.samples) < self.sample_size:
                return
            # окончательно признаем ненадёжным
            self.unreliable = True
            self.format_determined = True
            return

        # Выбираем лучший
        if score_big >= score_little:
            self.endian = 'big'
        else:
            self.endian = 'little'

        self.format_determined = True

    def extract_sequence(self, raw4: bytes):
        if not self.format_determined or self.unreliable:
            return None
        if self.use_masked_16:
            part = bytes([raw4[self.mask_bytes[0]], raw4[self.mask_bytes[1]]])
            seq = int.from_bytes(part, self.endian)
            return seq
        else:
            return int.from_bytes(raw4, self.endian)

# --- Packet loss helper ---
def packet_loss_delta(prev_id, curr_id, bits=32):
    """
    Вычисляет пропущенные пакеты с защитой от шумовых скачков.
    Возвращает (missed, wrapped, valid, noisy_jump)

    noisy_jump=True если скачок явно нереалистичный и должен приводить
    к временной приостановке учета потерь.
    """
    if prev_id is None:
        return 0, False, False, False

    modulus = 1 << bits
    prev_u = prev_id % modulus
    curr_u = curr_id % modulus

    if curr_u == prev_u:
        return 0, False, True, False  # дубликат

    wrapped = curr_u < prev_u
    if not wrapped:
        gap = curr_u - prev_u - 1
    else:
        gap = (modulus - prev_u) + curr_u - 1

    if gap < 0:
        return 0, wrapped, False, True

    # Нереалистичный скачок?
    if gap > MAX_ABSOLUTE_GAP:
        return 0, wrapped, False, True

    # Разумный?
    if gap <= MAX_REASONABLE_GAP:
        return gap, wrapped, True, False

    # Между MAX_REASONABLE_GAP и MAX_ABSOLUTE_GAP — считаем шумом, но отмечаем
    return 0, wrapped, False, True

class CVSDDecoder:
    """CVSD (Continuously Variable Slope Delta) декодер с исправленной логикой для HFP"""

    def __init__(self):
        # Параметры CVSD специально для Bluetooth HFP
        self.step_size = 8.0       # Еще меньший начальный шаг
        self.min_step = 2.0        # Минимальный шаг
        self.max_step = 128.0      # Максимальный шаг
        self.step_adaptation = 1.1 # Очень медленная адаптация
        self.integrator = 0.0      # Интегратор для CVSD

        # Фильтр для сглаживания
        self.history = [0.0] * 8
        self.history_index = 0

        # Счетчик одинаковых битов для slope overload
        self.same_bit_count = 0
        self.last_bit = None

    def _apply_filter(self, sample):
        """Применяем простой FIR фильтр низких частот"""
        self.history[self.history_index] = sample
        self.history_index = (self.history_index + 1) % len(self.history)

        # Простое усреднение с весами (низкочастотный фильтр)
        filtered = 0.0
        weights = [0.2, 0.15, 0.15, 0.1, 0.1, 0.1, 0.1, 0.1]
        for i, weight in enumerate(weights):
            idx = (self.history_index - 1 - i) % len(self.history)
            filtered += self.history[idx] * weight

        return filtered

    def decode(self, encoded_data):
        """Декодирование CVSD данных в PCM с правильной логикой для HFP"""
        if not encoded_data:
            return b''

        # Преобразуем данные в биты - попробуем LSB first (как в оригинале)
        bits = []
        for byte in encoded_data:
            for i in range(8):
                bits.append((byte >> i) & 1)

        output_samples = []

        for bit in bits:
            # Отслеживаем повторяющиеся биты для slope overload detection
            if self.last_bit is not None and bit == self.last_bit:
                self.same_bit_count += 1
            else:
                self.same_bit_count = 0
            self.last_bit = bit

            # CVSD алгоритм: интегратор + адаптивный шаг
            if bit == 1:
                self.integrator += self.step_size
            else:
                self.integrator -= self.step_size

            # Ограничиваем выход интегратора
            self.integrator = max(-8192, min(8192, self.integrator))

            # Применяем фильтр низких частот
            filtered_output = self._apply_filter(self.integrator)

            # Адаптация размера шага на основе slope overload
            if self.same_bit_count >= 3:  # Slope overload detected
                self.step_size = min(self.max_step, self.step_size * self.step_adaptation)
            else:
                self.step_size = max(self.min_step, self.step_size / self.step_adaptation)

            # Преобразуем в 16-битный сэмпл
            sample = int(filtered_output)
            sample = max(-32767, min(32767, sample))
            output_samples.append(sample)

        # Конвертируем в байты
        return struct.pack('<' + 'h' * len(output_samples), *output_samples)


class MSBCDecoder:
    """mSBC (modified SBC) декодер через FFmpeg"""

    def __init__(self):
        self.ffmpeg_available = self._check_ffmpeg()
        self.buffer = b''

    def _check_ffmpeg(self):
        """Проверяем наличие FFmpeg"""
        try:
            result = subprocess.run(['ffmpeg', '-version'],
                                  capture_output=True, text=True, timeout=5)
            return result.returncode == 0
        except Exception:
            return False

    def decode(self, encoded_data):
        """Декодирование mSBC данных в PCM через FFmpeg"""
        if not self.ffmpeg_available:
            print("⚠️ FFmpeg недоступен для декодирования mSBC")
            return None

        if not encoded_data:
            return b''

        # Накапливаем данные в буфере
        self.buffer += encoded_data

        # mSBC фреймы обычно 57 байт, но может варьироваться
        # Пытаемся декодировать когда накопилось достаточно данных
        if len(self.buffer) < 114:  # Минимум 2 фрейма
            return b''

        try:
            # Создаем временные файлы
            with tempfile.NamedTemporaryFile(suffix='.sbc', delete=False) as input_file:
                input_file.write(self.buffer)
                input_path = input_file.name

            with tempfile.NamedTemporaryFile(suffix='.wav', delete=False) as output_file:
                output_path = output_file.name

            # Декодируем через FFmpeg
            cmd = [
                'ffmpeg', '-y', '-f', 'sbc', '-i', input_path,
                '-f', 's16le', '-ar', '16000', '-ac', '1', output_path
            ]

            result = subprocess.run(cmd, capture_output=True, timeout=2)

            if result.returncode == 0:
                # Читаем декодированные данные
                with open(output_path, 'rb') as f:
                    decoded_data = f.read()

                # Очищаем буфер
                self.buffer = b''
                return decoded_data
            else:
                # Если декодирование не удалось, очищаем часть буфера
                self.buffer = self.buffer[57:]  # Удаляем один предполагаемый фрейм
                return b''

        except Exception as e:
            print(f"❌ Ошибка декодирования mSBC: {e}")
            self.buffer = b''
            return b''
        finally:
            # Удаляем временные файлы
            try:
                os.unlink(input_path)
                os.unlink(output_path)
            except:
                pass


class AudioServer:
    def __init__(self, host='0.0.0.0', port=12345):
        self.host = host
        self.port = port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind((self.host, self.port))
        self.packet_count = 0
        self.last_packet_id = None
        self.log_file_path = "packet_log.txt"
        self.start_time = time.time()
        self.log_interval = 20  # Интервал записи в секунду
        self.last_log_time = self.start_time

        # Очищаем файл при запуске
        with open(self.log_file_path, "w") as f:
            f.write("")

        # Регистрируем очистку при завершении
        atexit.register(self.cleanup_logs)

        # Декодеры
        self.cvsd_decoder = CVSDDecoder()
        self.msbc_decoder = MSBCDecoder()

        # Параметры аудио по умолчанию
        self.sample_rate = 8000
        self.channels = 1
        self.bits_per_sample = 16
        self.chunk_size = 120  # Уменьшили размер чанка для меньшей задержки

        self.is_msbc = False  # текущий активный кодек (False=CVSD)
        self.disable_cvsd_decode = False  # Если True, CVSD payload пропускается (для теста скорости)

        self.stream = None  # Инициализация аудио потока
        self.client_lock = threading.Lock()  # Блокировка для управления клиентами
        self.current_client = None  # Инициализация текущего клиента
        self.playback_thread = None  # Инициализация потока воспроизведения
        self.audio_queue = queue.Queue(maxsize=400)  # Увеличен размер очереди для снижения дропов
        self.log_file = open(self.log_file_path, "a")  # Инициализация файла логов

        # Новый код для нарезки аудиоданных на сегменты
        self.audio_buffer = []  # Буфер для хранения аудиоданных
        self.audio_segment_duration = 20  # Длительность сегмента в секундах
        self.last_audio_segment_time = time.time()

        # Статистика
        self.total_packets = 0  # Общее количество полученных пакетов
        self.dropped_packets = 0  # Количество сброшенных пакетов
        self.missed_packets = 0  # Количество пропущенных пакетов
        self.packet_counter = 0  # Счетчик пакетов для отслеживания общего количества

        # Буфер приема (TCP может дробить пакеты произвольно)
        self.recv_buffer = b''
        self.last_stream_seq = None
        self.expected_seq = None
        self.header_done = False  # получили ли ASCII пролог "AUDIO_STREAM"
        # self.raw_frame_mode = False      # режим приема "сырых" кадров без бинарного заголовка
        # self.frame_size = None           # размер кадра в raw_frame_mode
        # self.raw_mode_detect_window = 0  # сколько байт просмотрено при попытке sync
        self.raw_assume_pcm = False  # будем ли трактовать raw кадры как уже PCM (не CVSD)
        # Когда raw_assume_pcm=True, входные кадры считаются готовым PCM (не CVSD)
        self.accept_legacy_magic = True   # временно принимаем старый magic от прошивки

    def cleanup_logs(self):
        """Очистка логов при завершении."""
        with open(self.log_file_path, "w") as f:
            f.write("")

    def log_packet(self, packet_id, size, timestamp):
        if self.last_packet_id is not None:
            # Обработка переполнения идентификатора пакета (64-битное значение)
            if packet_id >= self.last_packet_id:
                missed = packet_id - self.last_packet_id - 1
            else:
                # Переполнение: идентификатор сбросился на 0
                missed = (packet_id + (2**64 - self.last_packet_id))

            # Убедимся, что значение пропущенных пакетов не отрицательное
            missed = max(0, missed)

            if missed > 0:
                self.missed_packets += missed
                self.log_file.write(f"WARNING: Packet loss detected! Missed {missed} packets. Expected {self.last_packet_id + 1}, got {packet_id}\n")

        self.packet_counter += 1
        self.log_file.write(f"Packet {packet_id}: size={size}, timestamp={timestamp}\n")
        self.last_packet_id = packet_id

    def log_packet_with_delay(self, packet_id, size, timestamp, delay):
        """Логирование пакета с задержкой"""
        if self.last_packet_id is not None and packet_id != self.last_packet_id + 1:
            self.log_file.write(f"WARNING: Packet loss detected! Expected {self.last_packet_id + 1}, got {packet_id}\n")
        self.log_file.write(f"Packet {packet_id}: size={size}, timestamp={timestamp}, delay={delay:.3f}s\n")
        self.last_packet_id = packet_id

    def start(self):
        """Запуск сервера"""
        try:
            # Инициализация PyAudio
            self.audio = pyaudio.PyAudio()

            # Создание сокета
            self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.socket.bind((self.host, self.port))
            self.socket.listen(1)

            print(f"🎵 Аудио сервер запущен на {self.host}:{self.port}")
            print("Ожидание подключения ESP32... (ASCII 'AUDIO_STREAM' + binary header или авто RAW fallback)")

            self.running = True

            while self.running:
                try:
                    client_socket, address = self.socket.accept()
                    print(f"📱 Подключение от {address}")

                    # Обрабатываем клиента в отдельном потоке
                    client_thread = threading.Thread(
                        target=self.handle_client,
                        args=(client_socket, address)
                    )
                    client_thread.daemon = True
                    client_thread.start()

                except socket.error as e:
                    if self.running:
                        print(f"❌ Ошибка сокета: {e}")

        except Exception as e:
            print(f"❌ Ошибка запуска сервера: {e}")
        finally:
            self.cleanup()

    def handle_client(self, client_socket, address):
        """Обработка подключенного клиента"""
        try:
            with self.client_lock:
                # Если уже есть подключенный клиент, закрываем его
                if self.current_client and self.current_client != client_socket:
                    print("🔌 Отключение предыдущего клиента")
                    self.current_client.close()

                # Обновляем текущего клиента
                self.current_client = client_socket

            # Запускаем воспроизведение в отдельном потоке
            if not self.playback_thread or not self.playback_thread.is_alive():
                self.playback_thread = threading.Thread(target=self.audio_playback_thread)
                self.playback_thread.daemon = True
                self.playback_thread.start()

            # Основной цикл приема данных
            self.receive_audio_data(client_socket)

        except Exception as e:
            print(f"❌ Ошибка обработки клиента {address}: {e}")
        finally:
            with self.client_lock:
                if client_socket == self.current_client:
                    self.current_client = None
            client_socket.close()
            print(f"📱 Отключение {address}")

    # read_header and parse_header removed: protocol is now binary only

    def init_audio_stream(self):
        """Инициализация аудио потока для воспроизведения"""
        try:
            if self.stream:
                self.stream.close()

            format_map = {
                8: pyaudio.paInt8,
                16: pyaudio.paInt16,
                24: pyaudio.paInt24,
                32: pyaudio.paInt32
            }

            audio_format = format_map.get(self.bits_per_sample, pyaudio.paInt16)

            self.stream = self.audio.open(
                format=audio_format,
                channels=self.channels,
                rate=self.sample_rate,
                output=True,
                frames_per_buffer=self.chunk_size
            )

            print("🔊 Аудио поток для воспроизведения инициализирован")

        except Exception as e:
            print(f"❌ Ошибка инициализации аудио потока: {e}")

    def periodic_log(self):
        """Периодическая запись в файл каждые 20 секунд."""
        current_time = time.time()
        if current_time - self.last_log_time >= self.log_interval:
            with open(self.log_file_path, "a") as f:
                f.write(f"Periodic log at {time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(current_time))}\n")
            self.last_log_time = current_time

    def save_audio_segment(self):
        """Сохраняет текущий буфер аудиоданных в файл и очищает буфер."""
        if not self.audio_buffer:
            return

        segment_filename = f"audio_segment_{int(time.time())}.wav"
        with wave.open(segment_filename, 'wb') as wf:
            wf.setnchannels(self.channels)
            wf.setsampwidth(pyaudio.get_sample_size(pyaudio.paInt16))
            wf.setframerate(self.sample_rate)
            wf.writeframes(b''.join(self.audio_buffer))

        print(f"💾 Аудиосегмент сохранён: {segment_filename}")
        self.audio_buffer = []

    def receive_audio_data(self, client_socket):
        """Получение аудиоданных от клиента."""
        print("🎤 Начало приема аудиоданных...")

        while self.running:
            try:
                chunk = client_socket.recv(4096)
                if not chunk:
                    print("📡 Соединение закрыто клиентом")
                    break
                self.recv_buffer += chunk

                # Если ещё не обработан ASCII пролог — пытаемся его выделить
                if not self.header_done:
                    sep_pos = self.recv_buffer.find(b"\n\n")
                    if sep_pos != -1:
                        header_blob = self.recv_buffer[:sep_pos+2]
                        self.recv_buffer = self.recv_buffer[sep_pos+2:]
                        try:
                            header_txt = header_blob.decode(errors='ignore')
                            if header_txt.startswith("AUDIO_STREAM"):
                                # Парсим параметры
                                sample_rate = self.sample_rate
                                channels = self.channels
                                bits = self.bits_per_sample
                                codec_name = None
                                for line in header_txt.splitlines():
                                    if '=' in line:
                                        k,v = line.split('=',1)
                                        if k == 'sample_rate':
                                            sample_rate = int(v)
                                        elif k == 'channels':
                                            channels = int(v)
                                        elif k == 'bits_per_sample':
                                            bits = int(v)
                                        elif k == 'codec':
                                            codec_name = v.strip().upper()
                                self.sample_rate = sample_rate
                                self.channels = channels
                                self.bits_per_sample = bits
                                if codec_name == 'MSBC':
                                    self.is_msbc = True
                                    self.sample_rate = 16000
                                else:
                                    self.is_msbc = False
                                print(f"📄 Получен ASCII заголовок: {sample_rate}Hz {channels}ch {bits}bit codec={codec_name or 'UNKNOWN'}")
                                self.header_done = True
                                # Поток создадим позже (после первого декодированного пакета) — лениво
                            else:
                                print("ℹ️ Получен текст до бинарных пакетов, но не 'AUDIO_STREAM' — пропускаем")
                                self.header_done = True  # чтобы не застрять
                        except Exception as e:
                            print(f"⚠️ Ошибка парсинга заголовка: {e}")
                            self.header_done = True  # не повторять
                    else:
                        # ждём пока придёт полный заголовок или сразу начнётся бинарный поток
                        # ограничим размер ожидаемого пролога
                        if len(self.recv_buffer) > 2048:
                            print("⚠️ Слишком длинный пролог без разделителя, сбрасываю")
                            self.recv_buffer = b''
                        continue
                # После обработки (или отсутствия) заголовка синхронизируемся по magic
                if self.last_stream_seq is None:
                    # Строгий поиск нового или legacy magic – без raw fallback
                    magic_new_le = STREAM_MAGIC_NEW.to_bytes(4, 'little')
                    magic_old_le = STREAM_MAGIC_OLD.to_bytes(4, 'little')
                    pos_new = self.recv_buffer.find(magic_new_le)
                    pos_old = self.recv_buffer.find(magic_old_le) if self.accept_legacy_magic else -1

                    chosen_magic = None
                    pos = -1
                    if pos_new != -1 and (pos_old == -1 or pos_new < pos_old):
                        chosen_magic = STREAM_MAGIC_NEW
                        pos = pos_new
                    elif pos_old != -1:
                        chosen_magic = STREAM_MAGIC_OLD
                        pos = pos_old

                    if chosen_magic is None:
                        # ограничиваем рост буфера
                        if len(self.recv_buffer) > 4096:
                            self.recv_buffer = self.recv_buffer[-4096:]
                        continue
                    if pos > 0:
                        print(f"ℹ️ Отбрасываем {pos} байт до первого magic (возможно остаток ASCII заголовка)")
                        self.recv_buffer = self.recv_buffer[pos:]
                    # Зафиксируем используемый magic
                    if chosen_magic == STREAM_MAGIC_OLD:
                        print("⚠️ Используется legacy magic от прошивки (0x41554448). После обновления прошивки будет ожидаться новый 0x48445541.")
                    else:
                        print("✅ Найден новый magic (0x48445541).")
                        self.accept_legacy_magic = False
                    # Продолжаем – дальнейший разбор произойдет ниже в цикле пакетов

                # Разбираем буфер: header + payload
                while True:
                    if len(self.recv_buffer) < STREAM_HEADER_SIZE:
                        break  # ждём больше данных

                    # Peek header
                    header_bytes = self.recv_buffer[:STREAM_HEADER_SIZE]
                    magic, seq, ts_us, payload_len, codec = STREAM_HEADER_STRUCT.unpack(header_bytes)

                    if self.last_stream_seq is None:
                        print(f"✅ Синхронизация: magic=0x{magic:08X} bytes=[{magic_bytes_le(magic)}]")
                        # После первой синхронизации считаем, что payload CVSD = PCM
                        self.raw_assume_pcm = True

                    if not (magic == STREAM_MAGIC_NEW or (self.accept_legacy_magic and magic == STREAM_MAGIC_OLD)):
                        # Попытка ресинхронизации: сдвигаем на 1 байт
                        print(f"⚠️ Desync (magic=0x{magic:08X} != (0x{STREAM_MAGIC_NEW:08X} / 0x{STREAM_MAGIC_OLD:08X})), resync...")
                        self.recv_buffer = self.recv_buffer[1:]
                        continue

                    total_needed = STREAM_HEADER_SIZE + payload_len
                    if len(self.recv_buffer) < total_needed:
                        # ждём оставшиеся байты payload
                        break

                    # Извлекаем пакет целиком
                    packet = self.recv_buffer[:total_needed]
                    self.recv_buffer = self.recv_buffer[total_needed:]
                    if payload_len == 0 or payload_len > 1024:
                        print(f"⚠️ Нереалистичный payload_len={payload_len}, пропуск пакета")
                        continue

                    # Увеличиваем счетчик пакетов
                    self.total_packets += 1
                    self.packet_counter += 1

                    # Потери (32-битный seq, wrap учитывается)
                    if self.last_stream_seq is not None:
                        expected = (self.last_stream_seq + 1) & 0xFFFFFFFF
                        if seq != expected:
                            # вычислим пропуск
                            diff = (seq - expected) & 0xFFFFFFFF
                            if diff != 0:
                                self.missed_packets += diff
                                print(f"⚠️ Пропуск {diff} пакетов (expected={expected} got={seq})")
                    self.last_stream_seq = seq

                    # Задержка (прием - метка отправителя)
                    arrival_us = int(time.time() * 1_000_000)
                    raw_delta_us = arrival_us - ts_us
                    if raw_delta_us < 0 or raw_delta_us > 120_000_000:  # >120s считаем неверной меткой (например из-за несовместимых эпох)
                        net_delay_ms = -1.0
                    else:
                        net_delay_ms = raw_delta_us / 1000.0
                    if self.total_packets % 200 == 1:
                        if net_delay_ms >= 0:
                            print(f"🕒 seq={seq} delay={net_delay_ms:.1f}ms payload={payload_len} codec={codec}")
                        else:
                            print(f"🕒 seq={seq} delay=? payload={payload_len} codec={codec}")

                    # Проверка кодека (если поменялся — перенастроим аудио при необходимости)
                    if codec == STREAM_CODEC_MSBC and not getattr(self, 'is_msbc', False):
                        print("🔁 Переключение в mSBC (16 kHz)")
                        self.is_msbc = True
                        self.sample_rate = 16000
                        self.init_audio_stream()
                    elif codec == STREAM_CODEC_CVSD and getattr(self, 'is_msbc', False):
                        print("🔁 Переключение в CVSD (8 kHz)")
                        self.is_msbc = False
                        self.sample_rate = 8000
                        self.init_audio_stream()

                    payload = packet[STREAM_HEADER_SIZE:]
                    # Трактуем CVSD payload как уже готовый PCM (ESP шлет PCM 16-bit)
                    if codec == STREAM_CODEC_CVSD:
                        decoded_pcm = payload
                    elif codec == STREAM_CODEC_MSBC:
                        decoded_pcm = self.msbc_decoder.decode(payload)
                    else:
                        decoded_pcm = None
                    if not decoded_pcm:
                        continue
                    if self.stream is None:
                        # Инициализируем поток исходя из текущих параметров (sample_rate может обновляться при переключении кодека)
                        self.init_audio_stream()
                    processed = self.process_audio_data(decoded_pcm, seq=seq)
                    if processed is None:
                        continue
                    try:
                        self.audio_queue.put_nowait(processed)
                    except queue.Full:
                        self.dropped_packets += 1
                        if (self.dropped_packets % 50) == 1:
                            print(f"⚠️ Очередь переполнена: сброшено {self.dropped_packets} пакетов")

            except Exception as e:
                print(f"❌ Ошибка приема данных: {e}")
                break

    def process_audio_data(self, pcm_bytes, seq=None):
        """Обработка уже декодированных PCM s16le аудио данных"""
        try:
            if not pcm_bytes:
                return None
            audio_array = np.frombuffer(pcm_bytes, dtype=np.int16)
            if audio_array.size == 0:
                return None

            # Если очередь близка к заполнению, пропускаем AGC чтобы ускориться
            if self.audio_queue.qsize() > (self.audio_queue.maxsize * 0.85):
                return pcm_bytes

            # self.packet_counter += 1  # Удалено: теперь счетчик инкрементируется только в receive_audio_data
            max_val = int(np.max(np.abs(audio_array)))
            avg_val = float(np.mean(np.abs(audio_array)))
            rms_val = float(np.sqrt(np.mean(audio_array.astype(np.float32) ** 2)))

            # Диагностика пустых / почти пустых пакетов
            if max_val == 0 and avg_val == 0.0:
                if (self.packet_counter % 20) == 1:
                    if seq is not None:
                        print(f"⚠️ Пустой аудио пакет #{self.packet_counter} seq={seq}")
                    else:
                        print(f"⚠️ Пустой аудио пакет #{self.packet_counter}")
                self.log_file.write(f"[WARN] Пустой аудио пакет #{self.packet_counter}\n")
                self.log_file.flush()

            # Предупреждение о клиппинге
            if max_val > 32000:
                if seq is not None:
                    print(f"⚠️ Аномально громкий пакет #{self.packet_counter}: seq={seq} max={max_val}")
                else:
                    print(f"⚠️ Аномально громкий пакет #{self.packet_counter}: max={max_val}")
                self.log_file.write(f"[WARN] Loud packet #{self.packet_counter} max={max_val}\n")
                self.log_file.flush()

            # Легкая нормализация (AGC)
            if 0 < rms_val < 800:
                gain = min(4.0, 500.0 / rms_val)
                audio_array = np.clip(audio_array.astype(np.float32) * gain, -32768, 32767).astype(np.int16)

            return audio_array.tobytes()
        except Exception as e:
            print(f"❌ Ошибка обработки PCM: {e}")
            return None

    def audio_playback_thread(self):
        """Поток воспроизведения аудио"""
        print("🔊 Поток воспроизведения запущен")

        while self.running:
            try:
                with self.client_lock:
                    # Если нет текущего клиента, ждем
                    if not self.current_client:
                        time.sleep(0.1)
                        continue

                # Получаем данные из очереди
                audio_data = self.audio_queue.get(timeout=1.0)

                # Воспроизводим
                if self.stream:
                    self.stream.write(audio_data)

            except queue.Empty:
                continue
            except Exception as e:
                print(f"❌ Ошибка воспроизведения: {e}")
                time.sleep(0.1)

    def stop(self):
        """Остановка сервера"""
        print(f"\n📊 Статистика: получено={self.total_packets}, сброшено={self.dropped_packets}, пропущено={self.missed_packets}")
        self.running = False

    def cleanup(self):
        """Очистка ресурсов"""
        if self.stream:
            self.stream.stop_stream()
            self.stream.close()

        if self.audio:
            self.audio.terminate()

        if self.socket:
            self.socket.close()

        print("✅ Сервер остановлен")

def main():
    parser = argparse.ArgumentParser(description='Аудио сервер для ESP32')
    parser.add_argument('--host', default='0.0.0.0', help='IP адрес сервера')
    parser.add_argument('--port', type=int, default=8888, help='Порт сервера')

    args = parser.parse_args()

    # Проверяем наличие PyAudio
    try:
        import pyaudio
    except ImportError:
        print("❌ PyAudio не установлен. Установите: pip install pyaudio")
        sys.exit(1)

    server = AudioServer(args.host, args.port)

    try:
        server.start()
    except KeyboardInterrupt:
        server.stop()

if __name__ == '__main__':
    main()
