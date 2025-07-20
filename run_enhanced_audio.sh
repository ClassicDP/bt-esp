#!/bin/bash

# Enhanced audio server launcher with optimized parameters for reduced gaps and clicks
# Оптимизированные параметры для уменьшения гэпов и щелчков

echo "🎵 Starting enhanced audio server with anti-gap optimizations..."
echo "📊 Buffer settings: min=25ms, target=75ms, max=200ms"
echo "🔧 Enhanced concealment and fast start (50ms prebuffer)"

python3 audio_server.py \
    --host 0.0.0.0 \
    --port 8888 \
    --prebuffer-ms 50 \
    --min-buffer-ms 25 \
    --max-buffer-ms 200

echo "✅ Audio server stopped"
