#pragma once

// newlib owns the PSP process heap.  sceKernelTotalFreeMemSize() only reports
// memory left outside that heap after PSP_HEAP_SIZE_KB has reserved its
// partition, so it cannot diagnose an in-process OOM.
struct Th06PspHeapSnapshot
{
    unsigned int arenaBytes;
    unsigned int usedBytes;
    unsigned int freeBytes;
    unsigned int topFreeBytes;
    unsigned int freeBlocks;

    // Detailed boundary-only capture.  Unlike mallinfo's committed free
    // count, totalFreeBytes includes the reserved heap tail not yet handed
    // to dlmalloc by _sbrk. largestNoGrowRequestBytes excludes growth;
    // largestWithGrowRequestBytes includes the audited _sbrk growth path.
    unsigned int heapCapacityBytes;
    unsigned int totalFreeBytes;
    unsigned int largestNoGrowRequestBytes;
    // Analytic largest single malloc request at this exact allocator state,
    // including PSPSDK _sbrk growth. Valid only when scanValid is set.
    unsigned int largestWithGrowRequestBytes;
    unsigned int scanValid;
    unsigned int scanErrorFlags;
};

// Pure arithmetic for audited newlib 4.5.0 / PSPSDK: _malloc_r requests
// ceil((request2size + topPad + 16)/4096)*4096 additional bytes, and _sbrk
// requires the resulting break to be strictly below heap_top.
unsigned int th06_psp_largest_growth_request(unsigned int reservedTailBytes, unsigned int topPadBytes);

extern "C"
{
    Th06PspHeapSnapshot th06_psp_heap_snapshot();
    Th06PspHeapSnapshot th06_psp_heap_geometry_snapshot();
    unsigned int th06_psp_max_alloc_kb();
    void th06_psp_install_oom_handler();
    void th06_psp_note_new_request(unsigned int bytes, unsigned int caller);
#if defined(TH06_PSP_RECORD_LAYOUT)
    // Diagnostic build only: capture into fixed stack storage under the
    // allocator lock, then format/log after unlocking. Never probes malloc.
    void th06_psp_dump_heap_layout(const char *point);
#endif
}
