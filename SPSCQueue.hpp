#pragma once
#include <atomic>
#include <array>
#include <cstddef>

// Lock-free, wait-free Single-Producer Single-Consumer (SPSC) ring buffer
// Provides bounded constant-time telemetry logging with zero hot-path dynamic memory allocations
template <typename T, size_t Capacity>
class SPSCQueue {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");

public:
    SPSCQueue() : writeIndex_(0), readIndex_(0) {}

    bool push(const T& item) {
        const size_t currentWrite = writeIndex_.load(std::memory_order_relaxed);
        const size_t currentRead = readIndex_.load(std::memory_order_acquire);

        if (currentWrite - currentRead >= Capacity) return false;

        buffer_[currentWrite & (Capacity - 1)] = item;
        writeIndex_.store(currentWrite + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        const size_t currentRead = readIndex_.load(std::memory_order_relaxed);
        const size_t currentWrite = writeIndex_.load(std::memory_order_acquire);

        if (currentRead == currentWrite) return false;

        item = buffer_[currentRead & (Capacity - 1)];
        readIndex_.store(currentRead + 1, std::memory_order_release);
        return true;
    }

private:
    std::array<T, Capacity> buffer_{};

    // Align to 64 bytes (L1 cache line) to eliminate false sharing between producer and consumer cores
    alignas(64) std::atomic<size_t> writeIndex_{0};
    alignas(64) std::atomic<size_t> readIndex_{0};
};
