#!/bin/bash
set -e

RESULTS="${1:-results.csv}"
DURATION="${2:-10}"
CHURN="${3:-2}"
DEVICE="${4:-""}"

DEVICE_FLAG=""
if [ -n "$DEVICE" ]; then
    DEVICE_FLAG="-D $DEVICE"
fi

echo "================================================================="
echo "  Hard Real-Time Audio DSP Testbed - Automated Benchmark Sweep   "
echo "================================================================="
echo "Target Platform : Linux (ALSA backend)"
echo "Output File     : $RESULTS"
echo "Run Duration    : ${DURATION}s per test"
echo "Churn Workers   : $CHURN (SCHED_IDLE + nice = +19 background priority)"
echo "Presets to Test : adv0 adv1 adv3 adv10"
echo "Allocs / Frame  : 4 8 16"
if [ -n "$DEVICE" ]; then
    echo "Audio Device ID : $DEVICE (Hardware Override Active)"
else
    echo "Audio Device ID : System Default"
fi
echo "================================================================="

PRESETS=("adv0" "adv1" "adv3" "adv10")
ALLOC_COUNTS=(4 8 16)

TOTAL_RUNS=$((${#PRESETS[@]} * ${#ALLOC_COUNTS[@]}))
RUN_INDEX=0

for PRESET in "${PRESETS[@]}"; do
    for ALLOCS in "${ALLOC_COUNTS[@]}"; do
        RUN_INDEX=$((RUN_INDEX + 1))
        echo ""
        echo ">> ========================================================="
        echo ">> [$RUN_INDEX/$TOTAL_RUNS] Preset: $PRESET | Allocs/Callback: $ALLOCS [glibc malloc]"
        echo ">> ========================================================="
        
        ./build/audio_testbed $DEVICE_FLAG -p "$PRESET" -d "$DURATION" -c "$CHURN" -a "$ALLOCS" -f "$RESULTS"
        sleep 2
    done
done

echo ""
echo "================================================================="
echo ">> All benchmarks completed successfully!"
echo ">> Telemetry results saved to: $RESULTS"
echo "================================================================="
