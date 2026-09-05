#pragma once
#include <iostream>
#include <vector>
#include <iomanip>
#include <string>
#include <atomic>
#include <thread>
#include <algorithm>
#include <random>
#include <cstdlib>
#include <cstring>
#include <malloc.h> // glibc mallinfo2() heap diagnostic API

class HeapPoisoner {

public:
    enum class Strategy {
        Strided,          // Baseline alternating checkerboard
        AdversarialISMM26 // Stochastic preconditioning (van Kempen & Berger, ISMM '26 Algorithm 3)
    };

    struct Config {
        Strategy strategy = Strategy::AdversarialISMM26;

        // ISMM '26 Algorithm 3 Parameters (Table 3):
        size_t peakAllocations = 25000;
        double multiplier = 3.0;      // Adv3 setting: Footprint expansion factor
        double occupancy = 0.66;      // Adv3 setting: Fraction of surviving live blocks
        unsigned int randomSeed = 42; // Seed for reproducible Mersenne Twister PRNG

        // Strided baseline parameters:
        size_t totalAllocations = 80000;
        size_t freeStride = 2;

        // Power-of-two size classes spanning tcache, fastbins, small bins, and large bins
        std::vector<size_t> sizeClasses = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
    };

    ~HeapPoisoner() {
        cleanup();
    }

    void fragmentHeap(const Config& config) {
        if (config.strategy == Strategy::Strided) {
            std::cout << "[HeapPoisoner] Starting heap fragmentation (Strided Checkerboard)...\n";

            std::vector<void*> allAllocations;
            allAllocations.reserve(config.totalAllocations);
            
            for (size_t i = 0; i < config.totalAllocations; ++i) {
                size_t size = config.sizeClasses[i % config.sizeClasses.size()];
                void* ptr = std::malloc(size);
                if (!ptr) continue;

                // memset forces kernel physical page mapping (avoids zero-page virtual-only allocation)
                std::memset(ptr, 0xA5, size);
                allAllocations.push_back(ptr);
            }

            pinnedBlocks_.reserve(config.totalAllocations / config.freeStride + 1);

            for (size_t i = 0; i < allAllocations.size(); ++i) {
                if (i % config.freeStride == 0) {
                    std::free(allAllocations[i]);
                } else {
                    // Pinned blocks maintain PREV_INUSE neighbor bits to suppress chunk coalescing
                    pinnedBlocks_.push_back(allAllocations[i]);
                }
            }

            std::cout << "[HeapPoisoner] Poisoning complete. Pinned blocks: " 
                      << pinnedBlocks_.size() << " | Freed holes: " 
                      << (allAllocations.size() - pinnedBlocks_.size()) << "\n";
        }
        else if (config.strategy == Strategy::AdversarialISMM26) {
            std::cout << "[HeapPoisoner] Running ISMM '26 Adversarial Allocation (Algorithm 3)...\n";
            std::cout << "  - Multiplier: " << config.multiplier << " | Occupancy: " << (config.occupancy * 100.0) << "%\n";

            size_t n = static_cast<size_t>(config.multiplier * static_cast<double>(config.peakAllocations));

            std::vector<void*> allAllocations;
            allAllocations.reserve(n);

            std::mt19937 rng(config.randomSeed);
            std::uniform_int_distribution<size_t> dist(0, config.sizeClasses.size() - 1);

            // Sample heterogeneous sizes and commit physical pages
            for (size_t i = 0; i < n; ++i) {
                size_t size = config.sizeClasses[dist(rng)];
                void* ptr = std::malloc(size);
                if (ptr) {
                    std::memset(ptr, 0xA5, size);
                    allAllocations.push_back(ptr);
                }
            }

            // Uniform Fisher-Yates shuffle across allocated addresses
            std::shuffle(allAllocations.begin(), allAllocations.end(), rng);

            // Deallocate (1 - occupancy) fraction; retain surviving blocks to break spatial cache locality
            size_t numToFree = static_cast<size_t>(static_cast<double>(allAllocations.size()) * (1.0 - config.occupancy));

            for (size_t i = 0; i < numToFree; ++i) {
                std::free(allAllocations[i]);
            }

            pinnedBlocks_.reserve(allAllocations.size() - numToFree);
            for (size_t i = numToFree; i < allAllocations.size(); ++i) {
                pinnedBlocks_.push_back(allAllocations[i]);
            }

            std::cout << "[HeapPoisoner] Adversarial preconditioning complete.\n"
                      << "  - Pinned (Live): " << pinnedBlocks_.size() 
                      << " | Scattered Holes (Freed): " << numToFree << "\n";
        }
    }
    
    void cleanup() {
        stopBackgroundChurn();
        if(pinnedBlocks_.empty()) return;
        for(void* ptr: pinnedBlocks_) std::free(ptr);
        pinnedBlocks_.clear();
        pinnedBlocks_.shrink_to_fit();
        std::cout << "[HeapPoisoner] Cleaned up all pinned allocations.\n";
    }

    void printHeapStats(const std::string& label) const {
        struct mallinfo2 info = mallinfo2();
        
        std::cout << "\n--- Heap Status: " << label << " ---\n";
        std::cout << "  - Total Allocated Space (uordblks) : " 
                    << std::fixed << std::setprecision(2) 
                    << (static_cast<double>(info.uordblks) / (1024.0 * 1024.0)) << " MB\n";
        std::cout << "  - Total Free Chunk Space (fordblks): " 
                    << (static_cast<double>(info.fordblks) / (1024.0 * 1024.0)) << " MB\n";
        std::cout << "  - Space via mmap (hblkhd)          : " 
                    << (static_cast<double>(info.hblkhd) / (1024.0 * 1024.0)) << " MB\n";
        std::cout << "  - Number of Free Chunks (ordblks)  : " << info.ordblks << "\n";
        std::cout << "-------------------------------------------\n";
    }

    // Multi-threaded churn simulating concurrent process activity (e.g. GUI rendering, sample streaming)
    void startBackgroundChurn(size_t numThreads = 2) {
        if (churnRunning_.load()) return;
        churnRunning_.store(true);

        std::cout << "[HeapPoisoner] Starting " << numThreads << " background heap churn thread(s)...\n";

        for (size_t t = 0; t < numThreads; ++t) {
            churnWorkers_.emplace_back([this, t]() {
                const size_t churnSizes[] = {64, 256, 1024, 4096, 16384, 65536};
                std::vector<void*> tempBlocks(16, nullptr);

                size_t counter = t;
                while (churnRunning_.load(std::memory_order_relaxed)) {
                    for (size_t i = 0; i < tempBlocks.size(); ++i) {
                        size_t sz = churnSizes[(counter + i) % 6];
                        tempBlocks[i] = std::malloc(sz);
                        if (tempBlocks[i]) std::memset(tempBlocks[i], 0x55, sz);
                    }

                    for (size_t i = 0; i < tempBlocks.size(); ++i) {
                        if (tempBlocks[i]) {
                            std::free(tempBlocks[i]);
                            tempBlocks[i] = nullptr;
                        }
                    }
                    counter++;

                    // Yield CPU to maintain scheduler fairness while maintaining continuous arena mutex contention
                    std::this_thread::yield();
                }

                for (void* p : tempBlocks) {
                    if (p) std::free(p);
                }
            });
        }
    }

    void stopBackgroundChurn(){
        if(!churnRunning_.load()) return;

        churnRunning_.store(false);
        for (auto& worker: churnWorkers_){
            if(worker.joinable()) worker.join();
        }
        churnWorkers_.clear();
        std::cout << "[HeapPoisoner] Stopped all background churn threads.\n";
    }

private:
    std::vector<void*> pinnedBlocks_;

    // Churn thread state
    std::atomic<bool> churnRunning_{false};
    std::vector<std::thread> churnWorkers_;
};