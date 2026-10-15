#include "memory.hpp"
#include "newlib_heap_geometry.hpp"

#include <pspkernel.h>
#include <pspsysmem.h>
#include <malloc.h>
#include <new>
#include <cstdio>
#include <cstring>

#include "diag.hpp"

namespace
{
volatile int gHandlingOom;
volatile unsigned int gLastNewBytes;
volatile unsigned int gLastNewCaller;
volatile unsigned int gHeapReservationBytes;
volatile int gHeapReservationUid;
constexpr unsigned int kHeapReservationUnknown = 1u << 14;
constexpr unsigned int kHeapGrowthGeometryUnknown = 1u << 15;

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

extern "C" SceUID __real_sceKernelAllocPartitionMemory(SceUID partitionid, const char *name, int type,
                                                        SceSize size, void *addr);

extern "C" SceUID __wrap_sceKernelAllocPartitionMemory(SceUID partitionid, const char *name, int type,
                                                        SceSize size, void *addr)
{
    // Observe newlib's existing reservation without changing its parameters,
    // allocation policy or return value.  This can run before main, so it
    // records only primitive BSS fields and performs no logging/allocation.
    const SceUID uid = __real_sceKernelAllocPartitionMemory(partitionid, name, type, size, addr);
    if (uid >= 0 && partitionid == 2 && type == PSP_SMEM_Low && name != nullptr &&
        std::strcmp(name, "heap_block") == 0)
    {
        __atomic_store_n(&gHeapReservationUid, uid, __ATOMIC_RELAXED);
        __atomic_store_n(&gHeapReservationBytes, static_cast<unsigned int>(size), __ATOMIC_RELEASE);
    }
    return uid;
}

extern "C" SceUID __psp_heap_blockid;

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

extern "C" Th06PspHeapSnapshot th06_psp_heap_geometry_snapshot()
{
    const th06::psp::NewlibHeapGeometrySnapshot geometry = th06::psp::CaptureNewlibHeapGeometry();
    Th06PspHeapSnapshot result{};
    result.arenaBytes = geometry.arenaBytes;
    result.usedBytes = geometry.usedBytes;
    result.freeBytes = geometry.freeBytes;
    result.topFreeBytes = geometry.topChunkBytes;
    result.freeBlocks = geometry.freeChunkCount;
    result.largestNoGrowRequestBytes = geometry.largestNoGrowRequestBytes;
    result.largestWithGrowRequestBytes = th06::psp::kNewlibHeapMetricUnknown;
    result.scanValid = geometry.scanValid;
    result.scanErrorFlags = geometry.scanErrorFlags;

    const unsigned int capacity = __atomic_load_n(&gHeapReservationBytes, __ATOMIC_ACQUIRE);
    const int uid = __atomic_load_n(&gHeapReservationUid, __ATOMIC_RELAXED);
    result.heapCapacityBytes = capacity;
    result.totalFreeBytes = th06::psp::kNewlibHeapMetricUnknown;
    // _sbrk's private heap_top is heap_block base + requested size.  mallinfo
    // arena is the portion consumed by dlmalloc, not that reservation size.
    // Add only the unused tail inside this block; kernel free is separate.
    if (capacity == 0 || uid != __psp_heap_blockid || geometry.arenaBytes > capacity)
    {
        result.heapCapacityBytes = th06::psp::kNewlibHeapMetricUnknown;
        result.scanValid = 0;
        result.scanErrorFlags |= kHeapReservationUnknown;
    }
    else if (geometry.scanValid)
    {
        result.totalFreeBytes = capacity - geometry.arenaBytes + geometry.freeBytes;
        const unsigned int blockBase = reinterpret_cast<unsigned int>(sceKernelGetBlockHeadAddr(uid));
        // _sbrk has no external callers in this executable; dlmalloc's
        // contiguous arena end is its break. The SDK's contiguous-growth
        // shortcut also requires that break to be page aligned. Do not claim
        // a growth-capable maximum if the audited assumptions cannot be read.
        if (geometry.heapBaseAddress != blockBase || (geometry.heapEndAddress & 4095u) != 0u)
        {
            result.scanValid = 0;
            result.scanErrorFlags |= kHeapGrowthGeometryUnknown;
        }
        else
        {
            const unsigned int growthRequest = th06_psp_largest_growth_request(
                capacity - geometry.arenaBytes, geometry.topPadBytes);
            result.largestWithGrowRequestBytes = growthRequest > geometry.largestNoGrowRequestBytes
                ? growthRequest : geometry.largestNoGrowRequestBytes;
        }
    }
    return result;
}

unsigned int th06_psp_largest_growth_request(unsigned int reservedTailBytes, unsigned int topPadBytes)
{
    if (reservedTailBytes == 0u)
        return 0u;
    const unsigned int largestDelta = (reservedTailBytes - 1u) & ~4095u;
    // At least a 16-byte request chunk plus the mandatory 16-byte top remains.
    if (static_cast<unsigned long long>(topPadBytes) + 32u > largestDelta)
        return 0u;
    const unsigned int largestChunk = (largestDelta - topPadBytes - 16u) & ~15u;
    // request2size(request) = max(16, (request + 4 + 15) & ~15).
    return largestChunk - 4u;
}

extern "C" void th06_psp_install_oom_handler()
{
    std::set_new_handler(OutOfMemoryHandler);
}

// ---- 2026-10-14 measurement: which allocation fails, and how large a block the
// heap can still hand out.  --wrap=malloc/--wrap=memalign route the game's calls
// here (libc-internal calls are not affected).  A failure is logged once per
// call with the request size and the heap state; nothing else changes.
extern "C" void *__real_malloc(size_t bytes);
extern "C" void *__real_memalign(size_t align, size_t bytes);
extern "C" void th06_psp_boot_notef(const char *fmt, ...);

static void NoteAllocFailure(const char *what, unsigned int align, unsigned int bytes)
{
    const struct mallinfo info = mallinfo();
    th06_psp_boot_notef("%s NG %uB A%u ARENA%uKB U%uKB F%uKB TOP%uKB", what, bytes, align,
                        (unsigned int)info.arena / 1024u, (unsigned int)info.uordblks / 1024u,
                        (unsigned int)info.fordblks / 1024u, (unsigned int)info.keepcost / 1024u);
}

extern "C" void *__wrap_malloc(size_t bytes)
{
    void *p = __real_malloc(bytes);
    if (p == NULL && bytes != 0)
        NoteAllocFailure("MALLOC", 0, (unsigned int)bytes);
    return p;
}

extern "C" void *__wrap_memalign(size_t align, size_t bytes)
{
    void *p = __real_memalign(align, bytes);
    if (p == NULL && bytes != 0)
        NoteAllocFailure("MEMALIGN", (unsigned int)align, (unsigned int)bytes);
    return p;
}

// Compatibility name for existing logs: this now reports the exact largest
// request available including audited _sbrk growth, rounded down to KiB. In contrast
// with the old malloc/free binary probe it never grows the arena or reorders
// free bins, and has no 2 MiB ceiling.  Detailed markers include MAXNG in bytes
// and MAXG in bytes, plus TOTAL (committed free and reserved tail) separately.
extern "C" unsigned int th06_psp_max_alloc_kb(void)
{
    const Th06PspHeapSnapshot heap = th06_psp_heap_geometry_snapshot();
    return heap.scanValid ? heap.largestWithGrowRequestBytes / 1024u : th06::psp::kNewlibHeapMetricUnknown;
}

#if defined(TH06_PSP_RECORD_LAYOUT)
extern "C" void th06_psp_dump_heap_layout(const char *point)
{
    // Fixed 16 KiB stack buffer, no allocator calls. Capture completes and
    // unlocks before the existing fixed-buffer/raw-sceIo logger is called.
    th06::psp::NewlibHeapLayoutRecord records[th06::psp::kNewlibHeapLayoutMaximumRecords];
    const th06::psp::NewlibHeapLayoutSnapshot layout =
        th06::psp::CaptureNewlibHeapLayout(records, th06::psp::kNewlibHeapLayoutMaximumRecords);
    const auto &geometry = layout.geometry;
    const char *const label = point ? point : "?";
    const auto u = [](std::uint32_t value) { return static_cast<unsigned int>(value); };
    th06_psp_boot_notef("HEAP LAYOUT BEGIN %s VALID%u FLAGS%08X BIN%u BASE%08X END%08X FIRST%08X TOP%08X PAD%uB",
                        label, u(layout.layoutValid), u(layout.layoutErrorFlags), u(geometry.scanValid),
                        u(geometry.heapBaseAddress), u(geometry.heapEndAddress), u(layout.firstChunkAddress),
                        u(layout.topChunkAddress), u(layout.frontPaddingBytes));
    th06_psp_boot_notef("HEAP LAYOUT TOTAL %s ARENA%uB USED%uB FREE%uB TOP%uB WALK%u ROWS%u FILTER%uB NEAR%uB",
                        label, u(geometry.arenaBytes), u(geometry.usedBytes), u(geometry.freeBytes),
                        u(geometry.topChunkBytes), u(layout.walkedChunkCount), u(layout.recordCount),
                        u(th06::psp::kNewlibHeapLayoutMinimumChunkBytes), u(th06::psp::kNewlibHeapLayoutNearTopBytes));
    for (unsigned int i = 0; i < layout.recordCount; ++i)
    {
        const auto &row = records[i];
        th06_psp_boot_notef("HEAP LAYOUT ROW %s %u C%08X U%08X SIZE%uB INUSE%u TOP%u", label, i,
                            u(row.chunkAddress), u(row.userAddress), u(row.chunkBytes),
                            (row.flags & th06::psp::kNewlibHeapLayoutInUse) ? 1u : 0u,
                            (row.flags & th06::psp::kNewlibHeapLayoutTop) ? 1u : 0u);
    }
    th06_psp_boot_notef("HEAP LAYOUT END %s %s FLAGS%08X", label,
                        layout.layoutValid ? "VALID" : "UNKNOWN", u(layout.layoutErrorFlags));
}
#endif
