#include "memory.hpp"

#include <pspkernel.h>
#include <malloc.h>
#include <new>
#include <cstdio>

#include "diag.hpp"

namespace
{
volatile int gHandlingOom;
volatile unsigned int gLastNewBytes;
volatile unsigned int gLastNewCaller;

void OutOfMemoryHandler()
{
    // operator new retries after a new_handler returns.  This handler never
    // returns: one thread records the allocator state and leaves cleanly,
    // while any concurrent failure waits for that process exit.
    if (__atomic_exchange_n(&gHandlingOom, 1, __ATOMIC_ACQ_REL) != 0)
    {
        for (;;)
            sceKernelDelayThread(1000 * 1000);
    }

    const Th06PspHeapSnapshot heap = th06_psp_heap_snapshot();
    const unsigned int requested = __atomic_load_n(&gLastNewBytes, __ATOMIC_ACQUIRE);
    const unsigned int caller = __atomic_load_n(&gLastNewCaller, __ATOMIC_RELAXED);
    th06_psp_boot_notef("CXX OOM REQ%uB PC%08X A%uKB U%uKB F%uKB TOP%uKB B%u", requested, caller,
                        heap.arenaBytes / 1024u, heap.usedBytes / 1024u, heap.freeBytes / 1024u,
                        heap.topFreeBytes / 1024u, heap.freeBlocks);

    char detail[52];
    std::snprintf(detail, sizeof(detail), "REQ %uKB FREE %uKB TOP %uKB", requested / 1024u,
                  heap.freeBytes / 1024u, heap.topFreeBytes / 1024u);
    th06_psp_fatal_exit("OUT OF MEMORY", detail);
}
}

extern "C" void th06_psp_note_new_request(unsigned int bytes, unsigned int caller)
{
    // Keep allocation diagnostics entirely in RAM.  The new-handler runs
    // synchronously on the failing call, so it can report the request without
    // putting any Memory Stick I/O on the allocation hot path.
    __atomic_store_n(&gLastNewCaller, caller, __ATOMIC_RELAXED);
    __atomic_store_n(&gLastNewBytes, bytes, __ATOMIC_RELEASE);
}

extern "C" Th06PspHeapSnapshot th06_psp_heap_snapshot()
{
    const struct mallinfo info = mallinfo();
    Th06PspHeapSnapshot result{};
    result.arenaBytes = static_cast<unsigned int>(info.arena);
    result.usedBytes = static_cast<unsigned int>(info.uordblks);
    result.freeBytes = static_cast<unsigned int>(info.fordblks);
    result.topFreeBytes = static_cast<unsigned int>(info.keepcost);
    result.freeBlocks = static_cast<unsigned int>(info.ordblks);
    return result;
}

extern "C" void th06_psp_install_oom_handler()
{
    std::set_new_handler(OutOfMemoryHandler);
}
