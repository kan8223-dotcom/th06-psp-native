// Host-only oracle: link the ACTUAL legacy Pbg3Archive decoder, not a
// rewritten reference. Proprietary archive/SMF inputs and test outputs stay
// outside the repository; this program never writes extracted song files.
#include "Pbg3SongDecode.hpp"
#include "pbg3/Pbg3Archive.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

// The legacy archive's only platform dependency used by this host test.
namespace FileSystem
{
FILE *FopenUTF8(const char *path, const char *mode) { return std::fopen(path, mode); }
}

namespace
{
using th06midi::Pbg3SongDecodeResult;
using Status = th06midi::Pbg3SongDecodeStatus;
constexpr std::size_t kGuard = 64;
constexpr unsigned char kSentinel = 0xa7;
unsigned gChecks;
unsigned gFailures;

bool Check(bool condition, const std::string &label)
{
    ++gChecks;
    if (!condition)
    {
        ++gFailures;
        std::fprintf(stderr, "FAIL %s\n", label.c_str());
    }
    return condition;
}

bool Load(const std::string &path, std::vector<unsigned char> &bytes)
{
    FILE *file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    const bool positioned = std::fseek(file, 0, SEEK_END) == 0;
    const long size = positioned ? std::ftell(file) : -1;
    if (size < 0 || std::fseek(file, 0, SEEK_SET) != 0)
    {
        std::fclose(file);
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    const bool complete = std::fread(bytes.data(), 1, bytes.size(), file) == bytes.size();
    std::fclose(file);
    return complete;
}

bool Guards(const std::vector<unsigned char> &buffer, std::size_t capacity)
{
    return std::all_of(buffer.begin(), buffer.begin() + kGuard,
                       [](unsigned char b) { return b == kSentinel; }) &&
           std::all_of(buffer.begin() + kGuard + capacity, buffer.end(),
                       [](unsigned char b) { return b == kSentinel; });
}

std::uint32_t Sum(const std::vector<unsigned char> &bytes)
{
    std::uint32_t result = 0;
    for (unsigned char byte : bytes) result += byte;
    return result;
}

std::uint64_t Hash(const unsigned char *bytes, std::size_t size)
{
    std::uint64_t result = 14695981039346656037ull;
    for (std::size_t i = 0; i < size; ++i)
        result = (result ^ bytes[i]) * 1099511628211ull;
    return result;
}

bool DecodeExpected(const std::string &name, const std::vector<unsigned char> &encoded,
                    const std::vector<unsigned char> &expected, std::uint32_t checksum,
                    std::size_t expectedConsumed)
{
    const std::size_t capacity = expected.size() + 19;
    std::vector<unsigned char> guarded(kGuard + capacity + kGuard, kSentinel);
    Pbg3SongDecodeResult result{};
    const bool decoded = th06midi::DecodePbg3Song(encoded.data(), encoded.size(), guarded.data() + kGuard,
                                               capacity, expected.size(), checksum, &result);
    const bool okay = decoded && result.status == Status::kOk &&
                      result.writtenBytes == expected.size() && result.consumedBytes == expectedConsumed &&
                      result.checksum == checksum &&
                      std::memcmp(guarded.data() + kGuard, expected.data(), expected.size()) == 0 &&
                      std::all_of(guarded.begin() + kGuard + expected.size(), guarded.end(),
                                  [](unsigned char b) { return b == kSentinel; }) && Guards(guarded, capacity);
    Check(okay, "synthetic " + name);
    std::printf("SYNTH\t%s\t%s\t%zu\t%zu\t%u\n", name.c_str(), okay ? "PASS" : "FAIL",
                encoded.size(), result.writtenBytes, result.checksum);
    return okay;
}

// Encoder used only to describe hand-authored format boundary examples.
// It does not share the production decoder's bit reader or dictionary.
struct Encoded
{
    std::vector<unsigned char> data;
    unsigned bits = 0;
    void Put(std::uint32_t value, unsigned count)
    {
        for (unsigned remaining = count; remaining; --remaining)
        {
            if ((bits & 7u) == 0) data.push_back(0);
            data.back() |= ((value >> (remaining - 1)) & 1u) << (7 - (bits & 7u));
            ++bits;
        }
    }
    void Literal(unsigned char byte) { Put(1, 1); Put(byte, 8); }
    void Match(unsigned offset, unsigned length) { Put(0, 1); Put(offset, 13); Put(length - 3, 4); }
    void End() { Put(0, 1); Put(0, 13); }
};

void Reject(const std::string &label, const std::vector<unsigned char> &encoded,
            std::size_t inputSize, std::size_t capacity, std::size_t decodedSize,
            std::uint32_t checksum, Status status, bool nullInput = false, bool nullOutput = false)
{
    std::vector<unsigned char> guarded(kGuard + capacity + kGuard, kSentinel);
    Pbg3SongDecodeResult result{};
    const bool decoded = th06midi::DecodePbg3Song(nullInput ? nullptr : encoded.data(), inputSize,
                                               nullOutput ? nullptr : guarded.data() + kGuard,
                                               capacity, decodedSize, checksum, &result);
    const bool untouchedRequired = status == Status::kInvalidArguments;
    const bool untouched = std::all_of(guarded.begin(), guarded.end(),
                                      [](unsigned char b) { return b == kSentinel; });
    const bool okay = !decoded && result.status == status && result.consumedBytes <= inputSize &&
                      result.writtenBytes <= capacity && Guards(guarded, capacity) &&
                      (!untouchedRequired || untouched);
    Check(okay, "reject " + label);
    std::printf("REJECT\t%s\t%s\t%d\t%zu\t%zu\n", label.c_str(), okay ? "PASS" : "FAIL",
                static_cast<int>(result.status), result.consumedBytes, result.writtenBytes);
}

void Synthetic()
{
    Encoded literal;
    literal.Literal('A'); literal.End();
    DecodeExpected("literal", literal.data, {'A'}, Sum(literal.data), literal.data.size());
    auto trailing = literal.data;
    trailing.insert(trailing.end(), {0xfa, 0xed, 0x71, 0x39});
    DecodeExpected("terminal-does-not-consume-trailing-bytes", trailing, {'A'}, Sum(literal.data), literal.data.size());
    // 23 bits above leave one padding bit. Its byte is checksummed even
    // though the bit does not belong to an opcode or output.
    auto padding = literal.data;
    padding.back() |= 1;
    DecodeExpected("terminal-padding-in-checksum", padding, {'A'}, Sum(padding), padding.size());

    Encoded overlap;
    overlap.Literal('A'); overlap.Match(1, 18); overlap.End();
    DecodeExpected("overlapping-max-length-match", overlap.data, std::vector<unsigned char>(19, 'A'),
                   Sum(overlap.data), overlap.data.size());
    Encoded zeroDictionary;
    zeroDictionary.Match(4096, 3); zeroDictionary.End();
    DecodeExpected("zero-initial-dictionary-min-match", zeroDictionary.data, std::vector<unsigned char>(3, 0),
                   Sum(zeroDictionary.data), zeroDictionary.data.size());
    Encoded wrap;
    for (unsigned i = 0; i < 8194; ++i) wrap.Literal('A');
    wrap.Match(8190, 18); wrap.End();
    DecodeExpected("dictionary-wrap", wrap.data, std::vector<unsigned char>(8212, 'A'),
                   Sum(wrap.data), wrap.data.size());

    Reject("null-input", literal.data, literal.data.size(), 1, 1, Sum(literal.data), Status::kInvalidArguments, true);
    Reject("null-output", literal.data, literal.data.size(), 1, 1, Sum(literal.data), Status::kInvalidArguments, false, true);
    Reject("empty-input", literal.data, 0, 1, 1, 0, Status::kInvalidArguments);
    Reject("zero-decoded-size", literal.data, literal.data.size(), 1, 0, Sum(literal.data), Status::kInvalidArguments);
    Reject("insufficient-capacity", literal.data, literal.data.size(), 0, 1, Sum(literal.data), Status::kInvalidArguments);
    for (std::size_t cut = 1; cut < literal.data.size(); ++cut)
        Reject("truncated-literal-" + std::to_string(cut), literal.data, cut, 1, 1, Sum(literal.data), Status::kTruncatedInput);
    Reject("short-output-at-terminator", literal.data, literal.data.size(), 2, 2,
           Sum(literal.data), Status::kOutputSizeMismatch);
    Reject("checksum", literal.data, literal.data.size(), 1, 1, Sum(literal.data) ^ 1u, Status::kChecksumMismatch);
    Reject("match-output-overflow", overlap.data, overlap.data.size(), 18, 18,
           Sum(overlap.data), Status::kOutputOverflow);
    Encoded extraLiteral;
    extraLiteral.Literal('A'); extraLiteral.Literal('B'); extraLiteral.End();
    Reject("literal-output-overflow", extraLiteral.data, extraLiteral.data.size(), 1, 1,
           Sum(extraLiteral.data), Status::kOutputOverflow);

    // Overlap/range checks must reject before touching either caller buffer.
    auto shared = literal.data;
    Pbg3SongDecodeResult result{};
    const bool overlapRejected = !th06midi::DecodePbg3Song(shared.data(), shared.size(), shared.data() + 1,
                                                          1, 1, Sum(shared), &result);
    Check(overlapRejected && result.status == Status::kInvalidArguments && shared == literal.data,
          "reject overlapping-source-destination");
    std::printf("REJECT\toverlapping-source-destination\t%s\n", overlapRejected ? "PASS" : "FAIL");
    const auto *impossible = reinterpret_cast<const unsigned char *>(std::numeric_limits<std::uintptr_t>::max() - 1u);
    unsigned char destination = kSentinel;
    const bool rangeRejected = !th06midi::DecodePbg3Song(impossible, 4, &destination, 1, 1, 0, &result);
    Check(rangeRejected && result.status == Status::kInvalidArguments && destination == kSentinel,
          "reject input-address-overflow");
    std::printf("REJECT\tinput-address-overflow\t%s\n", rangeRejected ? "PASS" : "FAIL");
}

unsigned RealSongs(const char *archivePath, const char *extractedDirectory)
{
    // Use the legacy parser itself for archive headers/table; do not assume
    // guessed compressed boundaries or manufacture compressed song files.
    Pbg3Parser table;
    if (!Check(table.OpenArchive(archivePath) && table.ReadMagic() == 0x33474250, "archive magic/open")) return 0;
    const std::uint32_t count = table.ReadVarInt();
    const std::uint32_t tableOffset = table.ReadVarInt();
    if (!Check(count > 0 && count < 1024 && table.SeekToOffset(tableOffset), "archive table bounds")) return 0;
    std::vector<std::string> names;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        for (unsigned field = 0; field < 5; ++field) table.ReadVarInt();
        char name[256] = {};
        if (!Check(table.ReadString(name, sizeof(name)), "archive filename")) return 0;
        const std::string file(name);
        if (file.size() >= 4 && file.substr(file.size() - 4) == ".mid") names.push_back(file);
    }
    Pbg3Archive archive;
    if (!Check(archive.Load(archivePath), "legacy archive load")) return 0;
    unsigned songs = 0;
    for (const auto &name : names)
    {
        const int index = archive.FindEntry(name.c_str());
        if (!Check(index >= 0, "find " + name)) continue;
        const std::size_t decodedBytes = archive.GetEntrySize(static_cast<unsigned>(index));
        std::uint32_t rawBytes = 0, checksum = 0;
        unsigned char *raw = archive.ReadEntryRaw(&rawBytes, &checksum, index);
        if (!Check(raw && rawBytes && decodedBytes, "legacy raw " + name)) { std::free(raw); continue; }
        const std::vector<unsigned char> encoded(raw, raw + rawBytes);
        std::free(raw);
        unsigned char *legacy = archive.ReadDecompressEntry(static_cast<unsigned>(index), name.c_str());
        if (!Check(legacy != nullptr, "legacy decode " + name)) continue;
        std::vector<unsigned char> extracted;
        const bool readExtracted = Load(std::string(extractedDirectory) + "/" + name, extracted);
        const bool referenceOkay = readExtracted && extracted.size() == decodedBytes &&
                                   std::memcmp(legacy, extracted.data(), decodedBytes) == 0;
        Check(referenceOkay, "legacy vs preexisting extraction " + name);
        std::vector<unsigned char> guarded(kGuard + decodedBytes + kGuard, kSentinel);
        Pbg3SongDecodeResult result{};
        const bool decoded = th06midi::DecodePbg3Song(encoded.data(), encoded.size(), guarded.data() + kGuard,
                                                   decodedBytes, decodedBytes, checksum, &result);
        const bool okay = decoded && referenceOkay && result.status == Status::kOk &&
                          result.writtenBytes == decodedBytes && result.consumedBytes <= encoded.size() &&
                          result.checksum == checksum && Guards(guarded, decodedBytes) &&
                          std::memcmp(guarded.data() + kGuard, legacy, decodedBytes) == 0 &&
                          decodedBytes >= 4 && std::memcmp(legacy, "MThd", 4) == 0;
        Check(okay, "direct vs actual legacy " + name);
        std::printf("REAL\t%s\t%s\t%zu\t%zu\t%zu\t%u\t%016llx\n", name.c_str(), okay ? "PASS" : "FAIL",
                    encoded.size(), decodedBytes, result.consumedBytes, result.checksum,
                    static_cast<unsigned long long>(Hash(legacy, decodedBytes)));
        std::free(legacy);
        if (okay)
        {
            ++songs;
            // A successful observed consumed boundary lets us exercise
            // truncation of the final required byte without guessing that
            // trailing archive bytes belong to the compressed stream.
            Reject(name + ":last-required-byte-missing", encoded, result.consumedBytes - 1,
                   decodedBytes, decodedBytes, checksum, Status::kTruncatedInput);
            Reject(name + ":capacity-minus-one", encoded, encoded.size(), decodedBytes - 1,
                   decodedBytes, checksum, Status::kInvalidArguments);
            Reject(name + ":decoded-minus-one", encoded, encoded.size(), decodedBytes,
                   decodedBytes - 1, checksum, Status::kOutputOverflow);
            Reject(name + ":decoded-plus-one", encoded, encoded.size(), decodedBytes + 1,
                   decodedBytes + 1, checksum, Status::kOutputSizeMismatch);
            Reject(name + ":checksum", encoded, encoded.size(), decodedBytes,
                   decodedBytes, checksum ^ 1u, Status::kChecksumMismatch);
        }
    }
    Check(names.size() == 17 && songs == 17, "all 17 actual SMF entries compared");
    return songs;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        std::fprintf(stderr, "usage: %s private/MD.DAT private/preexisting-extracted-midi-directory\n", argv[0]);
        return 2;
    }
    Synthetic();
    const unsigned songs = RealSongs(argv[1], argv[2]);
    std::printf("PBG3_SONG_ORACLE %s SONGS%u CHECKS%u FAILURES%u\n", gFailures ? "FAIL" : "PASS",
                songs, gChecks, gFailures);
    return gFailures ? 1 : 0;
}
