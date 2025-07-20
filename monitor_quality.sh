#!/bin/bash

# Real-time audio quality monitor
# Мониторинг качества аудио в реальном времени

echo "🔍 Audio Quality Monitor"
echo "========================"

# Monitor for new segment files and analyze them
monitor_segments() {
    echo "📁 Monitoring for new audio segments..."

    while true; do
        # Find newest segment file
        NEWEST=$(ls -t segment_*.wav 2>/dev/null | head -1)

        if [ -n "$NEWEST" ] && [ "$NEWEST" != "$LAST_SEGMENT" ]; then
            echo ""
            echo "🎵 New segment detected: $NEWEST"

            # Basic file info
            if command -v ffprobe >/dev/null 2>&1; then
                echo "📊 File info:"
                ffprobe -v quiet -show_entries format=duration,size -of csv=p=0 "$NEWEST" | while IFS=, read duration size; do
                    echo "   Duration: ${duration}s"
                    echo "   Size: ${size} bytes"
                done
            fi

            # Check for audio quality issues
            if command -v sox >/dev/null 2>&1; then
                echo "🔍 Audio analysis:"
                sox "$NEWEST" -n stat 2>&1 | grep -E "(RMS|Maximum|Minimum)" | head -3
            else
                echo "💡 Install sox for detailed audio analysis: brew install sox"
            fi

            LAST_SEGMENT="$NEWEST"
        fi

        sleep 2
    done
}

# Monitor packet log for quality metrics
monitor_packets() {
    echo "📈 Monitoring packet quality..."

    if [ -f "packet_log.csv" ]; then
        tail -f packet_log.csv | while IFS=, read time seq expected gap event delta jitter lost dup reorder qsize underrun buffer esp32_ts; do
            if [ "$event" = "GAP" ] && [ "$gap" -gt 0 ]; then
                echo "⚠️  GAP detected: $gap packets at seq=$seq (buffer=${buffer}ms)"
            elif [ "$event" = "UNDERRUN" ]; then
                echo "🪫 UNDERRUN at seq=$seq (buffer=${buffer}ms)"
            fi
        done
    fi
}

# Show usage
echo "🚀 Starting quality monitors..."
echo "💡 This will monitor:"
echo "   - New audio segment files"
echo "   - Packet gaps and underruns"
echo "   - Buffer health metrics"
echo ""

# Run monitors in background
monitor_segments &
SEGMENTS_PID=$!

monitor_packets &
PACKETS_PID=$!

# Cleanup on exit
trap "kill $SEGMENTS_PID $PACKETS_PID 2>/dev/null" EXIT

echo "📊 Monitors started (Press Ctrl+C to stop)"
echo "=========================================="

# Keep script running
wait
