#include "kernel.hh"
#include "k-apic.hh"
#include "k-vmiter.hh"
#include "obj/k-firstprocess.h"
#include <atomic>

// kernel.cc
//
//    This is the kernel.


// INITIAL PHYSICAL MEMORY LAYOUT
//
//  +-------------- Base Memory --------------+
//  v                                         v
// +-----+--------------------+----------------+--------------------+---------/
// |     | Kernel      Kernel |       :    I/O | App 1        App 1 | App 2
// |     | Code + Data  Stack |  ...  : Memory | Code + Data  Stack | Code ...
// +-----+--------------------+----------------+--------------------+---------/
// 0  0x40000              0x80000 0xA0000 0x100000             0x140000
//                                             ^
//                                             | \___ PROC_SIZE ___/
//                                      PROC_START_ADDR

#define PROC_SIZE 0x40000       // initial state only

proc ptable[MAXNPROC];          // array of process descriptors
                                // Note that `ptable[0]` is never used.
proc* current;                  // pointer to currently executing proc

#define HZ 100                  // timer interrupt frequency (interrupts/sec)
[[maybe_unused]] static std::atomic<unsigned long> ticks; // # timer interrupts so far


// Memory state - see `kernel.hh`
physpageinfo physpages[NPAGES];


[[noreturn]] void schedule();
[[noreturn]] void run(proc* p);
void exception(regstate* regs);
uintptr_t syscall(regstate* regs);
void memshow();


// kernel_start(command)
//    Initialize the hardware and processes and start running. The `command`
//    string is an optional string passed from the boot loader.

static void process_setup(pid_t pid, const char* program_name);

void kernel_start(const char* command) {
    // initialize hardware
    init_hardware();
    log_printf("Starting WeensyOS\n");

    ticks = 1;
    init_timer(HZ);

    // clear screen
    console_clear();

    // (re-)initialize kernel page table
    for (uintptr_t addr = 0; addr < MEMSIZE_PHYSICAL; addr += PAGESIZE) {
        int perm = PTE_P | PTE_W; // Default to kernel only access
        if (addr == 0) {
            // nullptr is inaccessible even to the kernel
            perm = 0;
        }
        else if (addr == CONSOLE_ADDR) {
            // CGA console page is accessible to user
            perm = PTE_P | PTE_W | PTE_U;
        }
        else if (addr >= PROC_START_ADDR) {
            // User region is accessible to user
            perm = PTE_P | PTE_W | PTE_U;
        }
        // install identity mapping
        int r = vmiter(kernel_pagetable, addr).try_map(addr, perm);
        assert(r == 0); // mappings during kernel_start MUST NOT fail
                        // (Note that later mappings might fail!!)
    }


    // set up process descriptors
    for (pid_t i = 0; i < MAXNPROC; i++) {
        ptable[i].pid = i;
        ptable[i].state = P_FREE;
    }
    if (!command) {
        command = WEENSYOS_FIRST_PROCESS;
    }
    if (!program_image(command).empty()) {
        process_setup(1, command);
    } else {
        process_setup(1, "allocator");
        process_setup(2, "allocator2");
        process_setup(3, "allocator3");
        process_setup(4, "allocator4");
    }

    // switch to first process using run()
    run(&ptable[1]);
}


// kalloc(sz)
//    Kernel physical memory allocator. Allocates at least `sz` contiguous bytes
//    and returns a pointer to the allocated memory, or `nullptr` on failure.
//    The returned pointer’s address is a valid physical address, but since the
//    WeensyOS kernel uses an identity mapping for virtual memory, it is also a
//    valid virtual address that the kernel can access or modify.
//
//    The allocator selects from physical pages that can be allocated for
//    process use (so not reserved pages or kernel data), and from physical
//    pages that are currently unused (`physpages[N].refcount == 0`).
//
//    On WeensyOS, `kalloc` is a page-based allocator: if `sz > PAGESIZE`
//    the allocation fails; if `sz < PAGESIZE` it allocates a whole page
//    anyway.
//
//    The returned memory is initially filled with 0xCC, which corresponds to
//    the `int3` instruction. Executing that instruction will cause a `PANIC:
//    Unhandled exception 3!` This may help you debug.

void* kalloc(size_t sz) {
    if (sz > PAGESIZE) {
        return nullptr;
    }

    int pageno = 0;
    int page_increment = 1;
    // In the handout code, `kalloc` returns the first free page.
    // Alternate search strategies can be faster and/or expose bugs elsewhere.
    // This initialization returns a random free page:
    //     int pageno = rand(0, NPAGES - 1);
    // This initialization remembers the most-recently-allocated page and
    // starts the search from there:
    //     static int pageno = 0;
    // In Phase 4, you must change the allocation to use non-sequential pages.
    // The easiest way to do this is to set page_increment to 3, but you can
    // also set `pageno` randomly.

    for (int tries = 0; tries != NPAGES; ++tries) {
        uintptr_t pa = pageno * PAGESIZE;
        if (allocatable_physical_address(pa)
            && physpages[pageno].refcount == 0) {
            ++physpages[pageno].refcount;
            void* ptr = reinterpret_cast<void*>(pa);
            memset(ptr, 0xCC, PAGESIZE);
            return ptr;
        }
        pageno = (pageno + page_increment) % NPAGES;
    }

    return nullptr;
}


// kfree(kptr)
//    Free `kptr`, which must have been previously returned by `kalloc`.
//    If `kptr == nullptr` does nothing.

void kfree(void* kptr) {
    // Handle nullptr case
    if(kptr == nullptr) {
        return;
    }

    // Convert void* to physical address
    uintptr_t pa = reinterpret_cast<uintptr_t>(kptr);
    // Check if physical address is valid, throw error if not
    if(pa >= MEMSIZE_PHYSICAL || pa % PAGESIZE != 0) {
        return;
    }

    int pageno = pa / PAGESIZE;
    // Guard against double frees
    if (physpages[pageno].refcount == 0) {
        return; 
    }

    // Decrement reference count for the physical page
    if(physpages[pageno].refcount > 0) {
        --physpages[pageno].refcount;
    }
}


// process_setup(pid, program_name)
//    Load application program `program_name` as process number `pid`.
//    This loads the application's code and data into memory, sets its
//    %rip and %rsp, gives it a stack page, and marks it as runnable.

void process_setup(pid_t pid, const char* program_name) {
    proc* p = &ptable[pid];
    init_process(p, 0);

    // initialize process page table
    p->pagetable = kalloc_pagetable();
    assert(p->pagetable != nullptr);

    // Copy kernel region mappings
    for (vmiter kit(kernel_pagetable, 0); kit.va() < PROC_START_ADDR; kit.next()) {
        if (!kit.present()) {
            continue;
        }
        // Store virtual address, physical address, and mappings
        uintptr_t kva = kit.va();
        uintptr_t kpa = kit.pa();
        int kperm = kit.perm();

        // If not the console address, get rid of user permission
        if (kva != CONSOLE_ADDR) {
            kperm &= ~PTE_U;
        }

        // Copy values into the new table
        vmiter pit(p->pagetable, kva);
        pit.map(kpa, kperm);
    }

    program_image pgm(program_name);

    // Iterate over each segment
    for (auto seg = pgm.begin(); seg != pgm.end(); ++seg) {
        // Align with page size to ensure we are covering whole pages
        uintptr_t seg_lo = round_down(seg.va(), PAGESIZE);
        uintptr_t seg_hi = round_up(seg.va() + seg.size(), PAGESIZE);

        // Calculate permissions for this segment
        int uperm = PTE_P | PTE_U | (seg.writable() ? PTE_W : 0);

        // Iterate over each virtual address
        for (uintptr_t va = seg_lo; va < seg_hi; va += PAGESIZE) {
            // Allocate a page and zero it
            void* kpage = kalloc(PAGESIZE);
            assert(kpage != nullptr);
            memset(kpage, 0, PAGESIZE);

            // Map the page to this virtual address
            vmiter pit(p->pagetable, va);
            pit.map(kpage, uperm);

            // Find section that actually contains segment data
            uintptr_t copy_lo = va < seg.va() ? seg.va() : va;
            uintptr_t copy_hi = (va + PAGESIZE) < (seg.va() + seg.data_size()) ? (va + PAGESIZE) : (seg.va() + seg.data_size());
            // Copy initialized bytes into the process's page
            if (copy_hi > copy_lo) {
                char* dst = reinterpret_cast<char*>(kpage) + (copy_lo - va);
                assert(dst != nullptr);
                const char* src = seg.data() + (copy_lo - seg.va());
                memcpy(dst, src, copy_hi - copy_lo);
            }
        } 
    }
    // Allocate & map one user stack page at the same virtual address
    uintptr_t stack_addr = MEMSIZE_VIRTUAL - PAGESIZE;
    {
        void* kpage = kalloc(PAGESIZE);
        assert(kpage != nullptr);
        memset(kpage, 0, PAGESIZE);
        vmiter pit(p->pagetable, stack_addr);
        pit.map(kpage, PTE_P | PTE_W | PTE_U);
        p->regs.reg_rsp = stack_addr + PAGESIZE;
    }

    // Set entry point and mark runnable
    p->regs.reg_rip = pgm.entry();
    p->state = P_RUNNABLE;
}


// exception(regs)
//    Exception handler (for interrupts, traps, and faults).
//
//    The register values from exception time are stored in `regs`.
//    The processor responds to an exception by saving application state on
//    the kernel's stack, then jumping to kernel assembly code (in
//    k-exception.S). That code saves more registers on the kernel's stack,
//    then calls exception().
//
//    Note that hardware interrupts are disabled when the kernel is running.

void exception(regstate* regs) {
    // Copy the saved registers into the `current` process descriptor.
    current->regs = *regs;
    regs = &current->regs;

    // It can be useful to log events using `log_printf`.
    // Events logged this way are stored in the host's `log.txt` file.
    /* log_printf("proc %d: exception %d at rip %p\n",
                current->pid, regs->reg_intno, regs->reg_rip); */

    // Show the current cursor location and memory state
    // (unless this is a kernel fault).
    console_show_cursor();
    if (regs->reg_intno != INT_PF || (regs->reg_errcode & PTE_U)) {
        memshow();
    }

    // If Control-C was typed, exit the virtual machine.
    check_keyboard();


    // Actually handle the exception.
    switch (regs->reg_intno) {

    case INT_IRQ + IRQ_TIMER:
        ++ticks;
        lapicstate::get().ack();
        schedule();
        break;                  /* will not be reached */

    case INT_PF: {
        // Analyze faulting address and access type.
        uintptr_t addr = rdcr2();
        const char* operation = regs->reg_errcode & PTE_W
                ? "write" : "read";
        const char* problem = regs->reg_errcode & PTE_P
                ? "protection problem" : "missing page";

        if (!(regs->reg_errcode & PTE_U)) {
            proc_panic(current, "Kernel page fault on %p (%s %s, rip=%p)!\n",
                       addr, operation, problem, regs->reg_rip);
        }
        error_printf("PAGE FAULT on %p (pid %d, %s %s, rip=%p)!\n",
                     addr, current->pid, operation, problem, regs->reg_rip);
        log_print_backtrace(current);
        current->state = P_FAULTED;
        break;
    }

    default:
        proc_panic(current, "Unhandled exception %d (rip=%p)!\n",
                   regs->reg_intno, regs->reg_rip);

    }


    // Return to the current process (or run something else).
    if (current->state == P_RUNNABLE) {
        run(current);
    } else {
        schedule();
    }
}


int syscall_page_alloc(uintptr_t addr);
int syscall_fork();
void sys_exit();


// syscall(regs)
//    Handle a system call initiated by a `syscall` instruction.
//    The process’s register values at system call time are accessible in
//    `regs`.
//
//    If this function returns with value `V`, then the user process will
//    resume with `V` stored in `%rax` (so the system call effectively
//    returns `V`). Alternately, the kernel can exit this function by
//    calling `schedule()`, perhaps after storing the eventual system call
//    return value in `current->regs.reg_rax`.
//
//    It is only valid to return from this function if
//    `current->state == P_RUNNABLE`.
//
//    Note that hardware interrupts are disabled when the kernel is running.

uintptr_t syscall(regstate* regs) {
    // Copy the saved registers into the `current` process descriptor.
    current->regs = *regs;
    regs = &current->regs;

    // It can be useful to log events using `log_printf`.
    // Events logged this way are stored in the host's `log.txt` file.
    /* log_printf("proc %d: syscall %d at rip %p\n",
                  current->pid, regs->reg_rax, regs->reg_rip); */

    // Show the current cursor location and memory state.
    console_show_cursor();
    memshow();

    // If Control-C was typed, exit the virtual machine.
    check_keyboard();


    // Actually handle the exception.
    switch (regs->reg_rax) {

    case SYSCALL_PANIC:
        user_panic(current);
        break; // will not be reached

    case SYSCALL_GETPID:
        return current->pid;

    case SYSCALL_YIELD:
        current->regs.reg_rax = 0;
        schedule();             // does not return

    case SYSCALL_PAGE_ALLOC:
        return syscall_page_alloc(current->regs.reg_rdi);
    
    case SYSCALL_FORK:
        return syscall_fork();

    case SYSCALL_EXIT:
        sys_exit();
        break;

    default:
        proc_panic(current, "Unhandled system call %ld (pid=%d, rip=%p)!\n",
                   regs->reg_rax, current->pid, regs->reg_rip);

    }

    panic("Should not get here!\n");
}


// syscall_page_alloc(addr)
//    Handles the SYSCALL_PAGE_ALLOC system call. This function
//    should implement the specification for `sys_page_alloc`
//    in `u-lib.hh` (but in the handout code, it does not).

int syscall_page_alloc(uintptr_t addr) {
    // Check that address is in user region and properly aligned
    if (addr < PROC_START_ADDR || addr >= MEMSIZE_VIRTUAL || addr % PAGESIZE != 0) {
        // Return with an error
        return -1;
    }

    vmiter it(current->pagetable, addr);

    // Allocate a physical page
    void* kpage = kalloc(PAGESIZE);
    if (kpage == nullptr) {
        return -1;
    }
    memset(kpage, 0, PAGESIZE);

    // If there was an old mapping, free it and unmap
    if (it.present() && it.user() && it.va() != CONSOLE_ADDR) {
        kfree(it.kptr<void*>());       
        it.map(it.pa(), 0);            
    }

    // Install new mapping
    int r = it.try_map(kpage, PTE_P | PTE_W | PTE_U);
    if (r != 0) {
        // If mapping fails, free the page
        kfree(kpage);
        return -1;
    }
    return 0;
}
/// Helper function for handling page frees
static void free_pagetable_and_pages(proc* free_proc) {
    // Unmap & free all user pages
    for (vmiter it(free_proc->pagetable, PROC_START_ADDR); !it.done(); it.next()) {
        if (it.present() && it.user() && it.va() != CONSOLE_ADDR) {
            kfree(it.kptr<void*>());
            it.map(it.pa(), 0);
        }
    }
    // Free page table pages
    for (ptiter pt(free_proc->pagetable); !pt.done(); pt.next()) {
        kfree(pt.kptr());
    }
    kfree(free_proc->pagetable);
    free_proc->pagetable = nullptr;
    free_proc->state = P_FREE;
}

int syscall_fork() {
    // Find a free slot
    pid_t free_pid = 0;
    for(pid_t i {1}; i < MAXNPROC; ++i) {
        if(ptable[i].state == P_FREE) {
            free_pid = i;
            break;
        }
    }
    // If no free slot found, return error
    if(free_pid == 0) {
        return -1;
    }

    proc* free_proc = &ptable[free_pid];
    proc* current_proc = current;

    // Initialize the new process
    init_process(free_proc, 0);

    // Allocate a new page table for the new process
    free_proc->pagetable = kalloc_pagetable();
    // If allocation fails, return error
    if(free_proc->pagetable == nullptr) {
        return -1;
    }

    // Copy kernel region mappings (same as in process_setup)
    for (vmiter kit(kernel_pagetable, 0); kit.va() < PROC_START_ADDR; kit.next()) {
        if (!kit.present()) {
            continue;
        }
        
        uintptr_t kva = kit.va();
        uintptr_t kpa = kit.pa();
        int kperm = kit.perm();
        
        if (kva != CONSOLE_ADDR) {
            kperm &= ~PTE_U;
        }
        
        vmiter cit(free_proc->pagetable, kva);
        int r = cit.try_map(kpa, kperm);
        if (r != 0) {
            free_pagetable_and_pages(free_proc);
            return -1;
        }
    }
    
    // Copy user region mappings (need separate physical pages)
    for (vmiter pit(current_proc->pagetable, PROC_START_ADDR); !pit.done(); pit.next()) {
        if (!pit.present() || !pit.user()) {
            continue;
        }
        
        uintptr_t va = pit.va();
        uintptr_t pa = pit.pa();
        int perm = pit.perm();

        // Copy page if in user region, writable, and not the console
        if (va >= PROC_START_ADDR && (perm & PTE_U) && pit.writable() && va != CONSOLE_ADDR) {
            // Allocate new physical page for child
            void* new_page = kalloc(PAGESIZE);
            // If allocation fails, clean up and return error
            if (new_page == nullptr) {
                free_pagetable_and_pages(free_proc);
                return -1;
            }
                        
            // Copy data from parent to child
            memcpy(new_page, reinterpret_cast<void*>(pa), PAGESIZE);
            
            // Map new page in child's page table
            vmiter cit(free_proc->pagetable, va);
            int r = cit.try_map(new_page, perm);
            if (r != 0) {
                free_pagetable_and_pages(free_proc);
                return -1;
            }
        } else {
            // Share read-only/kernel pages
            vmiter cit(free_proc->pagetable, va);
            int r = cit.try_map(pa, perm);
            if (r != 0) {
                // Cleanup and return error
                free_pagetable_and_pages(free_proc);
                return -1;
            }
            // Bump the refcount
            ++physpages[pa / PAGESIZE].refcount;
        }
    }
    
    // Copy current register's to forked process
    free_proc->regs = current_proc->regs;
    free_proc->regs.reg_rax = 0;
    free_proc->state = P_RUNNABLE;
    
    return free_pid;
}

void sys_exit() {
    proc* p = current;

    // Free all user pages
    for (vmiter it(p->pagetable, PROC_START_ADDR); !it.done(); it.next()) {
        if (it.present() && it.user() && it.va() != CONSOLE_ADDR) {
            // free physical page
            kfree(it.kptr<void*>());
            // unmap the virtual addresses
            it.map(it.pa(), 0);
        }
    }
    // Free all page tables
    for (ptiter pt(p->pagetable); !pt.done(); pt.next()) {
        kfree(pt.kptr());   
    }
    kfree(p->pagetable);     
    
    // Mark process as free
    p->state = P_FREE;
    p->pagetable = nullptr;
    
    // Schedule another process
    schedule();
}

// schedule
//    Pick the next process to run and then run it.
//    If there are no runnable processes, spins forever.

void schedule() {
    pid_t pid = current->pid;
    for (unsigned spins = 1; true; ++spins) {
        pid = (pid + 1) % MAXNPROC;
        if (ptable[pid].state == P_RUNNABLE) {
            run(&ptable[pid]);
        }

        // If Control-C was typed, exit the virtual machine.
        check_keyboard();

        // If spinning forever, show the memviewer.
        if (spins % (1 << 12) == 0) {
            memshow();
        }
    }
}


// run(p)
//    Run process `p`. This involves setting `current = p` and calling
//    `exception_return` to restore its page table and registers.

void run(proc* p) {
    assert(p->state == P_RUNNABLE);
    current = p;

    // Check the process's current registers.
    check_process_registers(p);

    // Check the process's current pagetable.
    check_pagetable(p->pagetable);

    // This function is defined in k-exception.S. It restores the process's
    // registers then jumps back to user mode.
    exception_return(p);

    // should never get here
    while (true) {
    }
}


// memshow()
//    Draw a picture of memory (physical and virtual) on the CGA console.
//    Switches to a new process's virtual memory map every 0.25 sec.
//    Uses `console_memviewer()`, a function defined in `k-memviewer.cc`.

void memshow() {
    static unsigned last_ticks = 0;
    static int showing = 0;

    // switch to a new process every 0.25 sec
    if (last_ticks == 0 || ticks - last_ticks >= HZ / 2) {
        last_ticks = ticks;
        showing = (showing + 1) % MAXNPROC;
    }

    proc* p = nullptr;
    for (int search = 0; !p && search < MAXNPROC; ++search) {
        if (ptable[showing].state != P_FREE
            && ptable[showing].pagetable) {
            p = &ptable[showing];
        } else {
            showing = (showing + 1) % MAXNPROC;
        }
    }

    console_memviewer(p);
    if (!p) {
        console_printf(CPOS(10, 26), CS_WHITE "   VIRTUAL ADDRESS SPACE\n"
            "                          [All processes have exited]\n"
            "\n\n\n\n\n\n\n\n\n\n\n");
    }
}
