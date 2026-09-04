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

// The Hard Real-Time Audio Callback (The Hot Path)
int audioCallback(void* outputBuffer, void* /*inputBuffer*/, unsigned int nBufferFrames,
                  double /*streamTime*/, RtAudioStreamStatus status, void* userData) {

    const auto startTime = std::chrono::steady_clock::now();
    auto* ctx = static_cast<AudioContext*>(userData);

    // --- HEAP ALLOCATION STRESS TEST ---
    if (ctx->stressHeap) {
        static const size_t stressSizes[4] = {512, 2048, 4096, 8192};
        void* scratchBlocks[4] = {nullptr, nullptr, nullptr, nullptr};

        for (size_t a = 0; a < 4; ++a) {
            // Allocate dynamically on the real-time audio thread
            scratchBlocks[a] = std::malloc(stressSizes[a]);
            if (scratchBlocks[a]) std::memset(scratchBlocks[a], 0, stressSizes[a]);
        }

        for(size_t a = 0; a < 4; ++a){
            if(scratchBlocks[a]) std::free(scratchBlocks[a]);
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

int main() {
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
    poisoner.printHeapStats("Baseline (Before Poisoning)");

    HeapPoisoner::Config poisonConfig;
    // poisonConfig.totalAllocations = 100000;
    // poisonConfig.freeStride = 2;

    poisonConfig.strategy = HeapPoisoner::Strategy::AdversarialISMM26;    

    poisoner.fragmentHeap(poisonConfig);
    poisoner.printHeapStats("After Heap Poisoning");

    // Allocate AudioContext on the Heap to prevent stack overflow from 1MB queue buffer
    auto ctx = std::make_unique<AudioContext>();

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

    poisoner.startBackgroundChurn(2);
    err = dac.startStream();
    if (err != RTAUDIO_NO_ERROR) {
        std::cerr << "[ERROR] Failed to start stream: " << dac.getErrorText() << "\n";
        return 1;
    }

    std::cout << ">>> Running Phase 3 Heap Stress Benchmark for 10 seconds...\n";
    std::vector<CallbackTelemetry> collectedTelemetry;
    collectedTelemetry.reserve(20000);

    const auto benchmarkStart = std::chrono::steady_clock::now();
    const auto benchmarkDuration = std::chrono::seconds(10);

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

    poisoner.stopBackgroundChurn();
    // Stop and clean up audio stream
    dac.stopStream();
    if (dac.isStreamOpen()) {
        dac.closeStream();
    }

    // Process and print telemetry report
    printBenchmarkReport(collectedTelemetry, budgetMs);

    return 0;
}