#include <iostream>
#include <random>
#include <chrono>
#include <sys/mman.h>
#include <unistd.h>

int main() {
    // 1. Define Constants
    const size_t GIGABYTE = 1024ULL * 1024ULL * 1024ULL;
    const size_t MAP_SIZE = 4ULL * GIGABYTE;
    const size_t PAGE_SIZE = 4096; // Standard 4KB page size
    const size_t NUM_PAGES = MAP_SIZE / PAGE_SIZE;

    std::cout << "Allocating 10 GB of anonymous memory..." << std::endl;

    // 2. Allocate Anonymous Memory via mmap
    // MAP_ANONYMOUS ensures it's not backed by a file.
    // MAP_PRIVATE ensures modifications are private to this process.
    void* addr = mmap(NULL, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (addr == MAP_FAILED) {
        std::cerr << "Memory allocation failed!" << std::endl;
        return 1;
    }

    std::cout << "Allocation successful at address: " << addr << std::endl;
    std::cout << "Total pages to sample from: " << NUM_PAGES << std::endl;
    std::cout << "Starting continuous random access loop (Press Ctrl+C to stop)..." << std::endl;

    // 3. Setup Fast Random Number Generator
    // We use a 64-bit Mersenne Twister for high-quality random distributions across the 10GB range
    std::mt19937_64 rng(std::chrono::steady_clock::now().time_since_epoch().count());
    std::uniform_int_distribution<size_t> dist(0, NUM_PAGES - 1);

    // Cast pointer to char* for easy byte/page math
    volatile char* memory_pool = static_cast<volatile char*>(addr);
    
    size_t access_count = 0;

    // 4. Continuous Random Access Loop
    while (true) {
        // Pick a random page index
        size_t random_page = dist(rng);
        
        // Calculate the exact byte offset for that page
        size_t byte_offset = random_page * PAGE_SIZE;

        // Write to the first byte of that page to force an allocation/TLB/Cache event
        // We use 'volatile' so the compiler doesn't optimize away this write
        memory_pool[byte_offset] = 'A'; 

        // Optional: Print a status update every 10 million accesses so you know it's alive
        if (++access_count % 10000000 == 0) {
            std::cout << "Performed " << access_count << " random page accesses." << std::endl;
        }
    }

    // 5. Cleanup (Unreachable in this infinite loop, but good practice)
    munmap(addr, MAP_SIZE);
    return 0;
}