#!/bin/bash
set -e

RESULTS="results.csv"
DURATION=${1:-10}
ITERATIONS=${2:-3}
CHURN=${3:-2}
ALLOCS=${4:-4}

echo "================================================================="
echo "  Hard Real-Time Audio DSP Testbed - Automated Benchmark Sweep   "
echo "================================================================="
echo "Target Platform : elpatr0n (EndeavourOS / ALSA)"
echo "Output File     : $RESULTS"
echo "Run Duration    : ${DURATION}s per test"
echo "Iterations      : $ITERATIONS per preset"
echo "Churn Workers   : $CHURN"
echo "Allocs / Frame  : $ALLOCS"
echo "================================================================="

# Array of presets to sweep
PRESETS=("adv0" "adv1" "adv3" "adv10")

for PRESET in "${PRESETS[@]}"; do
    echo ""
    echo ">> ========================================================="
    echo ">> [SWEEP] Testing Preset: $PRESET"
    echo ">> ========================================================="
    
    for i in $(seq 1 $ITERATIONS); do
        echo "   -> Running Iteration $i of $ITERATIONS for $PRESET..."
        ./build/audio_testbed -p "$PRESET" -d "$DURATION" -c "$CHURN" -a "$ALLOCS" -f "$RESULTS"
        sleep 1 # 1s cool-down between runs
    done
done

echo ""
echo "================================================================="
echo ">> All benchmarks completed successfully!"
echo ">> Telemetry results saved to: $RESULTS"
echo "================================================================="
