#!/bin/bash


set -e

# --- Проверка: запускается ли скрипт через 'source' ---
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
  echo "❌ Please run this script using 'source ./py.sh' to properly export environment variables"
  exit 1
fi

echo "🔄 Checking environment..."

# --- Деактивируем внешний venv, если он активен ---
if [[ -n "$VIRTUAL_ENV" ]]; then
  echo "⚠️  Deactivating external virtual environment: $VIRTUAL_ENV"
  deactivate || true
fi

# --- Настройки путей ---
export IDF_PATH="$HOME/esp/esp-idf"
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.2_py3.13_env"

# --- Устанавливаем certifi, если не установлен ---
if ! "$IDF_PYTHON_ENV_PATH/bin/python" -m pip show certifi >/dev/null 2>&1; then
  echo "📦 Installing certifi into $IDF_PYTHON_ENV_PATH"
  "$IDF_PYTHON_ENV_PATH/bin/python" -m pip install --trusted-host pypi.org --trusted-host files.pythonhosted.org certifi || {
    echo "❌ Failed to install certifi"
    exit 1
  }
fi


# --- Устанавливаем packaging, если не установлен ---
if ! "$IDF_PYTHON_ENV_PATH/bin/python" -m pip show packaging >/dev/null 2>&1; then
  echo "📦 Installing packaging into $IDF_PYTHON_ENV_PATH"
  "$IDF_PYTHON_ENV_PATH/bin/python" -m pip install --trusted-host pypi.org --trusted-host files.pythonhosted.org packaging || {
    echo "❌ Failed to install packaging"
    exit 1
  }
fi

# --- Устанавливаем все необходимые зависимости ESP-IDF ---
echo "📦 Installing required ESP-IDF Python packages..."
SSL_CERT_FILE="$("$IDF_PYTHON_ENV_PATH/bin/python" -m certifi)" \
REQUESTS_CA_BUNDLE="$("$IDF_PYTHON_ENV_PATH/bin/python" -m certifi)" \
"$IDF_PATH/tools/idf_tools.py" install-python-env || {
  echo "❌ Failed to install full ESP-IDF Python environment"
  exit 1
}

# --- Устанавливаем SSL_CERT_FILE заново (после certifi) ---
export SSL_CERT_FILE="$("$IDF_PYTHON_ENV_PATH/bin/python" -m certifi)"

# --- Проверка наличия окружения ---
if [[ ! -x "$IDF_PYTHON_ENV_PATH/bin/python" ]]; then
  echo "⚠️  Python environment not found at $IDF_PYTHON_ENV_PATH"
  echo "📦 Installing Python environment using idf_tools.py..."

  "$IDF_PATH"/tools/idf_tools.py install-python-env || {
    echo "❌ Failed to install Python environment"
    exit 1
  }

  echo "✅ Python environment installed"
fi

# --- Подгружаем переменные из ESP-IDF ---
echo "✅ Sourcing IDF environment from $IDF_PATH"
. "$IDF_PATH/export.sh"

# --- Переходим в папку проекта (если запущено откуда-то ещё) ---
cd "$(dirname "$0")"

# --- Действия по умолчанию ---
idf.py "$@"