// DEFLATE (RFC 1951) and zlib (RFC 1950), written from the two RFCs and nothing else.
//
// Reading (inflate) is the simple half: a bit reader, three block types, and canonical Huffman
// decoding. Writing (deflate) finds repeats with hash chains (LZ77), counts the symbols of each
// block, and writes the block in whichever of the three types is the smallest.
#include "Deflate.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <utility>

namespace editor {

namespace {

// ---- Constants from RFC 1951 -------------------------------------------------------------------

constexpr int maximumCodeLength = 15;                    // 3.2.7: no Huffman code is longer than 15 bits.
constexpr int maximumCodeLengthCodeLength = 7;           // 3.2.7: the code-length alphabet's codes fit 3 bits.
constexpr int endOfBlockSymbol = 256;
constexpr size_t literalLengthSymbolCount = 286;         // 3.2.5: symbols 286 and 287 exist only in the fixed code.
constexpr size_t fixedLiteralLengthSymbolCount = 288;
constexpr size_t distanceSymbolCount = 30;               // 3.2.5: symbols 30 and 31 exist only in the fixed code.
constexpr size_t codeLengthSymbolCount = 19;
constexpr size_t windowSize = 32768;                     // 2.: a distance reaches back at most 32 KiB.
constexpr int minimumMatchLength = 3;
constexpr int maximumMatchLength = 258;
constexpr size_t maximumStoredLength = 65535;            // 3.2.4: LEN is 16 bits.

/// RFC 1951, 3.2.5: the smallest length of each length symbol 257...285 (index 0...28).
constexpr std::array<uint16_t, 29> lengthBase = {3,  4,  5,  6,  7,  8,  9,  10, 11, 13,  15,  17,  19,  23, 27,
                                                 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
/// RFC 1951, 3.2.5: how many extra bits follow each length symbol.
constexpr std::array<uint8_t, 29> lengthExtraBits = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                     2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
/// RFC 1951, 3.2.5: the smallest distance of each distance symbol 0...29.
constexpr std::array<uint16_t, 30> distanceBase = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                                   33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                                   1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
/// RFC 1951, 3.2.5: how many extra bits follow each distance symbol.
constexpr std::array<uint8_t, 30> distanceExtraBits = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
/// RFC 1951, 3.2.7: the order in which a dynamic header lists the code-length code's lengths.
constexpr std::array<uint8_t, 19> codeLengthOrder = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

[[noreturn]] void fail(const char* sentence) { throw DeflateProblem(sentence); }

/// `value`'s lowest `length` bits in reverse order. Huffman codes are defined most significant bit
/// first (RFC 1951, 3.1.1) but travel in the stream least significant bit first, so the writer
/// reverses each code once and then writes it like any other number.
uint16_t reverseBits(uint16_t value, int length) {
    uint16_t reversed = 0;
    for (int bit = 0; bit < length; ++bit) {
        reversed = static_cast<uint16_t>((reversed << 1) | ((value >> bit) & 1));
    }
    return reversed;
}

// ---- Reading -----------------------------------------------------------------------------------

/// Reads numbers of up to 16 bits from `bytes`, least significant bit first (RFC 1951, 3.1.1).
/// Bytes are loaded one at a time and only when a number needs them, so after any read fewer than
/// 8 bits are held, and dropping those aligns the reader to the next whole byte.
class BitReader {
public:
    explicit BitReader(std::span<const uint8_t> bytes) : bytes(bytes) {}

    /// The next `count` bits as a number, the first bit read being the lowest. Throws when the
    /// stream ends first.
    uint32_t bits(int count) {
        while (heldBits < count) {
            if (position >= bytes.size()) fail("the DEFLATE stream is cut short.");
            buffer |= static_cast<uint32_t>(bytes[position]) << heldBits;
            position += 1;
            heldBits += 8;
        }
        const uint32_t value = buffer & ((1u << count) - 1u);
        buffer >>= count;
        heldBits -= count;
        return value;
    }

    /// Drops the unread bits of the current byte, so the next read starts on a byte boundary.
    void alignToByte() {
        buffer = 0;
        heldBits = 0;
    }

    /// The next `count` whole bytes, which must start on a byte boundary (call `alignToByte` first).
    std::span<const uint8_t> wholeBytes(size_t count) {
        if (count > bytes.size() - position) fail("the DEFLATE stream is cut short.");
        const auto taken = bytes.subspan(position, count);
        position += count;
        return taken;
    }

    /// How many bytes of the input have been taken so far.
    size_t bytesTaken() const { return position; }

private:
    std::span<const uint8_t> bytes;
    size_t position = 0;
    uint32_t buffer = 0;
    int heldBits = 0;
};

/// A canonical Huffman code ready for decoding: how many codes there are of each length, and the
/// symbols in code order (RFC 1951, 3.2.2: codes of one length are consecutive numbers, in symbol order).
struct HuffmanDecoder {
    std::array<uint16_t, maximumCodeLength + 1> countOfLength{};
    std::vector<uint16_t> symbolsInCodeOrder;
};

/// How strictly `decoderFor` judges a code that leaves some bit patterns unused.
enum class Completeness {
    /// Every bit pattern must decode (the code-length code).
    complete,
    /// Also accept no code at all, or one code of one bit (RFC 1951, 3.2.7: a block with a single
    /// distance code, or only literals, has such a distance code).
    allowSingleCode,
};

/// The decoder for the code whose lengths are `codeLengths` (0 = symbol unused). Throws for lengths
/// that cannot be a prefix code: more codes of some length than fit (over-subscribed), or, per
/// `completeness`, a code that leaves bit patterns unused.
///
/// How: walking lengths 1...15, `left` counts the code numbers still free at that length; it
/// doubles per extra bit and shrinks by the codes taken, and going negative means too many codes.
HuffmanDecoder decoderFor(std::span<const uint8_t> codeLengths, Completeness completeness) {
    HuffmanDecoder decoder;
    for (const uint8_t length : codeLengths) decoder.countOfLength[length] += 1;
    const size_t codeCount = codeLengths.size() - decoder.countOfLength[0];
    decoder.countOfLength[0] = 0;

    int left = 1;
    for (int length = 1; length <= maximumCodeLength; ++length) {
        left = (left << 1) - decoder.countOfLength[length];
        if (left < 0) fail("the DEFLATE stream has a Huffman code that is over-subscribed.");
    }
    if (left > 0) {
        const bool singleBit = codeCount == 1 && decoder.countOfLength[1] == 1;
        const bool acceptable = completeness == Completeness::allowSingleCode && (codeCount == 0 || singleBit);
        if (!acceptable) fail("the DEFLATE stream has a Huffman code that is incomplete.");
    }

    // Sort the symbols by (length, symbol): the order canonical codes are numbered in.
    std::array<uint16_t, maximumCodeLength + 2> offsetOfLength{};
    for (int length = 1; length <= maximumCodeLength; ++length) {
        offsetOfLength[length + 1] = static_cast<uint16_t>(offsetOfLength[length] + decoder.countOfLength[length]);
    }
    decoder.symbolsInCodeOrder.assign(codeCount, 0);
    for (size_t symbol = 0; symbol < codeLengths.size(); ++symbol) {
        if (codeLengths[symbol] != 0) {
            decoder.symbolsInCodeOrder[offsetOfLength[codeLengths[symbol]]++] = static_cast<uint16_t>(symbol);
        }
    }
    return decoder;
}

/// The next symbol of `decoder` from `reader`. Because canonical codes of one length are
/// consecutive, after reading `length` bits as `code` the code is a symbol of that length exactly
/// when `code − first` is below the count of that length, `first` being the first code of the length.
int decodedSymbol(BitReader& reader, const HuffmanDecoder& decoder) {
    int code = 0;
    int first = 0;
    int index = 0;
    for (int length = 1; length <= maximumCodeLength; ++length) {
        code |= static_cast<int>(reader.bits(1));
        const int count = decoder.countOfLength[length];
        if (code - count < first) return decoder.symbolsInCodeOrder[static_cast<size_t>(index + (code - first))];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    fail("the DEFLATE stream has a bit pattern that no Huffman code gives.");
}

/// A stored block (RFC 1951, 3.2.4): the bytes after the byte boundary, copied as they are.
void inflateStoredBlock(BitReader& reader, std::vector<uint8_t>& output) {
    reader.alignToByte();
    const uint32_t length = reader.bits(16);
    const uint32_t complement = reader.bits(16);
    if ((length ^ 0xFFFFu) != complement) {
        fail("the DEFLATE stream has a stored block whose length and its complement disagree.");
    }
    const auto bytes = reader.wholeBytes(length);
    output.insert(output.end(), bytes.begin(), bytes.end());
}

/// The fixed code of RFC 1951, 3.2.6 for literals and lengths: symbols 0...143 take 8 bits,
/// 144...255 take 9, 256...279 take 7, 280...287 take 8.
std::vector<uint8_t> fixedLiteralLengthLengths() {
    std::vector<uint8_t> lengths(fixedLiteralLengthSymbolCount, 8);
    std::fill(lengths.begin() + 144, lengths.begin() + 256, uint8_t{9});
    std::fill(lengths.begin() + 256, lengths.begin() + 280, uint8_t{7});
    return lengths;
}

/// The fixed code of RFC 1951, 3.2.6 for distances: 5 bits each. (Written with 30 symbols; the
/// stream's 30 and 31 are not distances, and decoding those fails below.)
std::vector<uint8_t> fixedDistanceLengths() { return std::vector<uint8_t>(32, 5); }

/// The literal/length and distance codes a dynamic block (RFC 1951, 3.2.7) announces in its header.
struct DynamicDecoders {
    HuffmanDecoder literalLength;
    HuffmanDecoder distance;
};

/// Reads a dynamic block's header: HLIT, HDIST, HCLEN, the code-length code, and with it the two
/// code-length sequences, which run-length codes 16, 17 and 18 may spell out and may carry across
/// from the literal/length lengths into the distance lengths.
DynamicDecoders readDynamicDecoders(BitReader& reader) {
    const size_t literalLengthCount = reader.bits(5) + 257;
    const size_t distanceCount = reader.bits(5) + 1;
    const size_t codeLengthCount = reader.bits(4) + 4;
    if (literalLengthCount > literalLengthSymbolCount || distanceCount > distanceSymbolCount) {
        fail("the DEFLATE stream announces more Huffman symbols than exist.");
    }

    std::array<uint8_t, codeLengthSymbolCount> codeLengthLengths{};
    for (size_t index = 0; index < codeLengthCount; ++index) {
        codeLengthLengths[codeLengthOrder[index]] = static_cast<uint8_t>(reader.bits(3));
    }
    const HuffmanDecoder codeLengthDecoder = decoderFor(codeLengthLengths, Completeness::complete);

    std::vector<uint8_t> lengths(literalLengthCount + distanceCount, 0);
    size_t filled = 0;
    while (filled < lengths.size()) {
        const int symbol = decodedSymbol(reader, codeLengthDecoder);
        if (symbol < 16) {
            lengths[filled++] = static_cast<uint8_t>(symbol);
            continue;
        }
        uint8_t value = 0;
        size_t repeat = 0;
        if (symbol == 16) {
            if (filled == 0) fail("the DEFLATE stream repeats a code length before giving one.");
            value = lengths[filled - 1];
            repeat = reader.bits(2) + 3;
        } else if (symbol == 17) {
            repeat = reader.bits(3) + 3;
        } else {
            repeat = reader.bits(7) + 11;
        }
        if (filled + repeat > lengths.size()) fail("the DEFLATE stream repeats more code lengths than it has symbols.");
        std::fill_n(lengths.begin() + static_cast<std::ptrdiff_t>(filled), repeat, value);
        filled += repeat;
    }
    if (lengths[endOfBlockSymbol] == 0) fail("the DEFLATE stream's dynamic block has no end-of-block code.");

    const std::span<const uint8_t> all(lengths);
    return {decoderFor(all.first(literalLengthCount), Completeness::allowSingleCode),
            decoderFor(all.subspan(literalLengthCount), Completeness::allowSingleCode)};
}

/// The symbols of a Huffman-coded block (RFC 1951, 3.2.5) until its end-of-block code: a literal
/// byte, or a length and distance that copy earlier output (the copy may overlap itself, which is
/// how a distance of 1 repeats one byte).
void inflateCodedBlock(BitReader& reader, const HuffmanDecoder& literalLength, const HuffmanDecoder& distance,
                       std::vector<uint8_t>& output) {
    for (;;) {
        int symbol = decodedSymbol(reader, literalLength);
        if (symbol < endOfBlockSymbol) {
            output.push_back(static_cast<uint8_t>(symbol));
            continue;
        }
        if (symbol == endOfBlockSymbol) return;

        symbol -= endOfBlockSymbol + 1;
        if (symbol >= static_cast<int>(lengthBase.size())) fail("the DEFLATE stream has a length symbol that does not exist.");
        const size_t length = lengthBase[static_cast<size_t>(symbol)] + reader.bits(lengthExtraBits[static_cast<size_t>(symbol)]);

        const int distanceSymbol = decodedSymbol(reader, distance);
        if (distanceSymbol >= static_cast<int>(distanceBase.size())) {
            fail("the DEFLATE stream has a distance symbol that does not exist.");
        }
        const size_t distanceBack =
            distanceBase[static_cast<size_t>(distanceSymbol)] + reader.bits(distanceExtraBits[static_cast<size_t>(distanceSymbol)]);
        if (distanceBack > output.size()) fail("the DEFLATE stream has a distance that reaches back before its start.");

        const size_t start = output.size() - distanceBack;
        output.resize(output.size() + length);
        uint8_t* bytes = output.data();
        for (size_t offset = 0; offset < length; ++offset) bytes[start + distanceBack + offset] = bytes[start + offset];
    }
}

/// Appends every block of the stream to `output` and returns how many input bytes the stream took
/// (the last, partly used byte included), so a wrapper can find what follows it.
size_t inflateBlocks(std::span<const uint8_t> compressed, std::vector<uint8_t>& output) {
    BitReader reader(compressed);
    bool isFinalBlock = false;
    while (!isFinalBlock) {
        isFinalBlock = reader.bits(1) == 1;
        const uint32_t blockType = reader.bits(2);
        if (blockType == 0) {
            inflateStoredBlock(reader, output);
        } else if (blockType == 1) {
            static const HuffmanDecoder fixedLiteralLength = decoderFor(fixedLiteralLengthLengths(), Completeness::complete);
            static const HuffmanDecoder fixedDistance = decoderFor(fixedDistanceLengths(), Completeness::complete);
            inflateCodedBlock(reader, fixedLiteralLength, fixedDistance, output);
        } else if (blockType == 2) {
            const DynamicDecoders decoders = readDynamicDecoders(reader);
            inflateCodedBlock(reader, decoders.literalLength, decoders.distance, output);
        } else {
            fail("the DEFLATE stream has the reserved block type 3.");
        }
    }
    reader.alignToByte();
    return reader.bytesTaken();
}

// ---- Writing: bits and codes -------------------------------------------------------------------

/// Appends numbers to a byte vector, least significant bit first, the way `BitReader` reads them.
class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& output) : output(output) {}

    /// Writes the lowest `count` bits of `value`, lowest first.
    void bits(uint32_t value, int count) {
        buffer |= static_cast<uint64_t>(value) << heldBits;
        heldBits += count;
        while (heldBits >= 8) {
            output.push_back(static_cast<uint8_t>(buffer & 0xFF));
            buffer >>= 8;
            heldBits -= 8;
        }
    }

    /// Pads the current byte with zero bits, so the next write starts on a byte boundary.
    void alignToByte() {
        if (heldBits > 0) bits(0, 8 - heldBits);
    }

    /// Whole bytes, written as they are; the writer must be on a byte boundary.
    void wholeBytes(std::span<const uint8_t> bytes) { output.insert(output.end(), bytes.begin(), bytes.end()); }

    /// How many bits of the current byte are used (0...7); 0 means on a byte boundary.
    int bitOffset() const { return heldBits; }

private:
    std::vector<uint8_t>& output;
    uint64_t buffer = 0;
    int heldBits = 0;
};

/// A canonical Huffman code ready for writing: the length of every symbol's code (0 = unused) and
/// the code itself, bit-reversed so `BitWriter::bits` can write it directly.
struct HuffmanCode {
    std::vector<uint8_t> lengths;
    std::vector<uint16_t> reversedCodes;
};

/// The canonical code of RFC 1951, 3.2.2 for `lengths`.
///
/// How: the first code of length L is (first code of L−1 + how many codes have length L−1) · 2,
/// and the symbols of one length take consecutive codes in symbol order, so the code lengths alone
/// determine every code and the stream never has to send the codes.
HuffmanCode canonicalCode(std::vector<uint8_t> lengths) {
    std::array<uint16_t, maximumCodeLength + 2> countOfLength{};
    for (const uint8_t length : lengths) countOfLength[length] += 1;
    countOfLength[0] = 0;

    std::array<uint16_t, maximumCodeLength + 2> nextCode{};
    uint16_t code = 0;
    for (int length = 1; length <= maximumCodeLength; ++length) {
        code = static_cast<uint16_t>((code + countOfLength[length - 1]) << 1);
        nextCode[length] = code;
    }

    HuffmanCode result;
    result.reversedCodes.assign(lengths.size(), 0);
    for (size_t symbol = 0; symbol < lengths.size(); ++symbol) {
        const int length = lengths[symbol];
        if (length != 0) result.reversedCodes[symbol] = reverseBits(nextCode[length]++, length);
    }
    result.lengths = std::move(lengths);
    return result;
}

/// A leaf (one symbol) or a package (two nodes merged) in the package-merge lists below.
struct PackageNode {
    uint64_t weight = 0;
    int32_t left = -1;     // child nodes of a package; -1 for a leaf
    int32_t right = -1;
    int32_t symbol = -1;   // the symbol of a leaf
};

/// Adds, to every symbol under `index`, one to its length: a leaf is one coin of a symbol.
void addLeafCoins(const std::vector<PackageNode>& nodes, int32_t index, std::vector<uint8_t>& lengths) {
    const PackageNode& node = nodes[static_cast<size_t>(index)];
    if (node.symbol >= 0) {
        lengths[static_cast<size_t>(node.symbol)] += 1;
        return;
    }
    addLeafCoins(nodes, node.left, lengths);
    addLeafCoins(nodes, node.right, lengths);
}

/// The code length of every symbol of an optimal prefix code for `frequencies` in which no code is
/// longer than `maximumLength` (0 for a symbol that is never used). Always gives at least two
/// codes, so the code is complete and every decoder accepts it: if fewer than two symbols are used,
/// unused ones with the lowest numbers are given codes too.
///
/// How: the package-merge algorithm (Larmore and Hirschberg, 1990). Think of each used symbol as a
/// coin of value 2⁻¹, 2⁻², …, 2⁻ᴸ (L = `maximumLength`) with its frequency as the cost of each.
/// A symbol's code length is how many of its coins are chosen, and a set of lengths is a prefix
/// code exactly when the chosen values total n − 1 (n symbols), which is the Kraft sum
/// Σ 2⁻ˡᵉⁿᵍᵗʰ = 1. Going from the smallest value up, each list is the symbols' coins merged with
/// the pairs ("packages") of the cheapest coins of the list before; the cheapest 2n − 2 items of
/// the final list are the choice. It is optimal among codes of limited length, so it equals the
/// plain Huffman code whenever that one is short enough, and it needs 2ᴸ ≥ n, true for 286 symbols
/// at 15 bits and 19 at 7.
std::vector<uint8_t> limitedCodeLengths(std::span<const uint32_t> frequencies, int maximumLength) {
    std::vector<uint8_t> lengths(frequencies.size(), 0);

    std::vector<size_t> symbols;
    for (size_t symbol = 0; symbol < frequencies.size(); ++symbol) {
        if (frequencies[symbol] > 0) symbols.push_back(symbol);
    }
    for (size_t symbol = 0; symbols.size() < 2 && symbol < frequencies.size(); ++symbol) {
        if (frequencies[symbol] == 0) symbols.push_back(symbol);
    }
    std::stable_sort(symbols.begin(), symbols.end(),
                     [&](size_t first, size_t second) { return frequencies[first] < frequencies[second]; });

    std::vector<PackageNode> nodes;
    std::vector<int32_t> leaves;
    for (const size_t symbol : symbols) {
        nodes.push_back({frequencies[symbol], -1, -1, static_cast<int32_t>(symbol)});
        leaves.push_back(static_cast<int32_t>(nodes.size() - 1));
    }
    const auto lighter = [&](int32_t first, int32_t second) {
        return nodes[static_cast<size_t>(first)].weight < nodes[static_cast<size_t>(second)].weight;
    };

    std::vector<int32_t> list = leaves;
    for (int level = 1; level < maximumLength; ++level) {
        std::vector<int32_t> packages;
        for (size_t index = 0; index + 1 < list.size(); index += 2) {
            PackageNode package;
            package.weight = nodes[static_cast<size_t>(list[index])].weight + nodes[static_cast<size_t>(list[index + 1])].weight;
            package.left = list[index];
            package.right = list[index + 1];
            nodes.push_back(package);
            packages.push_back(static_cast<int32_t>(nodes.size() - 1));
        }
        std::vector<int32_t> merged(leaves.size() + packages.size());
        std::merge(leaves.begin(), leaves.end(), packages.begin(), packages.end(), merged.begin(), lighter);
        list = std::move(merged);
    }

    const size_t chosen = std::min(list.size(), 2 * symbols.size() - 2);
    for (size_t index = 0; index < chosen; ++index) addLeafCoins(nodes, list[index], lengths);
    return lengths;
}

// ---- Writing: symbols --------------------------------------------------------------------------

/// The index (0...28) of the length symbol that `length` (3...258) belongs to, and so the symbol is
/// 257 + index (RFC 1951, 3.2.5). Length 258 has its own symbol, 285, rather than the last
/// 5-extra-bit range of symbol 284, as the RFC's table lists.
size_t lengthSymbolIndex(int length) {
    static const std::array<uint8_t, maximumMatchLength + 1> indexOfLength = [] {
        std::array<uint8_t, maximumMatchLength + 1> table{};
        for (size_t index = 0; index + 1 < lengthBase.size(); ++index) {
            for (int covered = lengthBase[index]; covered < lengthBase[index] + (1 << lengthExtraBits[index]); ++covered) {
                if (covered <= maximumMatchLength) table[static_cast<size_t>(covered)] = static_cast<uint8_t>(index);
            }
        }
        table[maximumMatchLength] = static_cast<uint8_t>(lengthBase.size() - 1);
        return table;
    }();
    return indexOfLength[static_cast<size_t>(length)];
}

/// The index (0...29) of the distance symbol that `distance` (1...32768) belongs to: the last
/// symbol whose smallest distance is not above it (RFC 1951, 3.2.5).
size_t distanceSymbolIndex(int distance) {
    size_t index = distanceBase.size() - 1;
    while (distanceBase[index] > distance) index -= 1;
    return index;
}

/// One step of the LZ77 parse: a literal byte (`distance` 0, the byte in `value`), or a copy of
/// `value` bytes (3...258) from `distance` bytes back (1...32768).
struct Token {
    uint16_t value;
    uint16_t distance;
};

/// How often each symbol occurs in a block's tokens, and how many extra bits they carry. The
/// end-of-block symbol is counted once.
struct SymbolCounts {
    std::array<uint32_t, literalLengthSymbolCount> literalLength{};
    std::array<uint32_t, distanceSymbolCount> distance{};
    uint64_t extraBits = 0;
};

SymbolCounts countSymbols(const std::vector<Token>& tokens) {
    SymbolCounts counts;
    for (const Token token : tokens) {
        if (token.distance == 0) {
            counts.literalLength[token.value] += 1;
            continue;
        }
        const size_t lengthIndex = lengthSymbolIndex(token.value);
        const size_t distanceIndex = distanceSymbolIndex(token.distance);
        counts.literalLength[endOfBlockSymbol + 1 + lengthIndex] += 1;
        counts.distance[distanceIndex] += 1;
        counts.extraBits += lengthExtraBits[lengthIndex] + distanceExtraBits[distanceIndex];
    }
    counts.literalLength[endOfBlockSymbol] += 1;
    return counts;
}

/// The bits the symbols of `counts` take when written with the two codes: each symbol's count
/// times its code length, plus the extra bits.
uint64_t symbolBits(const SymbolCounts& counts, std::span<const uint8_t> literalLengthLengths,
                    std::span<const uint8_t> distanceLengths) {
    uint64_t bits = counts.extraBits;
    for (size_t symbol = 0; symbol < counts.literalLength.size(); ++symbol) {
        bits += static_cast<uint64_t>(counts.literalLength[symbol]) * literalLengthLengths[symbol];
    }
    for (size_t symbol = 0; symbol < counts.distance.size(); ++symbol) {
        bits += static_cast<uint64_t>(counts.distance[symbol]) * distanceLengths[symbol];
    }
    return bits;
}

/// Writes the tokens, then the end-of-block symbol, with the two codes (RFC 1951, 3.2.5).
void writeSymbols(BitWriter& writer, const std::vector<Token>& tokens, const HuffmanCode& literalLength,
                  const HuffmanCode& distance) {
    for (const Token token : tokens) {
        if (token.distance == 0) {
            writer.bits(literalLength.reversedCodes[token.value], literalLength.lengths[token.value]);
            continue;
        }
        const size_t lengthIndex = lengthSymbolIndex(token.value);
        const size_t lengthSymbol = endOfBlockSymbol + 1 + lengthIndex;
        writer.bits(literalLength.reversedCodes[lengthSymbol], literalLength.lengths[lengthSymbol]);
        writer.bits(static_cast<uint32_t>(token.value - lengthBase[lengthIndex]), lengthExtraBits[lengthIndex]);

        const size_t distanceIndex = distanceSymbolIndex(token.distance);
        writer.bits(distance.reversedCodes[distanceIndex], distance.lengths[distanceIndex]);
        writer.bits(static_cast<uint32_t>(token.distance - distanceBase[distanceIndex]), distanceExtraBits[distanceIndex]);
    }
    writer.bits(literalLength.reversedCodes[endOfBlockSymbol], literalLength.lengths[endOfBlockSymbol]);
}

// ---- Writing: the three block types ------------------------------------------------------------

/// The bits `length` raw bytes take as stored blocks when written from bit `bitOffset` of a byte
/// (RFC 1951, 3.2.4): per block of at most 65535 bytes, 3 header bits, padding to a byte boundary,
/// 4 bytes of LEN and NLEN, and the bytes. After the first block the writer is on a boundary, so
/// each later header's padding is 5 bits. An empty range still takes one (empty) block.
uint64_t storedBlocksBits(size_t length, int bitOffset) {
    const size_t blockCount = std::max<size_t>(1, (length + maximumStoredLength - 1) / maximumStoredLength);
    const int firstPadding = (8 - (bitOffset + 3) % 8) % 8;
    return blockCount * (3 + 32) + static_cast<uint64_t>(firstPadding) + (blockCount - 1) * 5 + 8 * static_cast<uint64_t>(length);
}

/// Writes `bytes` as stored blocks, only the last of them marked final when `isFinal`.
void writeStoredBlocks(BitWriter& writer, std::span<const uint8_t> bytes, bool isFinal) {
    size_t written = 0;
    do {
        const size_t length = std::min(maximumStoredLength, bytes.size() - written);
        const bool isLast = written + length == bytes.size();
        writer.bits(isFinal && isLast ? 1 : 0, 1);
        writer.bits(0, 2);
        writer.alignToByte();
        writer.bits(static_cast<uint32_t>(length), 16);
        writer.bits(static_cast<uint32_t>(length ^ 0xFFFFu), 16);
        writer.wholeBytes(bytes.subspan(written, length));
        written += length;
    } while (written < bytes.size());
}

/// One entry of a dynamic header's run-length encoded code lengths (RFC 1951, 3.2.7): a symbol of
/// the code-length alphabet, and for 16, 17 and 18 the repeat count minus its minimum, in the
/// extra bits those symbols carry (2, 3 and 7).
struct CodeLengthItem {
    uint8_t symbol;
    uint8_t extraBitCount;
    uint8_t extraValue;
};

/// `lengths` as code-length symbols: a length 0...15 as itself, 16 for "repeat the previous length
/// 3...6 times", 17 for "3...10 zeros", 18 for "11...138 zeros". Each run of equal lengths is cut
/// with the longest repeats first; what is left under 3 is written plainly. A run may cross from
/// the literal/length lengths into the distance lengths, which the RFC allows.
std::vector<CodeLengthItem> runLengthEncoded(std::span<const uint8_t> lengths) {
    std::vector<CodeLengthItem> items;
    size_t index = 0;
    while (index < lengths.size()) {
        const uint8_t value = lengths[index];
        size_t run = 1;
        while (index + run < lengths.size() && lengths[index + run] == value) run += 1;
        index += run;

        if (value == 0) {
            while (run >= 11) {
                const size_t repeat = std::min<size_t>(run, 138);
                items.push_back({18, 7, static_cast<uint8_t>(repeat - 11)});
                run -= repeat;
            }
            if (run >= 3) {
                items.push_back({17, 3, static_cast<uint8_t>(run - 3)});
                run = 0;
            }
        } else {
            items.push_back({value, 0, 0});
            run -= 1;
            while (run >= 3) {
                const size_t repeat = std::min<size_t>(run, 6);
                items.push_back({16, 2, static_cast<uint8_t>(repeat - 3)});
                run -= repeat;
            }
        }
        for (; run > 0; --run) items.push_back({value, 0, 0});
    }
    return items;
}

/// Everything needed to write one block as dynamic Huffman (RFC 1951, 3.2.7) and what it would cost.
struct DynamicBlockPlan {
    HuffmanCode literalLength;       // 286 symbols
    HuffmanCode distance;            // 30 symbols
    HuffmanCode codeLength;          // 19 symbols
    size_t literalLengthCount = 0;   // HLIT + 257
    size_t distanceCount = 0;        // HDIST + 1
    size_t codeLengthCount = 0;      // HCLEN + 4
    std::vector<CodeLengthItem> items;
    uint64_t bits = 0;               // the whole block: 3 header bits, the header, the symbols, end of block
};

/// The dynamic-Huffman plan for a block with the symbol counts `counts`.
///
/// Codes are limited to 15 bits (7 for the code-length code, whose lengths take 3 bits). At least
/// one distance code is defined even when the block has no copies, because HDIST counts at least
/// one. Trailing unused symbols are left out of the header, to the minimum the format allows.
DynamicBlockPlan planDynamicBlock(const SymbolCounts& counts) {
    DynamicBlockPlan plan;
    plan.literalLength = canonicalCode(limitedCodeLengths(counts.literalLength, maximumCodeLength));
    plan.distance = canonicalCode(limitedCodeLengths(counts.distance, maximumCodeLength));

    plan.literalLengthCount = literalLengthSymbolCount;
    while (plan.literalLengthCount > 257 && plan.literalLength.lengths[plan.literalLengthCount - 1] == 0) plan.literalLengthCount -= 1;
    plan.distanceCount = distanceSymbolCount;
    while (plan.distanceCount > 1 && plan.distance.lengths[plan.distanceCount - 1] == 0) plan.distanceCount -= 1;

    std::vector<uint8_t> sequence(plan.literalLength.lengths.begin(),
                                  plan.literalLength.lengths.begin() + static_cast<std::ptrdiff_t>(plan.literalLengthCount));
    sequence.insert(sequence.end(), plan.distance.lengths.begin(),
                    plan.distance.lengths.begin() + static_cast<std::ptrdiff_t>(plan.distanceCount));
    plan.items = runLengthEncoded(sequence);

    std::array<uint32_t, codeLengthSymbolCount> itemCounts{};
    for (const CodeLengthItem& item : plan.items) itemCounts[item.symbol] += 1;
    plan.codeLength = canonicalCode(limitedCodeLengths(itemCounts, maximumCodeLengthCodeLength));

    plan.codeLengthCount = codeLengthSymbolCount;
    while (plan.codeLengthCount > 4 && plan.codeLength.lengths[codeLengthOrder[plan.codeLengthCount - 1]] == 0) plan.codeLengthCount -= 1;

    uint64_t headerBits = 5 + 5 + 4 + 3 * plan.codeLengthCount;
    for (const CodeLengthItem& item : plan.items) headerBits += plan.codeLength.lengths[item.symbol] + item.extraBitCount;
    plan.bits = 3 + headerBits + symbolBits(counts, plan.literalLength.lengths, plan.distance.lengths);
    return plan;
}

void writeDynamicBlock(BitWriter& writer, const DynamicBlockPlan& plan, const std::vector<Token>& tokens, bool isFinal) {
    writer.bits(isFinal ? 1 : 0, 1);
    writer.bits(2, 2);
    writer.bits(static_cast<uint32_t>(plan.literalLengthCount - 257), 5);
    writer.bits(static_cast<uint32_t>(plan.distanceCount - 1), 5);
    writer.bits(static_cast<uint32_t>(plan.codeLengthCount - 4), 4);
    for (size_t index = 0; index < plan.codeLengthCount; ++index) writer.bits(plan.codeLength.lengths[codeLengthOrder[index]], 3);
    for (const CodeLengthItem& item : plan.items) {
        writer.bits(plan.codeLength.reversedCodes[item.symbol], plan.codeLength.lengths[item.symbol]);
        writer.bits(item.extraValue, item.extraBitCount);
    }
    writeSymbols(writer, tokens, plan.literalLength, plan.distance);
}

/// Writes one block holding `tokens`, which reproduce `rawBytes`, in the smallest of the three
/// types. Sizes are computed exactly, so the choice never costs a wasted attempt.
void writeBestBlock(BitWriter& writer, const std::vector<Token>& tokens, std::span<const uint8_t> rawBytes, bool isFinal) {
    static const HuffmanCode fixedLiteralLength = canonicalCode(fixedLiteralLengthLengths());
    static const HuffmanCode fixedDistance = canonicalCode(fixedDistanceLengths());

    const SymbolCounts counts = countSymbols(tokens);
    const uint64_t fixedBits = 3 + symbolBits(counts, fixedLiteralLength.lengths, fixedDistance.lengths);
    const DynamicBlockPlan plan = planDynamicBlock(counts);
    const uint64_t storedBits = rawBytes.empty() ? UINT64_MAX : storedBlocksBits(rawBytes.size(), writer.bitOffset());

    if (storedBits <= fixedBits && storedBits <= plan.bits) {
        writeStoredBlocks(writer, rawBytes, isFinal);
    } else if (fixedBits <= plan.bits) {
        writer.bits(isFinal ? 1 : 0, 1);
        writer.bits(1, 2);
        writeSymbols(writer, tokens, fixedLiteralLength, fixedDistance);
    } else {
        writeDynamicBlock(writer, plan, tokens, isFinal);
    }
}

// ---- Writing: finding repeats ------------------------------------------------------------------

/// How hard a level searches, in the manner of zlib's table (levels 1-3 take the first match
/// found, 4-9 look one byte ahead for a longer one):
///   goodLength   a previous match this long makes the lookahead search a quarter as deep;
///   lazyLength   a match this long is taken without looking ahead (the lazy levels only);
///   niceLength   the search stops at a match this long;
///   chainLimit   at most this many earlier positions are tried.
struct SearchSettings {
    int goodLength;
    int lazyLength;
    int niceLength;
    int chainLimit;
    bool lookAhead;
};

SearchSettings searchSettingsFor(int level) {
    static const std::array<SearchSettings, 10> table = {{
        {0, 0, 0, 0, false},          // 0: stored only, never searched
        {4, 4, 8, 4, false},          // 1
        {4, 5, 16, 8, false},         // 2
        {4, 6, 32, 32, false},        // 3
        {4, 4, 16, 16, true},         // 4
        {8, 16, 32, 32, true},        // 5
        {8, 16, 128, 128, true},      // 6: the default
        {8, 32, 128, 256, true},      // 7
        {32, 128, 258, 1024, true},   // 8
        {32, 258, 258, 4096, true},   // 9
    }};
    return table[static_cast<size_t>(std::clamp(level, 0, 9))];
}

/// A copy found by the search: `length` below 3 means none.
struct Match {
    int length = 0;
    int distance = 0;
};

/// The LZ77 parse of `bytes` (Lempel and Ziv, 1977) with hash chains: positions are indexed by a
/// hash of their next 3 bytes, newer first, so the candidates for a match at a position are the
/// chain of earlier positions with the same hash. Written block by block into `writer`.
class Compressor {
public:
    Compressor(std::span<const uint8_t> bytes, int level, BitWriter& writer)
        : bytes(bytes), settings(searchSettingsFor(level)), writer(writer), head(hashCount, -1), previous(windowSize, -1) {}

    void run() {
        size_t position = 0;
        size_t blockStart = 0;
        Match current;
        bool haveCurrent = false;  // `current` was found already, by the look-ahead of the byte before
        while (position < bytes.size()) {
            if (tokens.size() >= blockTokenLimit) {
                writeBestBlock(writer, tokens, bytes.subspan(blockStart, position - blockStart), false);
                tokens.clear();
                blockStart = position;
            }
            if (!haveCurrent) {
                current = longestMatch(position, settings.chainLimit);
                insert(position);
            }
            haveCurrent = false;

            if (current.length < minimumMatchLength) {
                tokens.push_back({bytes[position], 0});
                position += 1;
                continue;
            }
            if (settings.lookAhead && current.length < settings.lazyLength && position + 1 < bytes.size()) {
                // A longer match one byte later beats this one: write this byte as a literal and
                // carry the better match to the next round.
                const int chain = current.length >= settings.goodLength ? settings.chainLimit / 4 : settings.chainLimit;
                const Match later = longestMatch(position + 1, chain);
                insert(position + 1);
                if (later.length > current.length) {
                    tokens.push_back({bytes[position], 0});
                    position += 1;
                    current = later;
                    haveCurrent = true;
                    continue;
                }
                tokens.push_back({static_cast<uint16_t>(current.length), static_cast<uint16_t>(current.distance)});
                insertRange(position + 2, position + static_cast<size_t>(current.length));
            } else {
                tokens.push_back({static_cast<uint16_t>(current.length), static_cast<uint16_t>(current.distance)});
                insertRange(position + 1, position + static_cast<size_t>(current.length));
            }
            position += static_cast<size_t>(current.length);
        }
        writeBestBlock(writer, tokens, bytes.subspan(blockStart), true);
    }

private:
    static constexpr int hashBits = 15;
    static constexpr size_t hashCount = size_t{1} << hashBits;
    static constexpr size_t windowMask = windowSize - 1;
    /// A block ends after this many tokens, so its Huffman codes follow the data as it changes.
    static constexpr size_t blockTokenLimit = 32768;
    /// zlib's observation: a 3-byte match from farther than this costs more bits than 3 literals.
    static constexpr int farDistance = 4096;

    /// The hash of the 3 bytes at `position`: their value times a large odd constant (Knuth's
    /// multiplicative hashing), keeping the top `hashBits` bits, where the mixing is best.
    size_t hashAt(size_t position) const {
        const uint32_t value = static_cast<uint32_t>(bytes[position]) | (static_cast<uint32_t>(bytes[position + 1]) << 8)
                               | (static_cast<uint32_t>(bytes[position + 2]) << 16);
        return (value * 0x9E3779B1u) >> (32 - hashBits);
    }

    /// Makes `position` a candidate for later matches. Positions with fewer than 3 bytes left have
    /// nothing to hash and are skipped.
    void insert(size_t position) {
        if (position + minimumMatchLength > bytes.size()) return;
        const size_t hash = hashAt(position);
        previous[position & windowMask] = head[hash];
        head[hash] = static_cast<int32_t>(position);
    }

    void insertRange(size_t first, size_t end) {
        for (size_t position = first; position < end; ++position) insert(position);
    }

    /// How many bytes at `candidate` and `position` agree, up to `limit`.
    size_t commonLength(size_t candidate, size_t position, size_t limit) const {
        size_t length = 0;
        while (length < limit && bytes[candidate + length] == bytes[position + length]) length += 1;
        return length;
    }

    /// The longest match for the bytes at `position` among the earlier positions of the hash
    /// chain, trying at most `chainLimit` of them. Chains run from newer to older positions, so a
    /// link that does not go back is a slot reused by a newer position and ends the chain.
    Match longestMatch(size_t position, int chainLimit) const {
        Match best;
        if (position + minimumMatchLength > bytes.size()) return best;
        const size_t limit = std::min<size_t>(maximumMatchLength, bytes.size() - position);

        int32_t candidate = head[hashAt(position)];
        for (int tried = 0; candidate >= 0 && tried < chainLimit; ++tried) {
            const size_t distance = position - static_cast<size_t>(candidate);
            if (distance > windowSize) break;
            // Checking the byte that would extend the best match first skips most hopeless candidates cheaply.
            if (bytes[static_cast<size_t>(candidate) + static_cast<size_t>(best.length)] == bytes[position + static_cast<size_t>(best.length)]) {
                const int length = static_cast<int>(commonLength(static_cast<size_t>(candidate), position, limit));
                if (length > best.length) {
                    best = {length, static_cast<int>(distance)};
                    if (length >= settings.niceLength || static_cast<size_t>(length) == limit) break;
                }
            }
            const int32_t next = previous[static_cast<size_t>(candidate) & windowMask];
            if (next >= candidate) break;
            candidate = next;
        }
        if (best.length < minimumMatchLength || (best.length == minimumMatchLength && best.distance > farDistance)) return {};
        return best;
    }

    std::span<const uint8_t> bytes;
    SearchSettings settings;
    BitWriter& writer;
    std::vector<int32_t> head;      // newest position of each hash, or -1
    std::vector<int32_t> previous;  // the position before each one (modulo the window) with the same hash, or -1
    std::vector<Token> tokens;
};

// ---- zlib (RFC 1950) ---------------------------------------------------------------------------

/// The two-byte zlib header: CMF 0x78 (method 8 = DEFLATE, window 2¹⁵ = 32 KiB) and FLG with the
/// compression-level hint FLEVEL in its top 2 bits (RFC 1950, 2.2) and FCHECK in its low 5, chosen
/// so that CMF · 256 + FLG is a multiple of 31. FLEVEL follows zlib: 0 for levels 0 and 1, 1 for
/// 2...5, 2 for 6, 3 for 7...9. FDICT stays 0.
std::array<uint8_t, 2> zlibHeader(int level) {
    const uint8_t compressionMethodAndInfo = 0x78;
    const uint8_t levelHint = level < 2 ? 0 : level < 6 ? 1 : level == 6 ? 2 : 3;
    uint8_t flags = static_cast<uint8_t>(levelHint << 6);
    const int remainder = (compressionMethodAndInfo * 256 + flags) % 31;
    flags = static_cast<uint8_t>(flags + (31 - remainder) % 31);
    return {compressionMethodAndInfo, flags};
}

} // namespace

std::vector<uint8_t> Deflate::inflated(std::span<const uint8_t> compressed) {
    std::vector<uint8_t> output;
    inflateBlocks(compressed, output);
    return output;
}

std::vector<uint8_t> Deflate::deflated(std::span<const uint8_t> bytes, int level) {
    if (bytes.size() > static_cast<size_t>(INT32_MAX)) throw DeflateProblem("the bytes are too many to compress: 2 GiB is the most.");
    std::vector<uint8_t> output;
    output.reserve(bytes.size() / 2 + 64);
    BitWriter writer(output);
    if (std::clamp(level, 0, 9) == 0) {
        writeStoredBlocks(writer, bytes, true);
    } else {
        Compressor(bytes, level, writer).run();
    }
    writer.alignToByte();
    return output;
}

std::vector<uint8_t> Deflate::zlibInflated(std::span<const uint8_t> compressed) {
    if (compressed.size() < 2) fail("the zlib stream is cut short.");
    const uint8_t compressionMethodAndInfo = compressed[0];
    const uint8_t flags = compressed[1];
    if ((compressionMethodAndInfo * 256 + flags) % 31 != 0) fail("the zlib stream's header check bits are wrong.");
    if ((compressionMethodAndInfo & 0x0F) != 8) fail("the zlib stream does not use the DEFLATE method.");
    if ((compressionMethodAndInfo >> 4) > 7) fail("the zlib stream's window is larger than 32 KiB.");
    if ((flags & 0x20) != 0) fail("the zlib stream needs a preset dictionary, which is not supported.");

    std::vector<uint8_t> output;
    const size_t taken = inflateBlocks(compressed.subspan(2), output);
    const size_t trailerStart = 2 + taken;
    if (compressed.size() < trailerStart + 4) fail("the zlib stream is cut short.");
    uint32_t expected = 0;
    for (size_t index = 0; index < 4; ++index) expected = (expected << 8) | compressed[trailerStart + index];
    if (expected != adler32(output)) fail("the zlib stream's Adler-32 does not match its bytes.");
    return output;
}

std::vector<uint8_t> Deflate::zlibDeflated(std::span<const uint8_t> bytes, int level) {
    std::vector<uint8_t> output;
    const auto header = zlibHeader(std::clamp(level, 0, 9));
    output.assign(header.begin(), header.end());
    const std::vector<uint8_t> body = deflated(bytes, level);
    output.insert(output.end(), body.begin(), body.end());
    const uint32_t checksum = adler32(bytes);
    for (int shift = 24; shift >= 0; shift -= 8) output.push_back(static_cast<uint8_t>((checksum >> shift) & 0xFF));
    return output;
}

uint32_t Deflate::adler32(std::span<const uint8_t> bytes) {
    constexpr uint32_t modulus = 65521;  // the largest prime below 2¹⁶
    // 5552 is the most bytes that can be added before the 32-bit sums could overflow
    // (255 · n · (n + 1) / 2 + (n + 1) · 65520 < 2³²), so the modulo runs once per chunk, not per byte.
    constexpr size_t chunkSize = 5552;
    uint32_t sumOfBytes = 1;
    uint32_t sumOfSums = 0;
    for (size_t start = 0; start < bytes.size(); start += chunkSize) {
        const size_t end = std::min(bytes.size(), start + chunkSize);
        for (size_t index = start; index < end; ++index) {
            sumOfBytes += bytes[index];
            sumOfSums += sumOfBytes;
        }
        sumOfBytes %= modulus;
        sumOfSums %= modulus;
    }
    return (sumOfSums << 16) | sumOfBytes;
}

} // namespace editor
