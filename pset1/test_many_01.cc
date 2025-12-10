#include "m61.hh"
#include <cstdio>
#include <vector>
#include <cassert>

// Test allocating many blocks that, when combined, are over 8 MiB.

int main() {
    std::vector<void*> ptrs;
    // 100 KB blocks
    size_t block_sz = 100 << 10;
    // Total 100 * 100 KB = 10 MB
    int count = 100;

    // Allocate the 100 blocks
    for (int i = 0; i < count; ++i) {
        void* p = m61_malloc();
        // Check that allocation was successful for each
        if (!p) {
             return 1;
        }
        ptrs.push_back(p);
    }
    
    m61_statistics stats = m61_get_statistics();

    // Checks that all blocks were properly allocated
    if (stats.ntotal != (unsigned long long)count) {
         fprintf(stderr, "ntotal is %llu, expected %d\n", stats.ntotal, count);
         return 1;
    }
    
    for (void* p : ptrs) {
        m61_free(p);
    }
    
    stats = m61_get_statistics();
    // Checks that all blocks were properly freed

    if (stats.nactive != 0) {
         fprintf(stderr, "nactive is %llu after free, expected 0\n", stats.nactive);
         return 1;
    }
    
    m61_print_statistics();
}

//! alloc count: active          0   total        100   fail          0
//! alloc size:  active          0   total   10240000   fail          0
