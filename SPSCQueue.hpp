#include<atomic>
#include<array>
#include<new>

template <typename T, size_t Capacity>
class SPSCQueue {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");

public:
    SPSCQueue(): writeIndex_(0), readIndex_(0) {}

    //called only by producer
    bool push(const T& item){
        const size_t currentWrite = writeIndex_.load(std::memory_order_relaxed);
        const size_t currentRead = readIndex_.load(std::memory_order_acquire);

        //Check if queue is full
        if(currentWrite - currentRead >= Capacity) return false;

        buffer_[currentWrite & (Capacity - 1)] = item;
        writeIndex_.store(currentWrite + 1, std::memory_order_release);
        return true;
    }

    //called only by consumer
    bool pop(T& item){
        const size_t currentRead = readIndex_.load(std::memory_order_relaxed);
        const size_t currentWrite = writeIndex_.load(std::memory_order_acquire);

        //Check if queue is empty
        if(currentRead == currentWrite) return false;

        item = buffer_[currentRead & (Capacity - 1)];
        readIndex_.store(currentRead + 1, std::memory_order_release);
        return true;
    }

private:
    std::array<T, Capacity> buffer_{};

    // Align to 64 bytes (cache line) to avoid false sharing
    alignas(64) std::atomic<size_t> writeIndex_{0};
    alignas(64) std::atomic<size_t> readIndex_{0};
};
