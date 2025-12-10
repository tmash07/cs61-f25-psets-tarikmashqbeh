#include "m61.hh"
#include <cstdio>
#include <vector>
#include <cassert>
#include <algorithm>

// Test buddy allocator (splitting and coalescing)

int main() {
    // Allocate minimum size block (requires splitting)
    void* p1 = m61_malloc(1); 
    assert(p1); // Check allocation was successful
    
    // Check that there is only 1 active allocation
    m61_statistics stats = m61_get_statistics();
    if (stats.nactive != 1) {
        fprintf(stderr, "nactive is %llu, expected 1\n", stats.nactive);
        return 1;
    }
    
    // Allocate another min size block (should be buddy of p1)
    void* p2 = m61_malloc(1);
    assert(p2); // Check allocation was successful
    
    // Allocate large block (Order 22). 
    size_t sz3 = 4 << 20; 
    void* p3 = m61_malloc(sz3 - 100); // Check that malloc rounds up to next order.
    assert(p3); // Check allocation was successful
    
    // Check that free is successful
    m61_free(p1);
    stats = m61_get_statistics();
    if (stats.nactive != 2) {
        fprintf(stderr, "nactive is %llu after free p1, expected 2\n", stats.nactive);
        return 1;
    }
    
    // Trigger coalescing by freeing other (buddy) allocations
    m61_free(p2);
    m61_free(p3);
    
    // Check that all allocations are freed
    stats = m61_get_statistics();
    if (stats.nactive != 0) {
        fprintf(stderr, "nactive is %llu after freeing all, expected 0\n", stats.nactive);
        return 1;
    }
    
    // Allocate large block to check if coalescing worke
    void* p4 = m61_malloc(7 << 20);
    if (!p4) {
        fprintf(stderr, "Coalescing failed\n");
        return 1;
    }
    m61_free(p4);

    m61_print_statistics();
}

//! alloc count: active          0   total          4   fail          0
//! alloc size:  active          0   total        ???   fail          0
