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
#include "RtAudio.h"
#include "SPSCQueue.hpp"
#include "HeapPoisoner.hpp"

constexpr double PI = 3.14159265358979323846;

struct CallbackTelemetry {
    uint64_t duration_ns;
    bool is_underflow;
};

struct AudioContext {
    double phase = 0.0;
    double frequency = 440.0;     // 440 Hz (A4 tone)
    double sampleRate = 48000.0;
    SPSCQueue<CallbackTelemetry, 65536> telemetryQueue;

    // Stress Test Controls:
    bool stressHeap = true;              // Enable/disable hot-path malloc
    size_t numAllocationsPerCallback = 4; // Simulate 4 temporary scratch buffers
    size_t allocationSizeBytes = 4096;    // 1024 bytes each (total 4KB per block)
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
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;
    while((opt = getopt_long(argc, argv, "p:m:o:c:a:d:s:f:nh", long_options, &option_index)) != -1) {
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
                        << "  -h, --help                                   Show this help message\n";
                std::exit(0);
        }
    }
    return cfg;
}

// The Hard Real-Time Audio Callback (The Hot Path)
int audioCallback(void* outputBuffer, void* /*inputBuffer*/, unsigned int nBufferFrames,
                  double /*streamTime*/, RtAudioStreamStatus status, void* userData) {

    const auto startTime = std::chrono::steady_clock::now();
    auto* ctx = static_cast<AudioContext*>(userData);

    // --- HEAP ALLOCATION STRESS TEST ---
    if (ctx->stressHeap) {
        static const size_t stressSizes[4] = {512, 2048, 4096, 8192};
        constexpr size_t MAX_SCRATCH = 32;
        size_t allocCount = std::min(ctx->numAllocationsPerCallback, MAX_SCRATCH);
        void* scratchBlocks[MAX_SCRATCH] = {nullptr};

        for (size_t a = 0; a < allocCount; ++a) {
            size_t sz = stressSizes[a % 4];
            scratchBlocks[a] = std::malloc(sz);
            if (scratchBlocks[a]) std::memset(scratchBlocks[a], 0, sz);
        }

        for (size_t a = 0; a < allocCount; ++a) {
            if (scratchBlocks[a]) std::free(scratchBlocks[a]);
        }
    }

    const bool underflow = (status & RTAUDIO_OUTPUT_UNDERFLOW) != 0;

    auto* buffer = static_cast<float*>(outputBuffer);

    const double phaseIncrement = 2.0 * PI * ctx->frequency / ctx->sampleRate;

    // Generate Stereo Sine Wave (Zero-Allocation Hot Path)
    for (unsigned int i = 0; i < nBufferFrames; ++i) {
        float sample = static_cast<float>(std::sin(ctx->phase) * 0.25); // Volume at 25%
        *buffer++ = sample; // Left
        *buffer++ = sample; // Right

        ctx->phase += phaseIncrement;
        if (ctx->phase >= 2.0 * PI) {
            ctx->phase -= 2.0 * PI;
        }
    }

    const auto endTime = std::chrono::steady_clock::now();
    const uint64_t elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime).count();

    CallbackTelemetry sample{elapsedNs, underflow};
    ctx->telemetryQueue.push(sample);
    return 0;
}

void printBenchmarkReport(const std::vector<CallbackTelemetry>& telemetryData, double budgetMs) {
    if (telemetryData.empty()) {
        std::cout << "[ERROR] No telemetry data collected!\n";
        return;
    }

    std::vector<double> durationsUs;
    durationsUs.reserve(telemetryData.size());
    uint64_t underflowCount = 0;

    for (const auto& entry : telemetryData) {
        durationsUs.push_back(static_cast<double>(entry.duration_ns) / 1000.0);
        if (entry.is_underflow) {
            underflowCount++;
        }
    }

    std::sort(durationsUs.begin(), durationsUs.end());

    double sum = std::accumulate(durationsUs.begin(), durationsUs.end(), 0.0);
    double mean = sum / static_cast<double>(durationsUs.size());
    double minVal = durationsUs.front();
    double maxVal = durationsUs.back();
    
    auto getPercentile = [&](double p) -> double {
        size_t idx = static_cast<size_t>(p * static_cast<double>(durationsUs.size() - 1));
        return durationsUs[idx];
    };

    double p50 = getPercentile(0.50);
    double p90 = getPercentile(0.90);
    double p99 = getPercentile(0.99);
    double p999 = getPercentile(0.999);

    double budgetUs = budgetMs * 1000.0;
    double maxBudgetUtilization = (maxVal / budgetUs) * 100.0;
    double p99BudgetUtilization = (p99 / budgetUs) * 100.0;

    std::cout << "\n========================================================\n";
    std::cout << "         PHASE 1: BASELINE CLEAN DSP BENCHMARK          \n";
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
    std::cout << "  Execution Latency Breakdown (Hot Path):\n";
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
                << "callbacks,underruns,min_us,mean_us,p50_us,p90_us,p99_us,p999_us,max_us,status\n";
    }

    std::vector<double> durationsUs;
    durationsUs.reserve(telemetryData.size());
    uint64_t underflowCount = 0;

    for (const auto& entry : telemetryData) {
        durationsUs.push_back(static_cast<double>(entry.duration_ns) / 1000.0);
        if (entry.is_underflow) underflowCount++;
    }

    std::sort(durationsUs.begin(), durationsUs.end());
    double sum = std::accumulate(durationsUs.begin(), durationsUs.end(), 0.0);
    double mean = sum / static_cast<double>(durationsUs.size());
    double minVal = durationsUs.front();
    double maxVal = durationsUs.back();

    auto getPercentile = [&](double p) -> double {
        size_t idx = static_cast<size_t>(p * static_cast<double>(durationsUs.size() - 1));
        return durationsUs[idx];
    };

    double p50 = getPercentile(0.50);
    double p90 = getPercentile(0.90);
    double p99 = getPercentile(0.99);
    double p999 = getPercentile(0.999);

    std::string stratName = (cfg.strategy == HeapPoisoner::Strategy::Strided) ? "Strided" : "ISMM26";
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
            << status << "\n";

    std::cout << "[Telemetry] Appended run results to " << csvPath << "\n";
}


int main(int argc, char* argv[]) {

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

    unsigned int defaultDevice = dac.getDefaultOutputDevice();
    RtAudio::DeviceInfo info = dac.getDeviceInfo(defaultDevice);
    std::cout << "Using Output Device: " << info.name << " (ID: " << defaultDevice << ")\n";

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

    // Allocate AudioContext on the Heap to prevent stack overflow from 1MB queue buffer
    auto ctx = std::make_unique<AudioContext>();
    ctx->stressHeap = cfg.stressHeap;
    ctx->numAllocationsPerCallback = cfg.numAllocationsPerCallback;

    unsigned int sampleRate = static_cast<unsigned int>(info.preferredSampleRate > 0 ? info.preferredSampleRate : 48000);
    ctx->sampleRate = static_cast<double>(sampleRate);

    unsigned int bufferFrames = 128; // 128 frames @ 48kHz = ~2.666 ms budget

    RtAudio::StreamParameters parameters;
    parameters.deviceId = defaultDevice;
    parameters.nChannels = 2;
    parameters.firstChannel = 0;

    RtAudio::StreamOptions options;
    options.flags = RTAUDIO_MINIMIZE_LATENCY;

    // Open Stream
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

    // Main thread consumer loop (drains the lock-free SPSC queue periodically)
    while (std::chrono::steady_clock::now() - benchmarkStart < benchmarkDuration) {
        CallbackTelemetry item;
        while (ctx->telemetryQueue.pop(item)) {
            collectedTelemetry.push_back(item);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // Drain any remaining telemetry items in the queue
    CallbackTelemetry item;
    while (ctx->telemetryQueue.pop(item)) {
        collectedTelemetry.push_back(item);
    }

    if (cfg.churnThreads > 0) {
        poisoner.stopBackgroundChurn();
    }

    // Stop and clean up audio stream
    dac.stopStream();
    if (dac.isStreamOpen()) {
        dac.closeStream();
    }

    // Process and print telemetry report
    printBenchmarkReport(collectedTelemetry, budgetMs);
    appendToCSV(cfg.csvPath, cfg, collectedTelemetry, budgetMs);

    return 0;
}