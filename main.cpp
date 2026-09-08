#define _USE_MATH_DEFINES
#include <iostream>
#include <cmath>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#include <numeric>
#include <iomanip>
#include <memory>
#include <getopt.h>
#include <fstream>
#include <filesystem>
#include <malloc.h>
#include "RtAudio.h"
#include "SPSCQueue.hpp"
#include "HeapPoisoner.hpp"

constexpr double PI = 3.14159265358979323846;

struct CallbackTelemetry {
    uint64_t duration_ns;      // Hot-path execution time inside callback (T_exec)
    uint64_t inter_arrival_ns; // Inter-arrival time between consecutive callbacks (T_interval)
    bool is_underflow;
};

struct AudioContext {
    double phase = 0.0;
    double frequency = 440.0;     // 440 Hz (A4 tone)
    double sampleRate = 48000.0;
    SPSCQueue<CallbackTelemetry, 65536> telemetryQueue;

    bool stressHeap = true;
    size_t numAllocationsPerCallback = 4;
    size_t allocationSizeBytes = 4096;

    size_t callbackCount = 0;
    size_t warmupCallbacks = 40; // Discard first ~106 ms of stream startup transients

    std::chrono::steady_clock::time_point lastCallbackTime{};
    bool hasLastCallbackTime = false;

    // Sliding-window history buffer: holds blocks alive across 8 callbacks (~21 ms)
    // to simulate real DSP history (delay lines, FIR filters, FFT overlap-add)
    // and defeat immediate glibc tcache recycling
    static constexpr size_t HISTORY_WINDOW = 8;
    static constexpr size_t MAX_SCRATCH = 32;
    void* historyBlocks[HISTORY_WINDOW][MAX_SCRATCH] = {{nullptr}};
    size_t historyAllocCounts[HISTORY_WINDOW] = {0};
    size_t historyIndex = 0;

    ~AudioContext() {
        for (size_t w = 0; w < HISTORY_WINDOW; ++w) {
            for (size_t a = 0; a < MAX_SCRATCH; ++a) {
                if (historyBlocks[w][a]) {
                    std::free(historyBlocks[w][a]);
                    historyBlocks[w][a] = nullptr;
                }
            }
        }
    }
};

struct BenchmarkConfig {
    std::string preset = "adv3";
    HeapPoisoner::Strategy strategy = HeapPoisoner::Strategy::AdversarialISMM26;

    // ISMM'26 Knobs:
    double multiplier = 3;
    double occupancy = 0.66;
    unsigned int randomSeed = 42;
    size_t peakAllocations = 25000;

    // Strided Knob:
    size_t totalAllocations = 80000;
    size_t freeStride = 2;

    // Audio and Telemetry Knobs:
    bool stressHeap = true;
    size_t numAllocationsPerCallback = 4;
    size_t churnThreads = 2;
    unsigned int durationSeconds = 10;
    std::string csvPath = "";

    int deviceId = -1; // -1 indicates system default device
};

BenchmarkConfig parseCommandLine(int argc, char* argv[]) {
    BenchmarkConfig cfg;

    static struct option long_options[] = {
        {"preset",     required_argument, 0, 'p'},
        {"multiplier", required_argument, 0, 'm'},
        {"occupancy",  required_argument, 0, 'o'},
        {"churn",      required_argument, 0, 'c'},
        {"allocs",     required_argument, 0, 'a'},
        {"duration",   required_argument, 0, 'd'},
        {"seed",       required_argument, 0, 's'},
        {"csv",        required_argument, 0, 'f'},
        {"no-stress",  no_argument,       0, 'n'},
        {"help",       no_argument,       0, 'h'},
        {"device",     required_argument, 0, 'D'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;
    while((opt = getopt_long(argc, argv, "p:m:o:c:a:d:s:f:nhD:", long_options, &option_index)) != -1) {
        switch(opt) {
            case 'p':
                cfg.preset = optarg;
                if (cfg.preset == "adv0") {
                    cfg.multiplier = 0.0;
                } else if (cfg.preset == "adv1") {
                    cfg.multiplier = 1.0;
                    cfg.occupancy = 0.33;
                    cfg.strategy = HeapPoisoner::Strategy::AdversarialISMM26;
                } else if (cfg.preset == "adv3") {
                    cfg.multiplier = 3.0;
                    cfg.occupancy = 0.66;
                    cfg.strategy = HeapPoisoner::Strategy::AdversarialISMM26;
                } else if (cfg.preset == "adv10") {
                    cfg.multiplier = 10.0;
                    cfg.occupancy = 0.80;
                    cfg.strategy = HeapPoisoner::Strategy::AdversarialISMM26;
                } else if (cfg.preset == "strided") {
                    cfg.strategy = HeapPoisoner::Strategy::Strided;
                }
                break;
            case 'm': cfg.multiplier = std::stod(optarg); break;
            case 'o': cfg.occupancy = std::stod(optarg); break;
            case 'c': cfg.churnThreads = std::stoul(optarg); break;
            case 'a': cfg.numAllocationsPerCallback = std::stoul(optarg); break;
            case 'd': cfg.durationSeconds = std::stoul(optarg); break;
            case 's': cfg.randomSeed = static_cast<unsigned int>(std::stoul(optarg)); break;
            case 'f': cfg.csvPath = optarg; break;
            case 'n': cfg.stressHeap = false; break;
            case 'D': cfg.deviceId = std::stoi(optarg); break;
            case 'h':
            default:
                std::cout << "Usage: " << argv[0] << " [options]\n"
                        << "  -p, --preset <adv0|adv1|adv3|adv10|strided>  Load paper preset\n"
                        << "  -m, --multiplier <float>                     Heap footprint multiplier M\n"
                        << "  -o, --occupancy <float>                      Live block occupancy fraction O\n"
                        << "  -c, --churn <int>                            Background churn threads\n"
                        << "  -a, --allocs <int>                           Hot-path allocations per callback\n"
                        << "  -d, --duration <seconds>                     Benchmark duration in seconds\n"
                        << "  -s, --seed <int>                             PRNG shuffle seed (default 42)\n"
                        << "  -f, --csv <path>                             Append metrics to CSV file\n"
                        << "  -n, --no-stress                              Disable hot-path malloc\n"
                        << "  -D, --device <id>                            Target specific audio hardware device ID\n"
                        << "  -h, --help                                   Show this help message\n";
                std::exit(0);
        }
    }
    return cfg;
}

// Hard real-time audio callback: invoked by ALSA driver at fixed 2.67 ms intervals (128 frames @ 48 kHz)
int audioCallback(void* outputBuffer, void* /*inputBuffer*/, unsigned int nBufferFrames,
                  double /*streamTime*/, RtAudioStreamStatus status, void* userData) {

    const auto startTime = std::chrono::steady_clock::now();
    auto* ctx = static_cast<AudioContext*>(userData);

    uint64_t interArrivalNs = 0;
    if (ctx->hasLastCallbackTime) {
        interArrivalNs = std::chrono::duration_cast<std::chrono::nanoseconds>(startTime - ctx->lastCallbackTime).count();
    }
    ctx->lastCallbackTime = startTime;
    ctx->hasLastCallbackTime = true;

    // Hot-path memory stress: evaluates dynamic allocation latency under adversarial fragmentation
    if (ctx->stressHeap) {
        // Heterogeneous non-power-of-two DSP scratch sizes (delay buffers, FFT states, tensor scratchpads)
        // Exceeds smallbin exact-match threshold (>1024B) to defeat trivial smallbin indexing and force large-bin tree searches
        static const size_t stressSizes[] = {1536, 2752, 3840, 5120, 6400, 8960, 12288, 16384};
        constexpr size_t numStressSizes = sizeof(stressSizes) / sizeof(stressSizes[0]);

        size_t allocCount = std::min(ctx->numAllocationsPerCallback, AudioContext::MAX_SCRATCH);

        // 1. Free historical blocks from HISTORY_WINDOW callbacks ago (defeats immediate tcache recycling)
        size_t oldestIdx = ctx->historyIndex;
        for (size_t a = 0; a < ctx->historyAllocCounts[oldestIdx]; ++a) {
            if (ctx->historyBlocks[oldestIdx][a]) {
                std::free(ctx->historyBlocks[oldestIdx][a]);
                ctx->historyBlocks[oldestIdx][a] = nullptr;
            }
        }
        ctx->historyAllocCounts[oldestIdx] = 0;

        // 2. Allocate new blocks for current callback
        for (size_t a = 0; a < allocCount; ++a) {
            size_t sz = stressSizes[(ctx->callbackCount * allocCount + a) % numStressSizes];
            void* ptr = std::malloc(sz);
            if (ptr) {
                // Touch cache lines at head and tail to guarantee physical page and cache footprint
                auto* b = static_cast<char*>(ptr);
                b[0] = 0x55;
                b[sz - 1] = 0xAA;
                ctx->historyBlocks[oldestIdx][a] = ptr;
            }
        }
        ctx->historyAllocCounts[oldestIdx] = allocCount;
        ctx->historyIndex = (ctx->historyIndex + 1) % AudioContext::HISTORY_WINDOW;
    }

    const bool underflow = (status & RTAUDIO_OUTPUT_UNDERFLOW) != 0;

    auto* buffer = static_cast<float*>(outputBuffer);

    const double phaseIncrement = 2.0 * PI * ctx->frequency / ctx->sampleRate;

    for (unsigned int i = 0; i < nBufferFrames; ++i) {
        float sample = static_cast<float>(std::sin(ctx->phase) * 0.25);
        *buffer++ = sample;
        *buffer++ = sample;

        ctx->phase += phaseIncrement;
        if (ctx->phase >= 2.0 * PI) {
            ctx->phase -= 2.0 * PI;
        }
    }

    const auto endTime = std::chrono::steady_clock::now();
    const uint64_t elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime).count();

    ctx->callbackCount++;
    if (ctx->callbackCount > ctx->warmupCallbacks) {
        CallbackTelemetry sample{elapsedNs, interArrivalNs, underflow};
        ctx->telemetryQueue.push(sample);
    }
    return 0;
}

void printBenchmarkReport(const std::vector<CallbackTelemetry>& telemetryData, double budgetMs) {
    if (telemetryData.empty()) {
        std::cout << "[ERROR] No telemetry data collected!\n";
        return;
    }

    std::vector<double> durationsUs;
    std::vector<double> intervalsUs;
    durationsUs.reserve(telemetryData.size());
    intervalsUs.reserve(telemetryData.size());
    uint64_t underflowCount = 0;

    for (const auto& entry : telemetryData) {
        durationsUs.push_back(static_cast<double>(entry.duration_ns) / 1000.0);
        if (entry.inter_arrival_ns > 0) {
            intervalsUs.push_back(static_cast<double>(entry.inter_arrival_ns) / 1000.0);
        }
        if (entry.is_underflow) {
            underflowCount++;
        }
    }

    std::sort(durationsUs.begin(), durationsUs.end());

    double sum = std::accumulate(durationsUs.begin(), durationsUs.end(), 0.0);
    double mean = sum / static_cast<double>(durationsUs.size());
    double minVal = durationsUs.front();
    double maxVal = durationsUs.back();
    
    auto getPercentile = [](const std::vector<double>& vec, double p) -> double {
        if (vec.empty()) return 0.0;
        size_t idx = static_cast<size_t>(p * static_cast<double>(vec.size() - 1));
        return vec[idx];
    };

    double p50 = getPercentile(durationsUs, 0.50);
    double p90 = getPercentile(durationsUs, 0.90);
    double p99 = getPercentile(durationsUs, 0.99);
    double p999 = getPercentile(durationsUs, 0.999);

    double budgetUs = budgetMs * 1000.0;
    double maxBudgetUtilization = (maxVal / budgetUs) * 100.0;
    double p99BudgetUtilization = (p99 / budgetUs) * 100.0;

    // Interval / Jitter Telemetry
    double meanInterval = 0.0;
    double minInterval = 0.0;
    double maxInterval = 0.0;
    double p50Interval = 0.0;
    double p99Interval = 0.0;
    double p999Interval = 0.0;

    if (!intervalsUs.empty()) {
        std::sort(intervalsUs.begin(), intervalsUs.end());
        double sumInterval = std::accumulate(intervalsUs.begin(), intervalsUs.end(), 0.0);
        meanInterval = sumInterval / static_cast<double>(intervalsUs.size());
        minInterval = intervalsUs.front();
        maxInterval = intervalsUs.back();
        p50Interval = getPercentile(intervalsUs, 0.50);
        p99Interval = getPercentile(intervalsUs, 0.99);
        p999Interval = getPercentile(intervalsUs, 0.999);
    }

    std::cout << "\n========================================================\n";
    std::cout << "         HARD REAL-TIME AUDIO BENCHMARK REPORT          \n";
    std::cout << "========================================================\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Total Audio Callbacks Processed : " << telemetryData.size() << "\n";
    std::cout << "Hardware Buffer Underruns Count : " << underflowCount;
    if (underflowCount == 0) {
        std::cout << " (PASSED - ZERO GLITCHES)\n";
    } else {
        std::cout << " [!] FAILED - GLITCHES DETECTED!\n";
    }
    std::cout << "Hard Real-Time Budget per Block : " << budgetUs << " us (" << budgetMs << " ms)\n";
    std::cout << "--------------------------------------------------------\n";
    std::cout << "  Execution Latency Breakdown (T_exec - Hot Path):\n";
    std::cout << "   - Min Latency                : " << std::setw(8) << minVal << " us\n";
    std::cout << "   - Mean Latency               : " << std::setw(8) << mean << " us\n";
    std::cout << "   - P50 (Median) Latency       : " << std::setw(8) << p50 << " us\n";
    std::cout << "   - P90 Tail Latency           : " << std::setw(8) << p90 << " us\n";
    std::cout << "   - P99 Tail Latency           : " << std::setw(8) << p99 << " us\n";
    std::cout << "   - P99.9 Extreme Tail Latency : " << std::setw(8) << p999 << " us\n";
    std::cout << "   - Worst-Case Latency (Max)   : " << std::setw(8) << maxVal << " us\n";
    std::cout << "--------------------------------------------------------\n";
    std::cout << "  Budget Headroom & Utilization:\n";
    std::cout << "   - P99 Budget Utilization     : " << std::setw(8) << p99BudgetUtilization << " %\n";
    std::cout << "   - Worst-Case Utilization     : " << std::setw(8) << maxBudgetUtilization << " %\n";
    std::cout << "--------------------------------------------------------\n";
    std::cout << "  Inter-Callback Interval & Jitter (T_interval):\n";
    std::cout << "   - Nominal Period             : " << std::setw(8) << budgetUs << " us\n";
    std::cout << "   - Min Interval               : " << std::setw(8) << minInterval << " us\n";
    std::cout << "   - Mean Interval              : " << std::setw(8) << meanInterval << " us\n";
    std::cout << "   - P50 (Median) Interval      : " << std::setw(8) << p50Interval << " us\n";
    std::cout << "   - P99 Tail Interval          : " << std::setw(8) << p99Interval << " us\n";
    std::cout << "   - P99.9 Extreme Interval     : " << std::setw(8) << p999Interval << " us\n";
    std::cout << "   - Worst-Case Interval (Max)  : " << std::setw(8) << maxInterval << " us\n";
    if (maxInterval > budgetUs) {
        std::cout << "   - Max Interval Overdue Gap   : +" << std::setw(7) << (maxInterval - budgetUs) << " us ("
                  << (maxInterval / budgetUs * 100.0) << "% of budget)\n";
    }
    std::cout << "========================================================\n\n";
}

void appendToCSV(const std::string& csvPath, const BenchmarkConfig& cfg, 
                const std::vector<CallbackTelemetry>& telemetryData, double budgetMs) {
    if (csvPath.empty() || telemetryData.empty()) return;

    bool fileExists = std::filesystem::exists(csvPath);
    std::ofstream file(csvPath, std::ios::app);
    if (!file.is_open()) {
        std::cerr << "[ERROR] Could not open CSV file: " << csvPath << "\n";
        return;
    }

    // Write header if creating a new file
    if (!fileExists) {
        file << "preset,strategy,multiplier,occupancy,churn_threads,allocs_per_cb,duration_s,"
                << "callbacks,underruns,min_us,mean_us,p50_us,p90_us,p99_us,p999_us,max_us,"
                << "mean_interval_us,p99_interval_us,max_interval_us,status\n";
    }

    std::vector<double> durationsUs;
    std::vector<double> intervalsUs;
    durationsUs.reserve(telemetryData.size());
    intervalsUs.reserve(telemetryData.size());
    uint64_t underflowCount = 0;

    for (const auto& entry : telemetryData) {
        durationsUs.push_back(static_cast<double>(entry.duration_ns) / 1000.0);
        if (entry.inter_arrival_ns > 0) {
            intervalsUs.push_back(static_cast<double>(entry.inter_arrival_ns) / 1000.0);
        }
        if (entry.is_underflow) underflowCount++;
    }

    std::sort(durationsUs.begin(), durationsUs.end());
    double sum = std::accumulate(durationsUs.begin(), durationsUs.end(), 0.0);
    double mean = sum / static_cast<double>(durationsUs.size());
    double minVal = durationsUs.front();
    double maxVal = durationsUs.back();

    auto getPercentile = [](const std::vector<double>& vec, double p) -> double {
        if (vec.empty()) return 0.0;
        size_t idx = static_cast<size_t>(p * static_cast<double>(vec.size() - 1));
        return vec[idx];
    };

    double p50 = getPercentile(durationsUs, 0.50);
    double p90 = getPercentile(durationsUs, 0.90);
    double p99 = getPercentile(durationsUs, 0.99);
    double p999 = getPercentile(durationsUs, 0.999);

    double meanInterval = 0.0;
    double p99Interval = 0.0;
    double maxInterval = 0.0;
    if (!intervalsUs.empty()) {
        std::sort(intervalsUs.begin(), intervalsUs.end());
        double sumInterval = std::accumulate(intervalsUs.begin(), intervalsUs.end(), 0.0);
        meanInterval = sumInterval / static_cast<double>(intervalsUs.size());
        p99Interval = getPercentile(intervalsUs, 0.99);
        maxInterval = intervalsUs.back();
    }

    std::string stratName = (cfg.strategy == HeapPoisoner::Strategy::Strided) ? "Strided" : "ISMM26";
    if (cfg.stressHeap) {
        stratName += "+malloc";
    }
    std::string status = (underflowCount == 0) ? "PASSED" : "FAILED";

    file << std::fixed << std::setprecision(2)
            << cfg.preset << ","
            << stratName << ","
            << cfg.multiplier << ","
            << cfg.occupancy << ","
            << cfg.churnThreads << ","
            << cfg.numAllocationsPerCallback << ","
            << cfg.durationSeconds << ","
            << telemetryData.size() << ","
            << underflowCount << ","
            << minVal << ","
            << mean << ","
            << p50 << ","
            << p90 << ","
            << p99 << ","
            << p999 << ","
            << maxVal << ","
            << meanInterval << ","
            << p99Interval << ","
            << maxInterval << ","
            << status << "\n";

    std::cout << "[Telemetry] Appended run results to " << csvPath << "\n";
}


int main(int argc, char* argv[]) {
    // Break glibc multi-arena isolation: force all threads (preconditioning, audio callback, churn workers)
    // to share the single global heap arena. This ensures the audio thread allocates directly within the
    // fragmented heap holes created by HeapPoisoner and contends on the arena mutex with background churn.
    mallopt(M_ARENA_MAX, 1);

    BenchmarkConfig cfg = parseCommandLine(argc, argv);

    std::cout << "=== Hard Real-Time Audio DSP Testbed ===\n";
    std::cout << "RtAudio Version: " << RtAudio::getVersion() << "\n";

    RtAudio dac;
    std::cout << "Current Backend API: " << RtAudio::getApiDisplayName(dac.getCurrentApi()) << "\n";

    std::vector<unsigned int> deviceIds = dac.getDeviceIds();
    if (deviceIds.empty()) {
        std::cerr << "[ERROR] No audio output devices found!\n";
        return 1;
    }

    std::cout << "\nAvailable Audio Output Devices:\n";
    unsigned int defaultDevice = dac.getDefaultOutputDevice();
    for (unsigned int id : deviceIds) {
        RtAudio::DeviceInfo devInfo = dac.getDeviceInfo(id);
        if (devInfo.outputChannels > 0) {
            std::cout << "  - ID " << std::setw(3) << id << ": " << devInfo.name
                      << (id == defaultDevice ? " [DEFAULT]" : "") << "\n";
        }
    }

    unsigned int selectedDevice = (cfg.deviceId >= 0) ? static_cast<unsigned int>(cfg.deviceId) : defaultDevice;
    RtAudio::DeviceInfo info = dac.getDeviceInfo(selectedDevice);
    std::cout << "\nUsing Output Device: " << info.name << " (ID: " << selectedDevice << ")\n";

    // Initialize and run Heap Poisoner
    HeapPoisoner poisoner;
    if (cfg.multiplier > 0.0 || cfg.strategy == HeapPoisoner::Strategy::Strided) {
        poisoner.printHeapStats("Baseline (Before Poisoning)");

        HeapPoisoner::Config poisonConfig;
        poisonConfig.strategy = cfg.strategy;
        poisonConfig.multiplier = cfg.multiplier;
        poisonConfig.occupancy = cfg.occupancy;
        poisonConfig.randomSeed = cfg.randomSeed;
        poisonConfig.peakAllocations = cfg.peakAllocations;
        poisonConfig.totalAllocations = cfg.totalAllocations;
        poisonConfig.freeStride = cfg.freeStride;  

        poisoner.fragmentHeap(poisonConfig);
        poisoner.printHeapStats("After Heap Poisoning");
    } else {
        std::cout << "[HeapPoisoner] Heap preconditioning skipped (clean baseline)\n";
    }

    // Pre-allocated AudioContext on heap to prevent thread stack exhaustion from internal SPSC queue
    auto ctx = std::make_unique<AudioContext>();
    ctx->stressHeap = cfg.stressHeap;
    ctx->numAllocationsPerCallback = cfg.numAllocationsPerCallback;

    unsigned int sampleRate = static_cast<unsigned int>(info.preferredSampleRate > 0 ? info.preferredSampleRate : 48000);
    ctx->sampleRate = static_cast<double>(sampleRate);

    unsigned int bufferFrames = 128; // 128 frames @ 48kHz = 2.666 ms hard real-time deadline budget

    RtAudio::StreamParameters parameters;
    parameters.deviceId = selectedDevice;
    parameters.nChannels = std::min(2u, info.outputChannels);
    parameters.firstChannel = 0;

    RtAudio::StreamOptions options;
    options.flags = RTAUDIO_MINIMIZE_LATENCY | RTAUDIO_SCHEDULE_REALTIME; // Request low-latency period and real-time thread scheduling
    options.priority = 50;

    RtAudioErrorType err = dac.openStream(
        &parameters,
        nullptr,
        RTAUDIO_FLOAT32,
        sampleRate,
        &bufferFrames,
        &audioCallback,
        ctx.get(),
        &options
    );

    if (err != RTAUDIO_NO_ERROR) {
        std::cerr << "[ERROR] Failed to open stream: " << dac.getErrorText() << "\n";
        return 1;
    }

    double budgetMs = (static_cast<double>(bufferFrames) / ctx->sampleRate) * 1000.0;
    std::cout << "\nStream Opened Successfully:\n";
    std::cout << " - Sample Rate: " << sampleRate << " Hz\n";
    std::cout << " - Buffer Size: " << bufferFrames << " frames\n";
    std::cout << " - Real-Time Callback Deadline Budget: " << budgetMs << " ms (" << (budgetMs * 1000.0) << " us)\n\n";

    if (cfg.churnThreads > 0) {
        poisoner.startBackgroundChurn(cfg.churnThreads);
    }

    err = dac.startStream();
    if (err != RTAUDIO_NO_ERROR) {
        std::cerr << "[ERROR] Failed to start stream: " << dac.getErrorText() << "\n";
        return 1;
    }

    std::cout << ">>> Running " << cfg.preset << " Benchmark for " << cfg.durationSeconds << " seconds...\n";
    std::vector<CallbackTelemetry> collectedTelemetry;
    collectedTelemetry.reserve(cfg.durationSeconds * 400);

    const auto benchmarkStart = std::chrono::steady_clock::now();
    const auto benchmarkDuration = std::chrono::seconds(cfg.durationSeconds);

    // Asynchronously drain telemetry queue on main thread during benchmark execution
    while (std::chrono::steady_clock::now() - benchmarkStart < benchmarkDuration) {
        CallbackTelemetry item;
        while (ctx->telemetryQueue.pop(item)) {
            collectedTelemetry.push_back(item);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    CallbackTelemetry item;
    while (ctx->telemetryQueue.pop(item)) {
        collectedTelemetry.push_back(item);
    }

    if (cfg.churnThreads > 0) {
        poisoner.stopBackgroundChurn();
    }

    dac.stopStream();
    if (dac.isStreamOpen()) {
        dac.closeStream();
    }

    printBenchmarkReport(collectedTelemetry, budgetMs);

    // Cold-path CSV export executed strictly after stream termination to guarantee zero real-time disk I/O
    appendToCSV(cfg.csvPath, cfg, collectedTelemetry, budgetMs);

    return 0;
}