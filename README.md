# Hard Real-Time Audio DSP Testbed & Allocator Benchmark

A headless, cross-platform C++20 benchmarking testbed to measure the impact of heap fragmentation and memory allocator determinism on hard real-time audio processing loops.

## System Architecture & Goals
- **Audio Framework:** RtAudio (v6.0+) with native low-latency backends (WASAPI / ALSA).
- **Telemetry Pipeline:** Lock-free, wait-free Single-Producer Single-Consumer (SPSC) queue with cache-line alignment (`alignas(64)`) and acquire-release semantics.
- **Real-Time Budget:** 128 frames @ 44.1/48 kHz (~2.66 - 2.90 ms deadline).

## Experimental Phases
1. **Phase 1 (Done):** Clean Baseline DSP Loop with zero hot-path allocations and lock-free telemetry.
2. **Phase 2 (In Progress):** Adversarial Heap Fragmentation Simulator.
3. **Phase 3:** Standard Heap Stress (`malloc` / `new`) in Hot Path under fragmentation.
4. **Phase 4:** Custom $O(1)$ Region / Arena Allocator integration.

## Building and Running
```bash
# Configure Release Build
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build --config Release

# Run
./build/Release/audio_testbed.exe
```
