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

The testbed is written in C++20 on Linux (ALSA backend via RtAudio) and consists of four distinct components:

1. **Hard Real-Time Audio Loop:**  
   A high-priority thread driven by the hardware soundcard (128 frames @ 48 kHz, ~2.67 ms deadline budget). Each callback performs dynamic allocations of varying sizes (512B to 8192B) to simulate real-time scratch buffers (e.g., intermediate tensor math or filter states).
2. **Concurrent Background Churn Workers:**  
   Separate background worker threads that continuously allocate, touch, and free memory of varying sizes (64B to 64KB). This simulates non-real-time application activity in a DAW or game (e.g., UI rendering, disk sample streaming, networking) and induces lock contention on shared `glibc` memory arenas.
3. **Lock-Free Telemetry Pipeline:**  
   Execution durations are measured with high-resolution clocks and pushed into a custom Single-Producer Single-Consumer (`SPSCQueue`) ring buffer.
   - Cache-line separation (`alignas(64)`) on head and tail pointers prevents false sharing.
   - Atomic acquire-release semantics ensure wait-free operation on the audio thread.
4. **Asynchronous Cold-Path Logging:**  
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

### C. Phase 3: Concurrent Allocation & Adversarial Stress Sweep (`glibc malloc`)
To rigorously evaluate general-purpose `glibc malloc` under multi-threaded contention and heap fragmentation, we executed an automated stress sweep across all ISMM '26 fragmentation presets (`adv0`, `adv1`, `adv3`, `adv10`) and allocation densities ($a \in \{4, 8, 16\}$ blocks per callback, sized 1.5 KB to 16 KB).

**Experimental Controls:**
- **Shared Global Arena (`mallopt(M_ARENA_MAX, 1)`):** Eliminates `glibc` multi-arena partitioning so the audio thread allocates directly inside the poisoned heap holes and contends on the arena mutex with background churn threads.
- **Sliding-Window History Buffer (8 frames / ~21 ms):** Holds previous allocations alive across callbacks, defeating immediate `tcache` and unsorted-bin head recycling.
- **Heterogeneous Large-Bin Allocations:** Variable non-power-of-two buffer sizes (1,536B to 16,384B) forcing skip-list tree search, chunk splitting, and remainder management.
- **Background Churn Threads:** 2 concurrent worker threads executing continuous alloc/free cycles (64B to 64KB), throttled to background priority (`SCHED_IDLE` + `nice = +19`) to eliminate raw CPU core starvation.
- **Real-Time Deadline:** 128 frames @ 48 kHz = **2,666.67 µs (2.67 ms)**.

Data directly generated by [`run_sweep.sh`](run_sweep.sh) saved to [`results.csv`](results.csv) (5 seconds / ~1,840 callbacks per run, ALSA backend):

| Preset & Fragmentation | Allocs / Block ($a$) | Median Latency ($P_{50}$) | Worst-Case Execution ($T_{\text{exec}}$ Max) | Glitches (Underruns) | Real-Time Status |
| :--- | :---: | :---: | :---: | :---: | :---: |
| **adv0** (Clean Baseline) | 4 | 6.8 µs | 2,148.5 µs (80.6% of budget) | 0 | **PASSED** |
| **adv0** (Clean Baseline) | 8 | 12.1 µs | **6,464.5 µs (+142% over deadline)** | **5** | **FAILED** |
| **adv0** (Clean Baseline) | 16 | 23.7 µs | **3,415.7 µs (+28% over deadline)** | **1** | **FAILED** |
| **adv1** (Mild: $M=1.0, O=33\%$) | 4 | 29.4 µs | **3,455.1 µs (+30% over deadline)** | 0 | **DEADLINE BLOWOUT** |
| **adv1** (Mild: $M=1.0, O=33\%$) | 8 | 45.2 µs | **3,369.4 µs (+26% over deadline)** | **2** | **FAILED** |
| **adv1** (Mild: $M=1.0, O=33\%$) | 16 | 73.3 µs | **14,607.5 µs (+448% over deadline)** | **3** | **FAILED** |
| **adv3** (Moderate: $M=3.0, O=66\%$) | 4 | 19.5 µs | **19,197.3 µs (+620% over deadline)** | **2** | **FAILED** |
| **adv3** (Moderate: $M=3.0, O=66\%$) | 8 | 28.7 µs | 202.4 µs (7.6% of budget) | 0 | **PASSED** |
| **adv3** (Moderate: $M=3.0, O=66\%$) | 16 | 45.4 µs | **10,238.0 µs (+284% over deadline)** | **1** | **FAILED** |
| **adv10** (Heavy: $M=10.0, O=80\%$) | 4 | 17.0 µs | **2,819.6 µs (+5.7% over deadline)** | **1** | **FAILED** |
| **adv10** (Heavy: $M=10.0, O=80\%$) | 8 | 26.6 µs | 128.7 µs (4.8% of budget) | 0 | **PASSED** |
| **adv10** (Heavy: $M=10.0, O=80\%$) | 16 | 40.8 µs | **16,590.9 µs (+522% over deadline)** | **1** | **FAILED** |

### Analysis of Allocator Failure Modes:

1. **Monotonic Allocation Density Scaling:**
   - Within every preset, median execution latency ($P_{50}$) scales monotonically with allocation count ($a = 4 \rightarrow 8 \rightarrow 16$).
   - On the clean baseline (`adv0`), $P_{50}$ rises smoothly from **6.8 µs $\rightarrow$ 12.1 µs $\rightarrow$ 23.7 µs**.
2. **Fragmentation Degradation (The Locality & Traversal Penalty):**
   - Adversarial preconditioning elevates median latency by **2x to 4.3x** over the clean baseline (jumping up to **73.3 µs** on `adv1`), reflecting the L1/L2 cache degradation and free-list traversal overhead documented in *van Kempen & Berger*.
3. **Severe Non-Deterministic Tail Blowouts:**
   - In **9 out of 12 runs (75%)**, `glibc malloc` exhibited worst-case latency spikes exceeding the 2.67 ms hard real-time deadline, reaching up to **19,197 µs (19.2 ms / 7.2x budget)**.
   - This extreme unpredictability stems from stochastic arena mutex contention (`pthread_mutex_t`) colliding with background churn threads, combined with unsorted-bin chunk splitting.
4. **High Real-Time Failure Rate:**
   - Hardware buffer underruns occurred in **8 out of 12 stress runs (66.7% failure rate)**, confirming empirically that general-purpose multi-threaded dynamic allocators cannot guarantee determinism in hard real-time systems.

---

## 5. Current Status & Next Steps

We have established both theoretically and empirically that general-purpose dynamic allocation (`glibc malloc`) cannot satisfy hard real-time guarantees under concurrent thread activity and heap fragmentation.

**Next Steps:**
- Implement an $O(1)$ Region / Arena Allocator (Algorithms 1 & 2 from *van Kempen & Berger*) using contiguous pre-reserved virtual memory blocks.
- Route time-critical scratch allocations exclusively through the Region Allocator while keeping the global heap poisoned under `adv10` and background churn threads active.
- Quantitatively evaluate whether bounded, constant-time region allocation completely eliminates hardware buffer underruns and tail latency spikes across the stress matrix.

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

# Run ISMM '26 adv3 preset with 2 churn threads for 10 seconds
./build/audio_testbed -p adv3 -c 2 -d 10

# Inspect all available command-line options
./build/audio_testbed --help
```

### Running the Automated Sweep
```bash
# Systematic stress sweep across adv0, adv1, adv3, adv10 and alloc densities {4, 8, 16}
./run_sweep.sh
```

---

## References
- Nicolas van Kempen and Emery D. Berger. *Reconsidering "Reconsidering Custom Memory Allocation"*, ISMM '26.
- Ross Bencina. *Real-time audio programming 101: time waits for nothing*, 2000.
