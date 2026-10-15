// Adapted from TH08 PSP psp/newlib_heap_geometry; allocator ABI and
// validation are retained.  Capture is read-only under the newlib lock.
#include "newlib_heap_geometry.hpp"

#include <_newlib_version.h>
#include <malloc.h>
#include <reent.h>

#include <cstddef>
#include <cstdint>

#if TH06_PSP_NEWLIB_4_5_0_DLMALLOC == 1 && __NEWLIB__ == 4 && \
    __NEWLIB_MINOR__ == 5 && __NEWLIB_PATCHLEVEL__ == 0 && \
    TH06_PSP_NEWLIB_MALLOC_ALIGNMENT == 16
// Keep the C-linkage declarations outside the anonymous namespace so their
// weak references resolve to the allocator's actual ELF symbols.
extern "C" void *__malloc_av_[] __attribute__((weak));
extern "C" char *__malloc_sbrk_base __attribute__((weak));
extern "C" struct mallinfo __malloc_current_mallinfo __attribute__((weak));
extern "C" std::size_t __malloc_top_pad __attribute__((weak));
#define TH06_PSP_NEWLIB_HEAP_SCAN_SUPPORTED 1
#else
#define TH06_PSP_NEWLIB_HEAP_SCAN_SUPPORTED 0
#endif

namespace
{
using th06::psp::NewlibHeapGeometrySnapshot;

constexpr std::size_t kNav = 128;
constexpr std::size_t kSizeBytes = 4;
constexpr std::size_t kMallocAlignment = TH06_PSP_NEWLIB_MALLOC_ALIGNMENT;
constexpr std::size_t kMallocAlignmentMask = kMallocAlignment - 1U;
constexpr std::size_t kMinChunkBytes = 16;
constexpr std::size_t kPrevInUse = 0x1U;
constexpr std::size_t kIsMmapped = 0x2U;
constexpr std::size_t kChunkSizeBits = kPrevInUse | kIsMmapped;
constexpr std::size_t kMaximumSmallBinSize = 512;
constexpr std::size_t kBinBlockWidth = 4;
constexpr std::size_t kMaximumPspUserRamBytes = 64U * 1024U * 1024U;
constexpr std::uintptr_t kPspCachedRamBegin = 0x08000000U;
constexpr std::uintptr_t kPspCachedRamEnd = 0x0c000000U;
// A valid TH06 heap is orders of magnitude below this.  Capping corrupt list
// traversal is preferable to spending seconds walking a cycle on a 333 MHz
// PSP; reaching the cap is reported as unknown, never as a smaller estimate.
constexpr std::uint32_t kMaximumScannableBinNodes = 256U * 1024U;

struct MallocChunk
{
    std::size_t prevSize;
    std::size_t size;
    MallocChunk *fd;
    MallocChunk *bk;
};

static_assert(__NEWLIB__ == 4 && __NEWLIB_MINOR__ == 5 && __NEWLIB_PATCHLEVEL__ == 0,
              "newlib private heap scanner must be re-audited for this SDK version");
static_assert(TH06_PSP_NEWLIB_4_5_0_DLMALLOC == 1,
              "newlib private heap scanner requires the audited classic dlmalloc ABI");
static_assert(sizeof(std::size_t) == kSizeBytes, "audited dlmalloc ABI is 32-bit");
static_assert(sizeof(void *) == kSizeBytes, "audited dlmalloc ABI is 32-bit");
static_assert(sizeof(MallocChunk) == kMinChunkBytes, "unexpected malloc_chunk layout");
static_assert(sizeof(struct mallinfo) == 10U * kSizeBytes, "unexpected mallinfo layout");
static_assert(offsetof(struct mallinfo, arena) == 0U, "unexpected mallinfo arena offset");
static_assert(offsetof(struct mallinfo, hblks) == 3U * kSizeBytes,
              "unexpected mallinfo hblks offset");
static_assert(offsetof(struct mallinfo, hblkhd) == 4U * kSizeBytes,
              "unexpected mallinfo hblkhd offset");
static_assert(offsetof(MallocChunk, fd) == 2U * kSizeBytes, "unexpected fd offset");
static_assert(offsetof(MallocChunk, bk) == 3U * kSizeBytes, "unexpected bk offset");
static_assert(kMallocAlignment == 16U, "linked PSPSDK allocator uses 16-byte rounding");
static_assert((kMallocAlignment & kMallocAlignmentMask) == 0U,
              "malloc alignment must be a power of two");

constexpr std::size_t RequestToChunkBytes(std::size_t request)
{
    const std::size_t padded = (request + kSizeBytes + kMallocAlignmentMask) &
                               ~kMallocAlignmentMask;
    return padded < kMinChunkBytes ? kMinChunkBytes : padded;
}

constexpr std::size_t LargestRequestForBinChunk(std::size_t chunkBytes)
{
    const std::size_t aligned = chunkBytes & ~kMallocAlignmentMask;
    return aligned >= kMinChunkBytes ? aligned - kSizeBytes : 0U;
}

constexpr std::size_t LargestRequestForTopChunk(std::size_t chunkBytes)
{
    if (chunkBytes < kMinChunkBytes)
        return 0U;
    const std::size_t available = (chunkBytes - kMinChunkBytes) & ~kMallocAlignmentMask;
    return available >= kMinChunkBytes ? available - kSizeBytes : 0U;
}

// Compile-time synthetic boundary cases for the exact request2size arithmetic
// observed in the linked PSPSDK 4.5.0 _malloc_r (request+19, low 4 bits clear).
static_assert(RequestToChunkBytes(0U) == 16U, "zero request rounding changed");
static_assert(RequestToChunkBytes(1U) == 16U, "small request rounding changed");
static_assert(RequestToChunkBytes(16U) == 32U, "alignment boundary changed");
static_assert(RequestToChunkBytes(143544U) == 143552U, "GUI allocation rounding changed");
static_assert(LargestRequestForBinChunk(16U) == 12U, "minimum bin capacity changed");
static_assert(LargestRequestForTopChunk(24U) == 0U, "top must retain MINSIZE");
static_assert(LargestRequestForTopChunk(32U) == 12U, "minimum top split changed");
static_assert(LargestRequestForTopChunk(61048U) == 61020U,
              "unaligned top-chunk capacity changed");

constexpr std::size_t BinIndexForChunk(std::size_t chunkBytes)
{
    const std::size_t shifted = chunkBytes >> 9U;
    return shifted == 0U      ? (chunkBytes >> 3U)
           : shifted <= 4U    ? 56U + (chunkBytes >> 6U)
           : shifted <= 20U   ? 91U + (chunkBytes >> 9U)
           : shifted <= 84U   ? 110U + (chunkBytes >> 12U)
           : shifted <= 340U  ? 119U + (chunkBytes >> 15U)
           : shifted <= 1364U ? 124U + (chunkBytes >> 18U)
                              : 126U;
}

static_assert(BinIndexForChunk(16U) == 2U, "minimum small-bin index changed");
static_assert(BinIndexForChunk(496U) == 62U, "small-bin index changed");
static_assert(BinIndexForChunk(504U) == 63U, "last small-bin index changed");
static_assert(BinIndexForChunk(512U) == 64U, "first large-bin index changed");
static_assert(BinIndexForChunk(0x7ffffff0U) == 126U, "terminal bin index changed");

std::uint32_t gHeapScanErrorCount = 0;

bool AddAddress(std::uintptr_t base, std::size_t amount, std::uintptr_t *result)
{
    if (amount > UINTPTR_MAX - base)
        return false;
    *result = base + amount;
    return true;
}

bool IsAlignedChunkAddress(std::uintptr_t address)
{
    std::uintptr_t memoryAddress = 0;
    return AddAddress(address, 2U * kSizeBytes, &memoryAddress) &&
           (memoryAddress & kMallocAlignmentMask) == 0U;
}

bool IsBinChunkAddress(std::uintptr_t address, std::uintptr_t heapBase,
                       std::uintptr_t topAddress)
{
    if (address < heapBase || address >= topAddress || !IsAlignedChunkAddress(address))
        return false;
    return topAddress - address >= sizeof(MallocChunk);
}

#if TH06_PSP_NEWLIB_HEAP_SCAN_SUPPORTED
MallocChunk *BinHeader(std::size_t index)
{
    // newlib bin_at(i): &av_[2*i+2] - 2*SIZE_SZ.  Pointer and SIZE_SZ are
    // both four bytes in this ABI, so the header begins at av_[2*i].
    return reinterpret_cast<MallocChunk *>(&__malloc_av_[2U * index]);
}

MallocChunk *BinForward(std::size_t index)
{
    return static_cast<MallocChunk *>(__malloc_av_[2U * index + 2U]);
}

MallocChunk *BinBackward(std::size_t index)
{
    return static_cast<MallocChunk *>(__malloc_av_[2U * index + 3U]);
}

MallocChunk *ForwardLink(MallocChunk *node, MallocChunk *header, std::size_t index)
{
    return node == header ? BinForward(index) : node->fd;
}

MallocChunk *BackwardLink(MallocChunk *node, MallocChunk *header, std::size_t index)
{
    return node == header ? BinBackward(index) : node->bk;
}

bool IsLinkTarget(MallocChunk *node, MallocChunk *header, std::uintptr_t heapBase,
                  std::uintptr_t topAddress)
{
    if (node == header)
        return true;
    return IsBinChunkAddress(reinterpret_cast<std::uintptr_t>(node), heapBase, topAddress);
}

std::uint32_t ScanHeap(const struct mallinfo &allocatorState,
                       NewlibHeapGeometrySnapshot *snapshot)
{
    std::uint32_t errors = th06::psp::kNewlibHeapScanErrorNone;

    if (reinterpret_cast<std::uintptr_t>(__malloc_av_) == 0U ||
        reinterpret_cast<std::uintptr_t>(&__malloc_sbrk_base) == 0U ||
        reinterpret_cast<std::uintptr_t>(&__malloc_current_mallinfo) == 0U ||
        reinterpret_cast<std::uintptr_t>(&__malloc_top_pad) == 0U)
        return th06::psp::kNewlibHeapScanErrorMissingSymbol;

    // In newlib 4.5.0 classic dlmalloc, sbrked_mem is a macro alias for
    // current_mallinfo.arena and is updated synchronously by heap grow/trim.
    // The other mallinfo totals are only refreshed by mallinfo(), so derive
    // them from this one bounded pass instead of invoking its unsafe pre-walk.
    const std::size_t arenaBytes = allocatorState.arena;
    const std::uintptr_t heapBase = reinterpret_cast<std::uintptr_t>(__malloc_sbrk_base);
    std::uintptr_t heapEnd = 0;
    if (heapBase < kPspCachedRamBegin || heapBase >= kPspCachedRamEnd ||
        arenaBytes == 0U ||
        arenaBytes > kMaximumPspUserRamBytes ||
        !AddAddress(heapBase, arenaBytes, &heapEnd) || heapEnd <= heapBase ||
        heapEnd > kPspCachedRamEnd ||
        allocatorState.hblks != 0U || allocatorState.hblkhd != 0U)
        return th06::psp::kNewlibHeapScanErrorHeapRange;

    MallocChunk *const top = BinForward(0U);
    const std::uintptr_t topAddress = reinterpret_cast<std::uintptr_t>(top);
    if (topAddress < heapBase || topAddress >= heapEnd || !IsAlignedChunkAddress(topAddress) ||
        heapEnd - topAddress < sizeof(MallocChunk))
        return th06::psp::kNewlibHeapScanErrorTopChunk;

    const std::size_t topRawSize = top->size;
    const std::size_t topSize = topRawSize & ~kChunkSizeBits;
    std::uintptr_t topEnd = 0;
    if ((topRawSize & kIsMmapped) != 0U || (topRawSize & kPrevInUse) == 0U ||
        topSize < kMinChunkBytes || !AddAddress(topAddress, topSize, &topEnd) ||
        topEnd != heapEnd)
        return th06::psp::kNewlibHeapScanErrorTopChunk;

    std::uint64_t scannedFreeBytes = topSize;
    std::uint32_t scannedFreeChunks = 1U;
    std::size_t largestChunk = topSize;
    std::size_t largestRequest = LargestRequestForTopChunk(topSize);
    const std::size_t maximumNodesFromArena = arenaBytes / kMinChunkBytes;
    std::uint32_t traversalBudget = maximumNodesFromArena < kMaximumScannableBinNodes
                                        ? static_cast<std::uint32_t>(maximumNodesFromArena)
                                        : kMaximumScannableBinNodes;
    const std::uint32_t binBlocks = static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(__malloc_av_[1U]));

    for (std::size_t index = 1U; index < kNav && errors == 0U; ++index)
    {
        MallocChunk *const header = BinHeader(index);
        MallocChunk *const forward = BinForward(index);
        MallocChunk *const backward = BinBackward(index);
        if (!IsLinkTarget(forward, header, heapBase, topAddress) ||
            !IsLinkTarget(backward, header, heapBase, topAddress) ||
            BackwardLink(forward, header, index) != header ||
            ForwardLink(backward, header, index) != header)
        {
            errors |= th06::psp::kNewlibHeapScanErrorBinLink;
            break;
        }

        if (index >= 2U && forward != header)
        {
            const std::uint32_t block = 1U << (index / kBinBlockWidth);
            if ((binBlocks & block) == 0U)
            {
                errors |= th06::psp::kNewlibHeapScanErrorBinBlock;
                break;
            }
        }

        std::uint32_t nodesInBin = 0U;
        std::size_t previousChunkSize = UINT32_MAX;
        for (MallocChunk *chunk = forward; chunk != header; chunk = chunk->fd)
        {
            if (traversalBudget == 0U)
            {
                errors |= th06::psp::kNewlibHeapScanErrorTraversalLimit;
                break;
            }
            --traversalBudget;
            ++nodesInBin;
            if (index == 1U && nodesInBin > 1U)
            {
                errors |= th06::psp::kNewlibHeapScanErrorLastRemainder;
                break;
            }

            const std::uintptr_t chunkAddress = reinterpret_cast<std::uintptr_t>(chunk);
            if (!IsBinChunkAddress(chunkAddress, heapBase, topAddress))
            {
                errors |= th06::psp::kNewlibHeapScanErrorChunkRange;
                break;
            }

            const std::size_t rawSize = chunk->size;
            const std::size_t chunkSize = rawSize & ~kChunkSizeBits;
            std::uintptr_t nextAddress = 0;
            if ((rawSize & kIsMmapped) != 0U || (rawSize & kPrevInUse) == 0U ||
                chunkSize < kMinChunkBytes || (chunkSize & kMallocAlignmentMask) != 0U ||
                !AddAddress(chunkAddress, chunkSize, &nextAddress) || nextAddress > topAddress)
            {
                errors |= th06::psp::kNewlibHeapScanErrorChunkMetadata;
                break;
            }

            if (index >= 2U && BinIndexForChunk(chunkSize) != index)
            {
                errors |= th06::psp::kNewlibHeapScanErrorBinPlacement;
                break;
            }
            if (chunkSize >= kMaximumSmallBinSize && chunkSize > previousChunkSize)
            {
                errors |= th06::psp::kNewlibHeapScanErrorBinOrder;
                break;
            }
            previousChunkSize = chunkSize;

            MallocChunk *const next = reinterpret_cast<MallocChunk *>(nextAddress);
            if (next->prevSize != chunkSize || (next->size & kPrevInUse) != 0U)
            {
                errors |= th06::psp::kNewlibHeapScanErrorChunkMetadata;
                break;
            }

            MallocChunk *const nextLink = chunk->fd;
            MallocChunk *const previousLink = chunk->bk;
            if (!IsLinkTarget(nextLink, header, heapBase, topAddress) ||
                !IsLinkTarget(previousLink, header, heapBase, topAddress) ||
                BackwardLink(nextLink, header, index) != chunk ||
                ForwardLink(previousLink, header, index) != chunk)
            {
                errors |= th06::psp::kNewlibHeapScanErrorBinLink;
                break;
            }

            ++scannedFreeChunks;
            scannedFreeBytes += chunkSize;
            if (scannedFreeBytes > arenaBytes)
            {
                errors |= th06::psp::kNewlibHeapScanErrorFreeBytes;
                break;
            }
            if (chunkSize > largestChunk)
                largestChunk = chunkSize;
            const std::size_t request = LargestRequestForBinChunk(chunkSize);
            if (request > largestRequest)
                largestRequest = request;
        }
    }

    if (errors == 0U)
    {
        snapshot->arenaBytes = static_cast<std::uint32_t>(arenaBytes);
        snapshot->freeBytes = static_cast<std::uint32_t>(scannedFreeBytes);
        snapshot->usedBytes = static_cast<std::uint32_t>(arenaBytes - scannedFreeBytes);
        snapshot->topChunkBytes = static_cast<std::uint32_t>(topSize);
        snapshot->freeChunkCount = scannedFreeChunks;
        snapshot->largestFreeChunkBytes = static_cast<std::uint32_t>(largestChunk);
        snapshot->largestNoGrowRequestBytes = static_cast<std::uint32_t>(largestRequest);
        snapshot->heapBaseAddress = static_cast<std::uint32_t>(heapBase);
        snapshot->heapEndAddress = static_cast<std::uint32_t>(heapEnd);
        snapshot->topPadBytes = static_cast<std::uint32_t>(__malloc_top_pad);
    }
    return errors;
}

#if defined(TH06_PSP_RECORD_LAYOUT)
constexpr std::uintptr_t FirstChunkForAlignedBase(std::uintptr_t base)
{
    return ((base + 2U * kSizeBytes + kMallocAlignmentMask) & ~kMallocAlignmentMask) - 2U * kSizeBytes;
}
static_assert(FirstChunkForAlignedBase(0x08000000U) == 0x08000008U,
              "page-aligned sbrk base has eight bytes of front padding");
static_assert(FirstChunkForAlignedBase(0x08000008U) == 0x08000008U,
              "already aligned chunk2mem must not gain padding");
static_assert(sizeof(th06::psp::NewlibHeapLayoutRecord) == 16U,
              "layout stack budget assumes sixteen-byte records");

std::uint32_t WalkHeapLayout(th06::psp::NewlibHeapLayoutRecord *records,
                            std::uint32_t recordCapacity,
                            th06::psp::NewlibHeapLayoutSnapshot *snapshot)
{
    using namespace th06::psp;
    const std::uintptr_t heapBase = snapshot->geometry.heapBaseAddress;
    const std::uintptr_t heapEnd = snapshot->geometry.heapEndAddress;
    const std::uintptr_t topAddress = heapEnd - snapshot->geometry.topChunkBytes;
    snapshot->topChunkAddress = static_cast<std::uint32_t>(topAddress);

    // Audited _malloc_r +0x380: front_misalign=(sbrk_result+8)&15;
    // correction=(16-front_misalign)&15. Chunk+8 is the aligned user pointer,
    // not the chunk header itself. Do not guess that the first chunk is base.
    std::uintptr_t alignmentEnd = 0;
    if (!AddAddress(heapBase, 2U * kSizeBytes + kMallocAlignmentMask, &alignmentEnd))
        return kNewlibHeapLayoutFirstChunk;
    std::uintptr_t address = (alignmentEnd & ~kMallocAlignmentMask) - 2U * kSizeBytes;
    if (address < heapBase || address > topAddress || !IsAlignedChunkAddress(address) ||
        address - heapBase >= kMallocAlignment)
        return kNewlibHeapLayoutFirstChunk;
    snapshot->firstChunkAddress = static_cast<std::uint32_t>(address);
    snapshot->frontPaddingBytes = static_cast<std::uint32_t>(address - heapBase);

    // The public diagnostic caller uses its stack. Reject a mistaken arena
    // buffer before writing any row, rather than modifying heap metadata.
    const std::uintptr_t recordsAddress = reinterpret_cast<std::uintptr_t>(records);
    std::uintptr_t recordsEnd = 0;
    if (!AddAddress(recordsAddress, recordCapacity * sizeof(*records), &recordsEnd) ||
        (recordsAddress < heapEnd && heapBase < recordsEnd))
        return kNewlibHeapLayoutInvalidArguments;

    const std::uintptr_t nearTop = topAddress - heapBase > kNewlibHeapLayoutNearTopBytes
                                       ? topAddress - kNewlibHeapLayoutNearTopBytes : heapBase;
    std::uint64_t freeBytes = 0;
    std::uint32_t freeChunks = 0;
    bool previousInUse = true; // First chunk cannot coalesce before the arena.
    std::size_t previousBytes = 0;
    for (std::uint32_t step = 0; step < kMaximumScannableBinNodes; ++step)
    {
        if (address > topAddress || !IsAlignedChunkAddress(address) ||
            heapEnd - address < sizeof(MallocChunk))
            return kNewlibHeapLayoutChunkMetadata;
        const MallocChunk *const chunk = reinterpret_cast<const MallocChunk *>(address);
        const std::size_t rawSize = chunk->size;
        const std::size_t bytes = rawSize & ~kChunkSizeBits;
        if ((rawSize & kIsMmapped) != 0U || bytes < kMinChunkBytes ||
            ((rawSize & kPrevInUse) != 0U) != previousInUse ||
            (!previousInUse && chunk->prevSize != previousBytes))
            return kNewlibHeapLayoutChunkMetadata;

        const bool isTop = address == topAddress;
        bool inUse = false;
        std::uintptr_t nextAddress = 0;
        if (!AddAddress(address, bytes, &nextAddress))
            return kNewlibHeapLayoutChunkMetadata;
        if (isTop)
        {
            // The page-aligned break minus an eight-mod-sixteen header can
            // leave an eight-mod-sixteen top size. The bin scan validated it.
            if (bytes != snapshot->geometry.topChunkBytes || nextAddress != heapEnd)
                return kNewlibHeapLayoutChunkMetadata;
        }
        else
        {
            if ((bytes & kMallocAlignmentMask) != 0U || nextAddress > topAddress ||
                !IsAlignedChunkAddress(nextAddress))
                return kNewlibHeapLayoutChunkMetadata;
            const MallocChunk *const next = reinterpret_cast<const MallocChunk *>(nextAddress);
            inUse = (next->size & kPrevInUse) != 0U;
            if (!inUse && next->prevSize != bytes)
                return kNewlibHeapLayoutChunkMetadata;
        }

        ++snapshot->walkedChunkCount;
        if (!inUse)
        {
            freeBytes += bytes;
            ++freeChunks;
            if (freeBytes > snapshot->geometry.arenaBytes)
                return kNewlibHeapLayoutAccounting;
        }
        if (isTop || !inUse || bytes >= kNewlibHeapLayoutMinimumChunkBytes || nextAddress > nearTop)
        {
            if (snapshot->recordCount == recordCapacity)
                return kNewlibHeapLayoutRecordOverflow;
            records[snapshot->recordCount++] = {
                static_cast<std::uint32_t>(address),
                static_cast<std::uint32_t>(address + 2U * kSizeBytes),
                static_cast<std::uint32_t>(bytes),
                (inUse ? static_cast<std::uint32_t>(kNewlibHeapLayoutInUse) : 0U) |
                (isTop ? static_cast<std::uint32_t>(kNewlibHeapLayoutTop) : 0U)};
        }
        if (isTop)
            return freeBytes == snapshot->geometry.freeBytes && freeChunks == snapshot->geometry.freeChunkCount
                       ? 0U : static_cast<std::uint32_t>(kNewlibHeapLayoutAccounting);
        previousInUse = inUse;
        previousBytes = bytes;
        address = nextAddress;
    }
    return kNewlibHeapLayoutTraversalLimit;
}
#endif
#endif
} // namespace

namespace th06::psp
{
NewlibHeapGeometrySnapshot CaptureNewlibHeapGeometry()
{
    NewlibHeapGeometrySnapshot snapshot{};
    snapshot.arenaBytes = kNewlibHeapMetricUnknown;
    snapshot.usedBytes = kNewlibHeapMetricUnknown;
    snapshot.freeBytes = kNewlibHeapMetricUnknown;
    snapshot.topChunkBytes = kNewlibHeapMetricUnknown;
    snapshot.freeChunkCount = kNewlibHeapMetricUnknown;
    snapshot.largestFreeChunkBytes = kNewlibHeapMetricUnknown;
    snapshot.largestNoGrowRequestBytes = kNewlibHeapMetricUnknown;
    snapshot.heapBaseAddress = kNewlibHeapMetricUnknown;
    snapshot.heapEndAddress = kNewlibHeapMetricUnknown;
    snapshot.topPadBytes = kNewlibHeapMetricUnknown;

    struct _reent *const reent = _REENT;
    __malloc_lock(reent);
#if TH06_PSP_NEWLIB_HEAP_SCAN_SUPPORTED
    const std::uint32_t errors =
        reinterpret_cast<std::uintptr_t>(&__malloc_current_mallinfo) == 0U
            ? kNewlibHeapScanErrorMissingSymbol
            : ScanHeap(__malloc_current_mallinfo, &snapshot);
#else
    const std::uint32_t errors = kNewlibHeapScanErrorUnsupportedAbi;
#endif
    __malloc_unlock(reent);

    snapshot.scanErrorFlags = errors;
    snapshot.scanValid = errors == 0U ? 1U : 0U;
    snapshot.scanErrorCount = errors == 0U
                                  ? __sync_fetch_and_add(&gHeapScanErrorCount, 0U)
                                  : __sync_add_and_fetch(&gHeapScanErrorCount, 1U);
    return snapshot;
}

#if defined(TH06_PSP_RECORD_LAYOUT)
NewlibHeapLayoutSnapshot CaptureNewlibHeapLayout(NewlibHeapLayoutRecord *records,
                                                std::uint32_t recordCapacity)
{
    NewlibHeapLayoutSnapshot snapshot{};
    snapshot.geometry.arenaBytes = kNewlibHeapMetricUnknown;
    snapshot.geometry.usedBytes = kNewlibHeapMetricUnknown;
    snapshot.geometry.freeBytes = kNewlibHeapMetricUnknown;
    snapshot.geometry.topChunkBytes = kNewlibHeapMetricUnknown;
    snapshot.geometry.freeChunkCount = kNewlibHeapMetricUnknown;
    snapshot.geometry.largestFreeChunkBytes = kNewlibHeapMetricUnknown;
    snapshot.geometry.largestNoGrowRequestBytes = kNewlibHeapMetricUnknown;
    snapshot.geometry.heapBaseAddress = kNewlibHeapMetricUnknown;
    snapshot.geometry.heapEndAddress = kNewlibHeapMetricUnknown;
    snapshot.geometry.topPadBytes = kNewlibHeapMetricUnknown;
    snapshot.firstChunkAddress = kNewlibHeapMetricUnknown;
    snapshot.topChunkAddress = kNewlibHeapMetricUnknown;
    snapshot.frontPaddingBytes = kNewlibHeapMetricUnknown;
    if (!records || recordCapacity == 0U || recordCapacity > kNewlibHeapLayoutMaximumRecords ||
        (reinterpret_cast<std::uintptr_t>(records) & (alignof(NewlibHeapLayoutRecord) - 1U)) != 0U)
    {
        snapshot.layoutErrorFlags = kNewlibHeapLayoutInvalidArguments;
        return snapshot;
    }

    struct _reent *const reent = _REENT;
    __malloc_lock(reent);
#if TH06_PSP_NEWLIB_HEAP_SCAN_SUPPORTED
    const std::uint32_t errors = reinterpret_cast<std::uintptr_t>(&__malloc_current_mallinfo) == 0U
                                    ? kNewlibHeapScanErrorMissingSymbol
                                    : ScanHeap(__malloc_current_mallinfo, &snapshot.geometry);
    const std::uint32_t layoutErrors = errors != 0U ? errors : WalkHeapLayout(records, recordCapacity, &snapshot);
#else
    const std::uint32_t errors = kNewlibHeapScanErrorUnsupportedAbi;
    const std::uint32_t layoutErrors = errors;
#endif
    __malloc_unlock(reent);
    snapshot.geometry.scanValid = errors == 0U ? 1U : 0U;
    snapshot.geometry.scanErrorFlags = errors;
    // This extra diagnostic capture must not increment the normal metric's
    // lifetime error counter or change allocator/bin ordering.
    snapshot.geometry.scanErrorCount = __sync_fetch_and_add(&gHeapScanErrorCount, 0U);
    snapshot.layoutValid = layoutErrors == 0U ? 1U : 0U;
    snapshot.layoutErrorFlags = layoutErrors;
    return snapshot;
}
#endif
} // namespace th06::psp
