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
    def __init__(self, host='0.0.0.0', port=8888):
        self.host = host
        self.port = port
        self.socket = None
        self.running = False
        self.audio = None
        self.stream = None
        self.audio_queue = queue.Queue(maxsize=200)
        self.playback_thread = None
        self.current_client = None
        self.client_lock = threading.Lock()

        # Декодеры
        self.cvsd_decoder = CVSDDecoder()
        self.msbc_decoder = MSBCDecoder()

        # Параметры аудио по умолчанию
        self.sample_rate = 8000
        self.channels = 1
        self.bits_per_sample = 16
        self.chunk_size = 512  # Уменьшили размер чанка для меньшей задержки

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
            print("Ожидание подключения ESP32...")

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
            # Читаем заголовок
            header = self.read_header(client_socket)
            if header:
                self.parse_header(header)
                print(f"🔧 Параметры аудио: {self.sample_rate}Hz, {self.channels}ch, {self.bits_per_sample}bit")

                # Инициализируем аудио поток для воспроизведения
                self.init_audio_stream()

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

    def read_header(self, client_socket):
        """Чтение заголовка с параметрами потока"""
        try:
            header = b""
            while b"\n\n" not in header:
                data = client_socket.recv(1)
                if not data:
                    return None
                header += data

                # Защита от слишком длинного заголовка
                if len(header) > 1024:
                    print("❌ Слишком длинный заголовок")
                    return None

            return header.decode('utf-8')
        except Exception as e:
            print(f"❌ Ошибка чтения заголовка: {e}")
            return None

    def parse_header(self, header):
        """Парсинг заголовка с параметрами аудио"""
        lines = header.strip().split('\n')

        for line in lines:
            if '=' in line:
                key, value = line.split('=', 1)
                if key == 'sample_rate':
                    self.sample_rate = int(value)
                elif key == 'channels':
                    self.channels = int(value)
                elif key == 'bits_per_sample':
                    self.bits_per_sample = int(value)
                elif key == 'codec':
                    self.codec_type = value

        # Определяем кодек на основе заголовка или частоты дискретизации
        if hasattr(self, 'codec_type'):
            if self.codec_type.upper() == 'MSBC':
                print("🔧 Обнаружен mSBC кодек из заголовка")
                self.is_msbc = True
                self.sample_rate = 16000  # mSBC всегда 16 кГц
            else:
                print(f"🔧 Обнаружен кодек {self.codec_type}")
                self.is_msbc = False
        else:
            # Фолбэк: определяем по частоте дискретизации
            if self.sample_rate == 16000:
                print("🔧 Обнаружен mSBC кодек (16 кГц)")
                self.is_msbc = True
            else:
                print("🔧 Обнаружен CVSD кодек (8 кГц)")
                self.is_msbc = False

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

    def receive_audio_data(self, client_socket):
        """Получение аудиоданных от клиента"""
        print("🎤 Начало приема аудиоданных...")
        dropped_packets = 0
        total_packets = 0

        while self.running:
            try:
                # Читаем данные
                data = client_socket.recv(4096)
                if not data:
                    print("📡 Соединение закрыто клиентом")
                    break

                total_packets += 1

                # Проверяем формат данных и конвертируем при необходимости
                processed_data = self.process_audio_data(data)
                if processed_data is None:
                    continue

                # Добавляем в очередь для воспроизведения
                try:
                    self.audio_queue.put_nowait(processed_data)
                except queue.Full:
                    # Очередь переполнена, очищаем старые данные более агрессивно
                    cleared = 0
                    try:
                        # Очищаем несколько старых пакетов сразу для освобождения места
                        while cleared < 10 and not self.audio_queue.empty():
                            self.audio_queue.get_nowait()
                            cleared += 1

                        # Добавляем новый пакет
                        self.audio_queue.put_nowait(processed_data)
                        dropped_packets += 1

                        # Печатаем статистику каждые 100 сброшенных пакетов
                        if dropped_packets % 100 == 0:
                            drop_rate = (dropped_packets / total_packets) * 100
                            print(f"⚠️ Очередь переполнена: сброшено {dropped_packets} из {total_packets} пакетов ({drop_rate:.1f}%)")
                    except queue.Empty:
                        # Очередь уже пуста, просто добавляем новый пакет
                        try:
                            self.audio_queue.put_nowait(processed_data)
                        except queue.Full:
                            # Все еще переполнена, пропускаем этот пакет
                            dropped_packets += 1

            except socket.timeout:
                continue
            except Exception as e:
                print(f"❌ Ошибка приема данных: {e}")
                break

    def process_audio_data(self, data):
        """Обработка и конвертация аудио данных"""
        try:
            # ESP32 с CONFIG_BT_HFP_AUDIO_DATA_PATH_HCI отправляет уже декодированные PCM данные
            # Не нужно их дополнительно декодировать через CVSD или mSBC

            # Проверяем, что размер данных кратен размеру сэмпла (16-bit mono = 2 байта)
            sample_size = 2  # 16-bit mono
            if len(data) % sample_size != 0:
                # Обрезаем данные до кратного размера
                trimmed_size = (len(data) // sample_size) * sample_size
                data = data[:trimmed_size]
                if trimmed_size == 0:
                    return None

            # Интерпретируем как 16-битные signed integers
            audio_array = np.frombuffer(data, dtype=np.int16)

            if len(audio_array) > 0:
                # Показываем статистику каждые 100 пакетов
                if not hasattr(self, 'packet_counter'):
                    self.packet_counter = 0
                self.packet_counter += 1

                if self.packet_counter % 100 == 1:  # Первый и каждый 100-й пакет
                    max_val = np.max(np.abs(audio_array))
                    avg_val = np.mean(np.abs(audio_array))
                    rms_val = np.sqrt(np.mean(audio_array.astype(np.float32) ** 2))
                    codec_type = "mSBC" if self.sample_rate == 16000 else "CVSD"
                    print(f"🔊 {codec_type} пакет #{self.packet_counter}: сэмплы={len(audio_array)}, макс={max_val}, сред={avg_val:.1f}, RMS={rms_val:.1f}")

                # Применяем небольшое усиление для лучшей слышимости
                audio_array = np.clip(audio_array * 2, -32768, 32767).astype(np.int16)
                return audio_array.tobytes()
            else:
                return None

        except Exception as e:
            print(f"❌ Ошибка обработки аудио данных: {e}")
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
        print("\n🛑 Остановка сервера...")
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
