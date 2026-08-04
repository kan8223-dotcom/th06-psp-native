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
};

extern "C"
{
    Th06PspHeapSnapshot th06_psp_heap_snapshot();
    void th06_psp_install_oom_handler();
}
