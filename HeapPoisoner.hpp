#pragma once
#include <iostream>
#include <vector>
#include <iomanip>
#include <string>
#include <atomic>
#include <thread>
#include <algorithm>
#include <random>
#include <cstdlib>   // for malloc, free
#include <cstring>   // for memset (touching memory pages)
#include <malloc.h>  // for Linux/glibc mallinfo2()

class HeapPoisoner {

public:
    enum class Strategy {
        Strided, // baseline checkerboard
        AdversarialISMM26 // Van Kempen & Berger (ISMM'26)
    };

    struct Config {
        Strategy strategy = Strategy::AdversarialISMM26;

        // Nicolas' Algorithm 3 settings:
        size_t peakAllocations = 25000;
        double multiplier = 3.0;        // Adv3 setting 
        double occupancy = 0.66;        // 66% pinned in memory (Adv3 setting)
        unsigned int randomSeed = 42;   // Detereministic seed for reproducable shuffle

        // Strided settings:
        size_t totalAllocations = 80000; // total blocks to allocate in the setup phase
        size_t freeStride = 2; // Deallocation stride

        // A list of heterogeneous sizes (in bytes) spanning across:
        // - Tcache / Fastbins: 32B, 64B, 128B
        // - Small bins: 256B, 512B, 1024B
        // - Large bins & Page size: 2048B, 4096B, 8192B
        std::vector<size_t> sizeClasses = {32, 64, 128, 256, 512, 1024, 2048, 4096, 8192};
    };

    ~HeapPoisoner (){
        cleanup();
    }

    void fragmentHeap(const Config& config){
        if(config.strategy == Strategy::Strided){
            std::cout << "[HeapPoisoner] Starting heap fragmentation...\n";

            std::vector<void*> allAllocations;
            allAllocations.reserve(config.totalAllocations);
            
            // Allocate and touch
            for(size_t i=0; i<config.totalAllocations; ++i){
                // Pick a size
                size_t size = config.sizeClasses[i%config.sizeClasses.size()];

                // Allocate memory
                void* ptr = std::malloc(size);
                if(!ptr) continue;

                // Touch the memory so the OS maps physical RAM pages
                std::memset(ptr, 0xA5, size);

                allAllocations.push_back(ptr);
            }

            // Punching holes (Strided deallocation)
            pinnedBlocks_.reserve(config.totalAllocations / config.freeStride +1);

            for(size_t i=0; i<allAllocations.size(); ++i){
                if(i%config.freeStride == 0){
                    // Free this block which creates a hole in ptmalloc bins
                    std::free(allAllocations[i]);
                } else{
                    // Keep this block pinned to prevent coalescing
                    pinnedBlocks_.push_back(allAllocations[i]);
                }
            }

            std::cout << "[HeapPoisoner] Poisoning complete. Pinned blocks: " 
                    << pinnedBlocks_.size() << " | Freed holes: " 
                    << (allAllocations.size() - pinnedBlocks_.size()) << "\n";
        }
        else if(config.strategy == Strategy::AdversarialISMM26){
            std::cout << "[HeapPoisoner] Running ISMM '26 Adversarial Allocation (Algorithm 3)...\n";
            std::cout << "  - Multiplier: " << config.multiplier << " | Occupancy: " << (config.occupancy * 100.0) << "%\n";

            // Total allocations
            size_t n = static_cast<size_t>(config.multiplier * static_cast<double>(config.peakAllocations));

            std::vector<void*> allAllocations;
            allAllocations.reserve(n);

            //PRNG for sampling sizes and shuffling
            std::mt19937 rng(config.randomSeed);
            std::uniform_int_distribution<size_t> dist(0, config.sizeClasses.size() - 1);

            // Allocate and touch
            for(size_t i = 0; i < n; ++i){
                size_t size = config.sizeClasses[dist(rng)];
                void* ptr = std::malloc(size);
                if(ptr) {
                    std::memset(ptr, 0xA5, size);
                    allAllocations.push_back(ptr);
                }
            }

            // Shuffle pointers uniformly
            std::shuffle(allAllocations.begin(), allAllocations.end(), rng);

            // Free fraction (1 - occupancy)
            size_t numToFree = static_cast<size_t>(static_cast<double> (allAllocations.size())*(1.0 - config.occupancy));

            for(size_t i = 0; i < numToFree; ++i){
                std::free(allAllocations[i]);
            }

            // Retain remaining pointers as pinned blocks
            pinnedBlocks_.reserve(allAllocations.size() - numToFree);
            for(size_t i = numToFree; i < allAllocations.size(); ++i){
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

    void startBackgroundChurn(size_t numThreads = 2){
        if(churnRunning_.load()) return;
        churnRunning_.store(true);

        std::cout<< "[HeapPoisoner] Starting" << numThreads << " background heap churn thread(s)";

        for(size_t t = 0; t < numThreads; ++t){
            churnWorkers_.emplace_back([this, t]() {
                const size_t churnSizes[] = {64, 256, 1024, 4096, 16384, 65536};
                std::vector<void*> tempBlocks(16, nullptr);

                size_t counter = t;
                while(churnRunning_.load(std::memory_order_relaxed)){
                    // Allocate a batch of varied blocks
                    for(size_t i = 0; i<tempBlocks.size(); ++i){
                        size_t sz = churnSizes[(counter + i) % 6];
                        tempBlocks[i] = std::malloc(sz);
                        if(tempBlocks[i]) std::memset(tempBlocks[i], 0x55, sz); // Touch memory and cache
                    }

                    for(size_t i = 0; i <tempBlocks.size(); ++i){
                        if(tempBlocks[i]){
                            std::free(tempBlocks[i]);
                            tempBlocks[i] = nullptr;
                        }
                    }
                    counter++;

                    // Yield CPU briefly to prevent burning 100% CPU on worker cores
                    std::this_thread::yield(); 
                }

                // Cleanup any remaining temporary allocations
                for( void* p: tempBlocks){
                    if(p) std::free(p);
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