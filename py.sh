#!/bin/bash

# Убираем set -e, чтобы скрипт не завершался при ошибках
# set -e

# Функция для обработки ошибок
handle_error() {
    local exit_code=$?
    local line_no=$1
    echo ""
    echo "❌ Ошибка на строке $line_no (код выхода: $exit_code)"
    echo "⚠️  Нажмите Enter для продолжения или Ctrl+C для выхода..."
    read -r
    return $exit_code
}

# Устанавливаем обработчик ошибок
trap 'handle_error $LINENO' ERR

# --- Проверка: запускается ли скрипт через 'source' ---
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
  echo "❌ Please run this script using 'source ./py.sh' to properly export environment variables"
  echo "⚠️  Нажмите Enter для продолжения..."
  read -r
  exit 1
fi

echo "🔄 Checking environment..."

# --- Деактивируем внешний venv, если он активен ---
if [[ -n "$VIRTUAL_ENV" ]]; then
  echo "⚠️  Deactivating external virtual environment: $VIRTUAL_ENV"
  deactivate 2>/dev/null || true
fi

# --- Настройки путей ---
export IDF_PATH="$HOME/esp/esp-idf"
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.2_py3.13_env"

# --- Проверяем существование ESP-IDF ---
if [[ ! -d "$IDF_PATH" ]]; then
  echo "❌ ESP-IDF не найден в $IDF_PATH"
  echo "💡 Убедитесь, что ESP-IDF установлен правильно"
  echo "⚠️  Нажмите Enter для продолжения..."
  read -r
  return 1
fi

# --- Устанавливаем certifi, если не установлен ---
echo "📦 Checking certifi installation..."
if ! "$IDF_PYTHON_ENV_PATH/bin/python" -m pip show certifi >/dev/null 2>&1; then
  echo "📦 Installing certifi into $IDF_PYTHON_ENV_PATH"
  if ! "$IDF_PYTHON_ENV_PATH/bin/python" -m pip install --trusted-host pypi.org --trusted-host files.pythonhosted.org certifi; then
    echo "❌ Failed to install certifi"
    echo "⚠️  Продолжаем без certifi. Нажмите Enter..."
    read -r
  else
    echo "✅ certifi installed successfully"
  fi
else
  echo "✅ certifi already installed"
fi

# --- Устанавливаем packaging, если не установлен ---
echo "📦 Checking packaging installation..."
if ! "$IDF_PYTHON_ENV_PATH/bin/python" -m pip show packaging >/dev/null 2>&1; then
  echo "📦 Installing packaging into $IDF_PYTHON_ENV_PATH"
  if ! "$IDF_PYTHON_ENV_PATH/bin/python" -m pip install --trusted-host pypi.org --trusted-host files.pythonhosted.org packaging; then
    echo "❌ Failed to install packaging"
    echo "⚠️  Продолжаем без packaging. Нажмите Enter..."
    read -r
  else
    echo "✅ packaging installed successfully"
  fi
else
  echo "✅ packaging already installed"
fi

# --- Устанавливаем все необходимые зависимости ESP-IDF ---
echo "📦 Installing required ESP-IDF Python packages..."
if command -v "$IDF_PYTHON_ENV_PATH/bin/python" >/dev/null 2>&1; then
  SSL_CERT_FILE="$("$IDF_PYTHON_ENV_PATH/bin/python" -m certifi 2>/dev/null || echo '')" \
  REQUESTS_CA_BUNDLE="$("$IDF_PYTHON_ENV_PATH/bin/python" -m certifi 2>/dev/null || echo '')" \
  "$IDF_PATH/tools/idf_tools.py" install-python-env || {
    echo "❌ Failed to install full ESP-IDF Python environment"
    echo "⚠️  Продолжаем с имеющимся окружением. Нажмите Enter..."
    read -r
  }
fi

# --- Устанавливаем SSL_CERT_FILE заново (после certifi) ---
if command -v "$IDF_PYTHON_ENV_PATH/bin/python" >/dev/null 2>&1; then
  export SSL_CERT_FILE="$("$IDF_PYTHON_ENV_PATH/bin/python" -m certifi 2>/dev/null || echo '')"
fi

# --- Проверка наличия окружения ---
if [[ ! -x "$IDF_PYTHON_ENV_PATH/bin/python" ]]; then
  echo "⚠️  Python environment not found at $IDF_PYTHON_ENV_PATH"
  echo "📦 Installing Python environment using idf_tools.py..."

  if ! "$IDF_PATH"/tools/idf_tools.py install-python-env; then
    echo "❌ Failed to install Python environment"
    echo "⚠️  Продолжаем без Python окружения. Нажмите Enter..."
    read -r
  else
    echo "✅ Python environment installed"
  fi
fi

# --- Подгружаем переменные из ESP-IDF ---
echo "✅ Sourcing IDF environment from $IDF_PATH"
if ! . "$IDF_PATH/export.sh"; then
  echo "❌ Failed to source ESP-IDF environment"
  echo "⚠️  Проверьте установку ESP-IDF. Нажмите Enter для продолжения..."
  read -r
fi

# --- Переходим в папку проекта (если запущено откуда-то ещё) ---
PROJECT_DIR="$(dirname "${BASH_SOURCE[0]}")"
if ! cd "$PROJECT_DIR"; then
  echo "❌ Failed to change to project directory: $PROJECT_DIR"
  echo "⚠️  Нажмите Enter для продолжения..."
  read -r
fi

# --- Проверяем наличие основных файлов проекта ---
if [[ ! -f "CMakeLists.txt" ]]; then
  echo "⚠️  CMakeLists.txt не найден в текущей директории"
  echo "💡 Убедитесь, что вы находитесь в корне проекта ESP32"
fi

if [[ $# -gt 0 ]]; then
  echo "⚠️  Autostart of idf.py removed to keep shell interactive."
  echo "👉 Run manually: idf.py $*"
fi

echo ""
echo "✅ IDF environment ready. Examples:"
echo "   idf.py build"
echo "   idf.py flash monitor"
echo "   idf.py -p /dev/ttyUSB0 flash monitor"
echo "   idf.py menuconfig"
echo ""
echo "📋 Autostart commands available:"
echo "   autostart_load_default  # Load your WiFi/stream commands"
echo "   autostart_show         # Show current autostart config"
echo "   autostart_enable 1     # Enable autostart on boot"
echo ""
echo "💡 Если возникли ошибки выше, попробуйте:"
echo "   - Переустановить ESP-IDF"
echo "   - Проверить интернет-соединение"
echo "   - Запустить: $IDF_PATH/install.sh"
