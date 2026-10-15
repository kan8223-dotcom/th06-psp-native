#pragma once
// Bounded version of the engine's Pbg3Archive::ReadDecompressEntry LZSS.
// Platform-independent, allocation-free; the caller owns an unpublished
// destination and discards it on failure. Never pass a live player payload.
#include <cstddef>
#include <cstdint>

namespace th06midi
{

enum class Pbg3SongDecodeStatus
{
    kOk = 0,
    kInvalidArguments,
    kTruncatedInput,
    kOutputOverflow,
    kOutputSizeMismatch,
    kChecksumMismatch,
};

struct Pbg3SongDecodeResult
{
    Pbg3SongDecodeStatus status;
    std::size_t consumedBytes;
    std::size_t writtenBytes;
    std::uint32_t checksum;
};

namespace pbg3song_detail
{
struct Bits
{
    const std::uint8_t *source;
    std::size_t size;
    std::size_t cursor = 0;
    std::uint32_t checksum = 0;
    unsigned mask = 0x80;
    unsigned current = 0;

    bool read(unsigned count, std::uint32_t &value)
    {
        value = 0;
        for (unsigned i = 0; i < count; ++i)
        {
            if (mask == 0x80)
            {
                if (cursor >= size) return false;
                current = source[cursor++];
                // Like DEC_HANDLE_FETCH_NEW_BYTE, count each newly fetched
                // compressed byte once, including the terminal byte padding.
                checksum += current;
            }
            value = (value << 1) | ((current & mask) != 0 ? 1u : 0u);
            mask >>= 1;
            if (mask == 0) mask = 0x80;
        }
        return true;
    }
};
} // namespace pbg3song_detail

inline bool DecodePbg3Song(const std::uint8_t *encoded, std::size_t encodedBytes,
                          std::uint8_t *destination, std::size_t destinationCapacity,
                          std::size_t decodedBytes, std::uint32_t expectedChecksum,
                          Pbg3SongDecodeResult *result = nullptr)
{
    if (result) *result = {Pbg3SongDecodeStatus::kInvalidArguments, 0, 0, 0};
    if (!encoded || !destination || encodedBytes == 0 || decodedBytes == 0 ||
        decodedBytes > destinationCapacity)
        return false;
    const std::uintptr_t inputAddress = reinterpret_cast<std::uintptr_t>(encoded);
    const std::uintptr_t outputAddress = reinterpret_cast<std::uintptr_t>(destination);
    const std::uintptr_t maximumAddress = ~std::uintptr_t(0);
    if (encodedBytes > maximumAddress - inputAddress || decodedBytes > maximumAddress - outputAddress ||
        (inputAddress < outputAddress + decodedBytes && outputAddress < inputAddress + encodedBytes))
        return false;

    pbg3song_detail::Bits bits{encoded, encodedBytes};
    std::size_t written = 0;
    const auto finish = [&](Pbg3SongDecodeStatus status) {
        if (result) *result = {status, bits.cursor, written, bits.checksum};
        return status == Pbg3SongDecodeStatus::kOk;
    };
    std::uint8_t dictionary[0x2000] = {};
    unsigned head = 1;
    for (;;)
    {
        std::uint32_t flag = 0;
        if (!bits.read(1, flag)) return finish(Pbg3SongDecodeStatus::kTruncatedInput);
        if (flag != 0)
        {
            std::uint32_t literal = 0;
            if (!bits.read(8, literal)) return finish(Pbg3SongDecodeStatus::kTruncatedInput);
            if (written == decodedBytes) return finish(Pbg3SongDecodeStatus::kOutputOverflow);
            const std::uint8_t value = static_cast<std::uint8_t>(literal);
            destination[written++] = value;
            dictionary[head] = value;
            head = (head + 1u) & 0x1fffu;
        }
        else
        {
            std::uint32_t offset = 0;
            if (!bits.read(13, offset)) return finish(Pbg3SongDecodeStatus::kTruncatedInput);
            if (offset == 0) break;
            std::uint32_t lengthBits = 0;
            if (!bits.read(4, lengthBits)) return finish(Pbg3SongDecodeStatus::kTruncatedInput);
            const unsigned length = static_cast<unsigned>(lengthBits) + 3u;
            if (length > decodedBytes - written) return finish(Pbg3SongDecodeStatus::kOutputOverflow);
            // Read then write for each byte: dictionary overlap/wrap must
            // reproduce the original DEC_WRITE_BYTE, not a bulk memcpy.
            for (unsigned i = 0; i < length; ++i)
            {
                const std::uint8_t value = dictionary[(offset + i) & 0x1fffu];
                destination[written++] = value;
                dictionary[head] = value;
                head = (head + 1u) & 0x1fffu;
            }
        }
    }
    // Original alignment discards only bits in the already-fetched last
    // byte. It does not consume/checksum any bytes after the terminator.
    if (written != decodedBytes) return finish(Pbg3SongDecodeStatus::kOutputSizeMismatch);
    if (bits.checksum != expectedChecksum) return finish(Pbg3SongDecodeStatus::kChecksumMismatch);
    return finish(Pbg3SongDecodeStatus::kOk);
}

} // namespace th06midi
