#include "io61.hh"
#include <thread>
#include <vector>
#include <cstdio>
#include <cassert>
#include <sys/file.h>
#include <unistd.h>

// Tests that multiple readers can hold the lock simultaneously
void reader_thread(io61_file* f, int id) {
    (void) id;
    off_t off = 0;
    off_t len = 100;
    
    // Acquire the shared lock
    int r = io61_lock(f, off, len, LOCK_SH);
    assert(r == 0);
    
    // Hold the lock (will take about 100ms if shared, over 100ms if exclusive)
    usleep(100000);
    
    io61_unlock(f, off, len);
}

int main() {
    // make a file
    io61_file* f = io61_fdopen(STDOUT_FILENO, O_RDWR);
    
    // Test the readers 
    // 10 readers should take about 0.1s each if sequential
    double start = monotonic_timestamp();
    
    std::vector<std::thread> readers;
    readers.reserve(10); 

    // Make 10 threads, each holding a shared lock
    for (int i = 0; i < 10; ++i) {
        readers.emplace_back(reader_thread, f, i);
    }
    for (auto& t : readers) {
        t.join();
    }
    
    double end = monotonic_timestamp();
    double duration = end - start;
    printf("10 readers finished in %.3fs\n", duration);
    
    if (duration > 0.5) {
        printf("Test failed, readers took longer than 0.5s\n");
    } else {
        printf("Test passed, readers took under 0.5s (not sequential)\n");
    }

    io61_close(f);
    return 0;
}
