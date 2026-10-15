// Adapted from TH08 PSP psp/newlib_heap_geometry; allocator ABI and
// validation are retained.  Capture is read-only under the newlib lock.
#pragma once

#include <cstdint>

namespace th06::psp
{
// A UINT32_MAX value is deliberately unambiguous in the text telemetry.  A
// zero-sized largest chunk is a valid result for an empty explicit arena, so
// it must not also mean that the private newlib ABI check failed.
constexpr std::uint32_t kNewlibHeapMetricUnknown = UINT32_MAX;

enum NewlibHeapScanError : std::uint32_t
{
    kNewlibHeapScanErrorNone = 0,
    kNewlibHeapScanErrorUnsupportedAbi = 1U << 0,
    kNewlibHeapScanErrorMissingSymbol = 1U << 1,
    kNewlibHeapScanErrorHeapRange = 1U << 2,
    kNewlibHeapScanErrorTopChunk = 1U << 3,
    kNewlibHeapScanErrorBinLink = 1U << 4,
    kNewlibHeapScanErrorChunkRange = 1U << 5,
    kNewlibHeapScanErrorChunkMetadata = 1U << 6,
    kNewlibHeapScanErrorFreeCount = 1U << 7,
    kNewlibHeapScanErrorFreeBytes = 1U << 8,
    kNewlibHeapScanErrorTraversalLimit = 1U << 9,
    kNewlibHeapScanErrorBinPlacement = 1U << 10,
    kNewlibHeapScanErrorBinBlock = 1U << 11,
    kNewlibHeapScanErrorLastRemainder = 1U << 12,
    kNewlibHeapScanErrorBinOrder = 1U << 13,
};

struct NewlibHeapGeometrySnapshot
{
    std::uint32_t arenaBytes;
    std::uint32_t usedBytes;
    std::uint32_t freeBytes;
    std::uint32_t topChunkBytes;
    std::uint32_t freeChunkCount;

    // Raw dlmalloc chunk size, including its boundary-tag overhead.  Use
    // largestNoGrowRequestBytes when deciding whether malloc(request) can be
    // satisfied without asking _sbrk_r to grow the heap.
    std::uint32_t largestFreeChunkBytes;
    std::uint32_t largestNoGrowRequestBytes;

    // Capture these with the same allocator lock: MAXG's analytic _sbrk
    // capacity depends on the current break and dynamic malloc top_pad.
    std::uint32_t heapBaseAddress;
    std::uint32_t heapEndAddress;
    std::uint32_t topPadBytes;

    std::uint32_t scanValid;
    std::uint32_t scanErrorCount;
    std::uint32_t scanErrorFlags;
};

// Scans the classic newlib dlmalloc bins/top once while its allocator lock is
// held.  The function performs no allocation, deallocation, stdio, mallinfo or
// sbrk operation and leaves allocator ordering intact.
NewlibHeapGeometrySnapshot CaptureNewlibHeapGeometry();

#if defined(TH06_PSP_RECORD_LAYOUT)
constexpr std::uint32_t kNewlibHeapLayoutMaximumRecords = 1024U;
constexpr std::uint32_t kNewlibHeapLayoutMinimumChunkBytes = 64U * 1024U;
constexpr std::uint32_t kNewlibHeapLayoutNearTopBytes = 4096U;

enum NewlibHeapLayoutRecordFlags : std::uint32_t
{
    kNewlibHeapLayoutInUse = 1U,
    kNewlibHeapLayoutTop = 2U,
};

enum NewlibHeapLayoutError : std::uint32_t
{
    // Low bits retain the existing bin-scanner error flags.
    kNewlibHeapLayoutInvalidArguments = 1U << 16,
    kNewlibHeapLayoutFirstChunk = 1U << 17,
    kNewlibHeapLayoutChunkMetadata = 1U << 18,
    kNewlibHeapLayoutRecordOverflow = 1U << 19,
    kNewlibHeapLayoutTraversalLimit = 1U << 20,
    kNewlibHeapLayoutAccounting = 1U << 21,
};

struct NewlibHeapLayoutRecord
{
    std::uint32_t chunkAddress;
    std::uint32_t userAddress;
    std::uint32_t chunkBytes;
    std::uint32_t flags;
};

struct NewlibHeapLayoutSnapshot
{
    NewlibHeapGeometrySnapshot geometry;
    std::uint32_t firstChunkAddress;
    std::uint32_t topChunkAddress;
    std::uint32_t frontPaddingBytes;
    std::uint32_t walkedChunkCount;
    std::uint32_t recordCount;
    std::uint32_t layoutValid;
    std::uint32_t layoutErrorFlags;
};

// Diagnostics only. The caller supplies a fixed stack buffer (1..1024 rows).
// Under one allocator lock, validate bins then walk all physical chunks;
// record only free/top, >=64 KiB and chunks intersecting the final 4 KiB.
// Any invalid metadata/overflow makes layoutValid zero; partial rows are then
// evidence only, not a complete layout. No alloc/free/stdio/mallinfo/sbrk.
NewlibHeapLayoutSnapshot CaptureNewlibHeapLayout(NewlibHeapLayoutRecord *records,
                                                std::uint32_t recordCapacity);
#endif
} // namespace th06::psp
