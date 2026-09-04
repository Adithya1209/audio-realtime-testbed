# Real-Time Allocator Determinism Testbed

An empirical study evaluating memory allocator latency and determinism in hard real-time systems, inspired by the heap fragmentation methodology from Nicolas van Kempen and Prof. Emery Berger (*Reconsidering "Reconsidering Custom Memory Allocation"*, ISMM '26).

---

## 1. Motivation & Background

In hard real-time environments, software must complete its work within strict, non-negotiable deadlines. A standard example is real-time audio rendering: with a buffer size of 128 frames at a 48 kHz sample rate, the system must process every block in under **2.67 milliseconds (2,666.67 µs)**. If the processing thread misses this deadline even by a few microseconds, the soundcard's hardware buffer starves, causing an **underrun** (an audible dropout or glitch).

Because of this, the longstanding guideline in real-time programming has been:
> *Never perform dynamic memory allocation (`malloc`, `free`, `new`, `delete`) in the time-critical path.*

While pre-allocating all memory statically works well for fixed pipelines, modern workloads increasingly challenge this restriction:
- **Neural audio models** with variable intermediate tensor shapes during inference.
- **Dynamic DSP graphs** with dynamic voice allocation and user-reconfigurable routing.
- **High-density automation streams** with variable-length event queues.

### Research Questions
1. How do general-purpose memory allocators (`glibc ptmalloc`) behave inside hard real-time loops when the heap is fragmented and other threads are active?
2. What causes deadline misses: free-list search overhead, memory fragmentation, or thread lock contention?
3. Can a bounded O(1) Region / Arena Allocator provide sufficient determinism to safely permit dynamic allocations in the critical path?

---

## 2. System Architecture

The testbed is written in C++20 on Linux (ALSA backend via RtAudio) and consists of three components:

1. **Hard Real-Time Audio Loop:**  
   A high-priority thread driven by the hardware soundcard (128 frames @ 48 kHz, ~2.67 ms deadline budget). Each callback optionally performs dynamic allocations of varying sizes (512B to 8192B) to simulate hot-path scratch memory.
2. **Lock-Free Telemetry Pipeline:**  
   Execution durations are measured with high-resolution clocks and pushed into a custom Single-Producer Single-Consumer (`SPSCQueue`) ring buffer.
   - Cache-line separation (`alignas(64)`) on head and tail pointers prevents false sharing.
   - Atomic acquire-release semantics ensure wait-free operation on the audio thread.
3. **Asynchronous Cold-Path Logging:**  
   The main thread drains the telemetry queue periodically. After audio processing finishes, the main thread computes statistical percentiles (P50, P90, P99, P99.9, Worst-case) and appends the run to `results.csv`. The time-critical thread performs no disk I/O, console logging, or mutex synchronization.

---

## 3. Heap Preconditioning (ISMM '26 Algorithm 3)

To test allocator behavior under realistic conditions rather than a pristine heap, we implemented the stochastic heap preconditioning algorithm from *van Kempen & Berger (ISMM '26, Algorithm 3)* in [`HeapPoisoner.hpp`](HeapPoisoner.hpp):

1. **Allocation & Page Touch:**  
   Allocates `N = multiplier × peak_allocations` objects, sampling uniformly across 9 size classes (32B to 8192B). Memory is touched (`memset`) to ensure physical RAM pages are committed by the OS kernel.
2. **Uniform Shuffle:**  
   The array of allocated pointers is randomized in-place using `std::shuffle` driven by a seeded Mersenne Twister (`std::mt19937`, seed 42) for scientific reproducibility.
3. **Occupancy-Based Deallocation:**  
   Frees `N × (1 - Occupancy)` pointers from the shuffled list, leaving `N × Occupancy` blocks pinned to prevent chunk coalescing.

### Evaluated Presets (from Table 3 of the ISMM '26 paper):
- **`adv0`:** Clean baseline (Multiplier = 0, preconditioning skipped).
- **`adv1`:** Mild fragmentation (Multiplier = 1.0, Occupancy = 33%).
- **`adv3`:** Moderate fragmentation (Multiplier = 3.0, Occupancy = 66%).
- **`adv10`:** Heavy fragmentation (Multiplier = 10.0, Occupancy = 80%).

The testbed also retains a synthetic **Strided** checkerboard method (`i % 2 == 0`) for baseline comparison.

---

## 4. Empirical Findings

All tests were conducted on Linux (EndeavourOS, kernel 6.x, GCC 16, ALSA output, 128 frames @ 48 kHz):

### A. Phase 1: Clean Baseline (No Hot-Path Allocations)
- **Result:** 0 hardware underruns.
- **Latency:** Mean ~10.3 µs, Worst-Case ~51.4 µs (< 2% of the 2,666.7 µs deadline budget).
- **Takeaway:** Pure arithmetic DSP with pre-allocated buffers is deterministic.

### B. Phase 2: Single-Threaded Allocation on Fragmented Heap
- Allocating and freeing 4 scratch blocks (512B to 8192B) on an adversarial heap without other threads.
- **Result:** 0 hardware underruns, mean latency rose from 10.3 µs to 17.1 µs (+66%).
- **Observation (The "Tcache Illusion"):** When allocation patterns repeat without contention, `glibc` thread-local caching (tcache and fastbins) recycles chunks in L1/L2 cache, partially masking the underlying heap fragmentation.

### C. Phase 3: Concurrent Allocation & Fragmentation Sweep
We executed an automated sweep across all ISMM '26 presets with 2 background threads allocating and freeing blocks (64B to 64KB) to simulate application workload contention.

Data from [`results.csv`](results.csv) (10 seconds per run, 128 frames @ 48 kHz):

| Preset | Multiplier (M) | Occupancy (O) | Churn Threads | Underruns (Glitches) | P50 Latency | P99 Tail | Worst-Case Max | Heap Footprint / Free Holes | Status |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **adv0** | 0.00 | – | 2 | **4** | 6.11 µs | 18.20 µs | 170.78 µs | 0.33 MB / 184 holes | FAILED |
| **adv0** | 0.00 | – | 2 | **20** | 8.98 µs | 15.17 µs | 55.86 µs | 0.33 MB / 184 holes | FAILED |
| **adv1** | 1.00 | 33% | 2 | **5** | 5.67 µs | 18.24 µs | 136.26 µs | ~29 MB / 5,600 holes | FAILED |
| **adv1** | 1.00 | 33% | 2 | **9** | 5.63 µs | 19.27 µs | 140.82 µs | ~29 MB / 5,600 holes | FAILED |
| **adv3** | 3.00 | 66% | 2 | **4** | 6.25 µs | 18.03 µs | 86.08 µs | 86.75 MB / 16,907 holes | FAILED |
| **adv3** | 3.00 | 66% | 2 | **9** | 5.83 µs | 12.28 µs | 54.84 µs | 86.75 MB / 16,907 holes | FAILED |
| **adv10**| 10.00 | 80% | 2 | **6** | 5.91 µs | 12.22 µs | 58.19 µs | 349.56 MB / 40,072 holes | FAILED |
| **adv10**| 10.00 | 80% | 2 | **7** | 6.05 µs | 13.37 µs | 62.19 µs | 349.56 MB / 40,068 holes | FAILED |

### Key Observations:
1. **100% Failure Rate Under Contention:** Every configuration experienced hardware buffer underruns (between 4 and 20 audio dropouts per 10s test).
2. **Lock Contention on Clean Heaps (`adv0`):** Even with zero heap fragmentation, `adv0` failed repeatedly. This indicates that `glibc` arena mutex contention alone—caused by background threads—is sufficient to stall the audio thread and cause DMA ring buffer starvation.
3. **Partial Coalescing Dynamics:** In `adv3`, 25,499 blocks were freed, but `mallinfo2` recorded 16,907 free chunks. The difference indicates that adjacent randomized freed blocks coalesced into irregular non-standard chunk sizes, exercising `ptmalloc`'s chunk-splitting and best-fit bin search logic.
4. **Memory Footprint Scaling at `adv10`:** Under `adv10`, memory expanded to **349.56 MB** with **40,072 disjoint free chunk holes**, representing severe heap fragmentation.

---

## 5. Current Status & Next Steps (Phase 4)

We have demonstrated that general-purpose multi-threaded dynamic allocation cannot satisfy hard real-time guarantees under heap fragmentation and concurrent thread activity.

**Current Work (Phase 4):**
- Implement an O(1) Region / Arena Allocator (Algorithms 1 & 2 from *van Kempen & Berger*).
- Route hot-path temporary scratch allocations through the Arena while keeping the heap poisoned under `adv10` and background churn active.
- Verify whether constant-time allocation eliminates all hardware buffer underruns, returning the system to 0 glitches.

---

## 6. Building and Running

### Requirements
- Linux (ALSA development headers: `libasound2-dev` on Debian/Ubuntu, `alsa-lib` on Arch)
- CMake 3.16+
- C++20 compliant compiler (GCC 11+ or Clang 13+)

### Build
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### Running Individual Tests
```bash
# Run clean baseline with zero hot-path allocations
./build/audio_testbed -p adv0 --no-stress

# Run ISMM '26 Adv3 preset with 2 churn threads for 10 seconds
./build/audio_testbed -p adv3 -c 2 -d 10

# Inspect all available command-line options
./build/audio_testbed --help
```

### Running the Automated Sweep
```bash
# Sweeps adv0, adv1, adv3, and adv10; appends metrics to results.csv
./run_sweep.sh
```

---

## References
- Nicolas van Kempen and Emery D. Berger. *Reconsidering "Reconsidering Custom Memory Allocation"*, ISMM '26.
- Ross Bencina. *Real-time audio programming 101: time waits for nothing*, 2000.
