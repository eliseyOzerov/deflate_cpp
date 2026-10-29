// DEFLATE and zlib (RFC 1951, RFC 1950): known streams read back, our own streams read back, and every
// way a stream can be broken. The byte vectors below were made by Python's zlib where a comment says so
// (python only produced the test data; nothing in the code depends on it).
#include <string>

#include "../Deflate/Deflate.h"
#include "Testing.h"

using namespace editor;

namespace {

using Bytes = std::vector<uint8_t>;

Bytes bytesOf(const std::string& text) { return Bytes(text.begin(), text.end()); }

std::string textOf(const Bytes& bytes) { return std::string(bytes.begin(), bytes.end()); }

/// The bytes a stream spelled out bit by bit holds: each '0' or '1' is the next bit of the stream, in
/// the order it is transmitted (the lowest bit of the first byte first). Spaces are ignored.
Bytes bytesFromBits(const std::string& bits) {
    Bytes bytes;
    int count = 0;
    for (const char bit : bits) {
        if (bit == ' ') continue;
        if (count % 8 == 0) bytes.push_back(0);
        if (bit == '1') bytes.back() = static_cast<uint8_t>(bytes.back() | (1u << (count % 8)));
        count += 1;
    }
    return bytes;
}

/// A simple linear congruential generator (Numerical Recipes constants), the same on every machine.
class Random {
public:
    explicit Random(uint32_t seed) : state(seed) {}
    uint8_t nextByte() {
        state = state * 1664525u + 1013904223u;
        return static_cast<uint8_t>(state >> 24);
    }
    uint32_t nextBelow(uint32_t limit) {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) % limit;
    }

private:
    uint32_t state;
};

Bytes randomBytes(size_t count, uint32_t seed) {
    Random random(seed);
    Bytes bytes(count);
    for (uint8_t& byte : bytes) byte = random.nextByte();
    return bytes;
}

Bytes repeatedText(size_t count) {
    const std::string paragraph =
        "The quick brown fox jumps over the lazy dog. A marble is a small sphere, and a marble editor "
        "turns every model into a node graph. Compression finds the repeats in text like this. ";
    std::string text;
    while (text.size() < count) text += paragraph;
    text.resize(count);
    return bytesOf(text);
}

/// Words chosen by a generator: English-like letters and spaces, but no long repeats.
Bytes shuffledWordsText(size_t count, uint32_t seed) {
    static const char* const words[] = {"the", "of", "and", "marble", "node", "graph", "a", "to", "in", "is", "you", "that",
                                        "it", "he", "was", "for", "on", "are", "as", "with", "his", "they", "at", "be",
                                        "this", "from", "have", "or", "by", "one", "had", "not", "but", "what", "all",
                                        "were", "when", "we", "there", "can", "an", "your", "which", "their", "said"};
    Random random(seed);
    std::string text;
    while (text.size() < count) {
        text += words[random.nextBelow(sizeof(words) / sizeof(words[0]))];
        text += random.nextBelow(12) == 0 ? ". " : " ";
    }
    text.resize(count);
    return bytesOf(text);
}

/// The block type (0 stored, 1 fixed, 2 dynamic) of the first block of a bare DEFLATE stream.
int firstBlockType(const Bytes& stream) { return (stream[0] >> 1) & 3; }

void expectRoundTrips(const Bytes& original, const char* name) {
    for (const int level : {0, 1, 6, 9}) {
        const Bytes bare = Deflate::deflated(original, level);
        const Bytes zlibbed = Deflate::zlibDeflated(original, level);
        const bool bareMatches = Deflate::inflated(bare) == original;
        const bool zlibMatches = Deflate::zlibInflated(zlibbed) == original;
        if (!bareMatches || !zlibMatches) {
            testing::fail(__FILE__, __LINE__, std::string("round trip failed for ") + name + " at level " + std::to_string(level));
        }
    }
}

} // namespace

TEST(adler32MatchesTheKnownValues) {
    EXPECT_EQUAL(Deflate::adler32(bytesOf("Wikipedia")), 0x11E60398u);  // the example of the Wikipedia article on Adler-32
    EXPECT_EQUAL(Deflate::adler32(Bytes{}), 1u);
    // "a": A = 1 + 97 = 98, B = 98, so 98 · 65536 + 98.
    EXPECT_EQUAL(Deflate::adler32(bytesOf("a")), 0x00620062u);
    // 100 000 bytes of 0xFF cross many 5552-byte chunks; the sums worked out in 64 bits with the modulo
    // taken at the end must equal the chunked result. A = 1 + 255 n, B = n + 255 n (n + 1) / 2.
    const Bytes ones(100000, 0xFF);
    const uint64_t n = 100000;
    const uint64_t a = (1 + 255 * n) % 65521;
    const uint64_t b = (n + 255 * n * (n + 1) / 2) % 65521;
    EXPECT_EQUAL(Deflate::adler32(ones), static_cast<uint32_t>((b << 16) | a));
}

TEST(inflatesTheZlibStreamOfTheEmptyString) {
    const Bytes stream = {0x78, 0x9c, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
    EXPECT(Deflate::zlibInflated(stream).empty());
    EXPECT(Deflate::inflated(Bytes{0x03, 0x00}).empty());
}

TEST(inflatesAHandBuiltStoredBlock) {
    // BFINAL = 1, BTYPE = 00, padding, LEN = 5, NLEN = ~5, then "hello".
    const Bytes stream = {0x01, 0x05, 0x00, 0xFA, 0xFF, 'h', 'e', 'l', 'l', 'o'};
    EXPECT_EQUAL(textOf(Deflate::inflated(stream)), std::string("hello"));
    // Two stored blocks, the first not final.
    const Bytes two = {0x00, 0x02, 0x00, 0xFD, 0xFF, 'h', 'i', 0x01, 0x01, 0x00, 0xFE, 0xFF, '!'};
    EXPECT_EQUAL(textOf(Deflate::inflated(two)), std::string("hi!"));
}

TEST(inflatesAHandBuiltFixedHuffmanBlock) {
    // 'a' is literal 97, code 00110000 + 97 = 10010001 (8 bits); then the end of block 0000000 (7 bits).
    // Bits in transmission order: BFINAL 1, BTYPE 01 as "1 0", the code, the end of block.
    EXPECT_EQUAL(textOf(Deflate::inflated(bytesFromBits("1 10 10010001 0000000"))), std::string("a"));
    // 'a', then a copy of length 5 from distance 1: length symbol 259 (code 0000011, no extra bits),
    // distance symbol 0 (00000): "aaaaaa".
    EXPECT_EQUAL(textOf(Deflate::inflated(bytesFromBits("1 10 10010001 0000011 00000 0000000"))), std::string("aaaaaa"));
}

TEST(inflatesAFixedHuffmanStreamFromZlib) {
    // python3: zlib.compress(b"hello hello hello hello", 9)  (a fixed-Huffman block with a copy)
    const Bytes stream = {0x78, 0xda, 0xcb, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0xc8, 0x40, 0x27, 0x01, 0x68, 0x03, 0x08, 0xb1};
    EXPECT_EQUAL((stream[2] >> 1) & 3, 1);
    EXPECT_EQUAL(textOf(Deflate::zlibInflated(stream)), std::string("hello hello hello hello"));
}

namespace {

// python3: zlib.compress(text, 9), a dynamic-Huffman block (BTYPE 2) of 225 bytes for the 314-byte text below.
const Bytes dynamicStream = {
    0x78, 0xda, 0x25, 0x8e, 0x4b, 0x5a, 0xc3, 0x30, 0x0c, 0x84, 0xaf, 0x32, 0xfb, 0x96, 0x1c, 0x00, 0x56, 0xd0, 0xa6, 0xb0, 0x00,
    0xbe, 0x2e, 0xb8, 0x80, 0x1a, 0xcb, 0x89, 0x89, 0x1f, 0x41, 0x72, 0x1a, 0xc2, 0xe9, 0x51, 0xd3, 0x9d, 0x46, 0x33, 0xfa, 0x35,
    0x1f, 0x24, 0x97, 0xc8, 0x08, 0x0a, 0xc2, 0x4b, 0xe4, 0xec, 0x58, 0x1e, 0x62, 0x18, 0x19, 0xec, 0x42, 0x2d, 0xd2, 0xa0, 0xbd,
    0xb2, 0xac, 0x48, 0xc5, 0x71, 0xbc, 0xa7, 0xb2, 0x8d, 0xe8, 0x85, 0xa6, 0x61, 0x0f, 0xca, 0x0e, 0xbc, 0x05, 0xb6, 0xad, 0xf9,
    0x8b, 0x84, 0x5a, 0x39, 0xa3, 0xe4, 0xce, 0x74, 0xc6, 0x61, 0xb7, 0x6b, 0x70, 0x28, 0x69, 0x12, 0x56, 0x65, 0x07, 0xad, 0xc2,
    0x94, 0xf4, 0x66, 0x9d, 0x8f, 0x27, 0xf8, 0x10, 0x59, 0x37, 0xcc, 0xf9, 0xf3, 0x15, 0x21, 0x51, 0x6f, 0x72, 0x56, 0xc6, 0xb1,
    0x3d, 0xbd, 0x3f, 0x7f, 0xb5, 0x4f, 0xa8, 0x83, 0x51, 0x95, 0xb3, 0x41, 0x8d, 0x58, 0x59, 0xab, 0xc2, 0xad, 0x99, 0x52, 0xe8,
    0xf0, 0x36, 0x7b, 0x9f, 0x28, 0xa3, 0xb3, 0xe7, 0xf6, 0x3a, 0xd4, 0x01, 0x26, 0x57, 0xb8, 0xe0, 0x3d, 0x8b, 0xdd, 0x20, 0xb2,
    0xb5, 0x11, 0xdd, 0xe3, 0x67, 0x0e, 0x95, 0xad, 0xbd, 0xe7, 0x05, 0xc2, 0x13, 0x53, 0xd5, 0x7b, 0x7d, 0x2d, 0x89, 0x6f, 0xae,
    0x8c, 0x2b, 0x96, 0x22, 0x4e, 0x1f, 0xf1, 0x4d, 0xdd, 0xe8, 0x68, 0x51, 0xc4, 0x72, 0x65, 0xa4, 0x15, 0x97, 0xd0, 0x43, 0xa7,
    0x21, 0xe4, 0x5f, 0x14, 0x6f, 0x61, 0x92, 0xfa, 0xd7, 0xfc, 0x03, 0x96, 0xd3, 0x6f, 0x4d};

const std::string dynamicStreamText =
    "Marble is a Blender-like editor. Every model is a node graph, and every node is written once in C++. "
    "Compressed streams in PDF files and PNG images use DEFLATE; this sentence tests dynamic Huffman codes with many different "
    "letters, quite a few repeats, and some quirky words: jackdaws love my big sphinx of quartz.";

} // namespace

TEST(inflatesADynamicHuffmanStreamFromZlib) {
    EXPECT_EQUAL((dynamicStream[2] >> 1) & 3, 2);
    EXPECT_EQUAL(textOf(Deflate::zlibInflated(dynamicStream)), dynamicStreamText);
}

TEST(inflatesAnOverlappingCopy) {
    // 'a' then a copy of 258 bytes from distance 1 (length symbol 285, code 11000101): 259 'a's.
    const Bytes stream = bytesFromBits("1 10 10010001 11000101 00000 0000000");
    EXPECT_EQUAL(textOf(Deflate::inflated(stream)), std::string(259, 'a'));
}

TEST(roundTripsEveryKindOfInput) {
    expectRoundTrips(Bytes{}, "empty");
    expectRoundTrips(Bytes{42}, "one byte");
    std::string abc;
    for (int index = 0; index < 1000; ++index) abc += "abc";
    expectRoundTrips(bytesOf(abc), "abc x 1000");
    Bytes everyByte;
    for (int value = 0; value < 256; ++value) everyByte.push_back(static_cast<uint8_t>(value));
    expectRoundTrips(everyByte, "all byte values");
    expectRoundTrips(randomBytes(200000, 1), "200 kB of random bytes");
    expectRoundTrips(repeatedText(300000), "300 kB of repeated text");
    expectRoundTrips(shuffledWordsText(300000, 7), "300 kB of shuffled words");
    expectRoundTrips(Bytes(100000, 0), "a run of zeros");
    expectRoundTrips(Bytes(65535, 7), "one full stored block of one byte");
    expectRoundTrips(Bytes(65536, 7), "just over one stored block");
}

TEST(roundTripsDataThatMixesRandomTextAndRuns) {
    // Random bytes (stored), then words (dynamic), then zeros (matches only), then random again: the
    // blocks of one stream choose different types.
    Bytes mixed = randomBytes(70000, 3);
    const Bytes words = shuffledWordsText(120000, 4);
    mixed.insert(mixed.end(), words.begin(), words.end());
    mixed.insert(mixed.end(), 50000, 0);
    const Bytes tail = randomBytes(40000, 5);
    mixed.insert(mixed.end(), tail.begin(), tail.end());
    expectRoundTrips(mixed, "mixed");
}

TEST(findsMatchesAsFarBackAsTheWindowAllows) {
    // The same 100 bytes twice, exactly 32768 bytes apart (the farthest a distance reaches), and then 32769 apart.
    for (const size_t gap : {size_t{32768}, size_t{32769}, size_t{32767}}) {
        Bytes data = randomBytes(gap + 100, 11);
        for (size_t index = 0; index < 100; ++index) data[gap + index] = data[index];
        expectRoundTrips(data, "window edge");
        // Within the window the second copy is found, so the output is much smaller than the input.
        if (gap <= 32768) {
            const Bytes compressed = Deflate::deflated(data, 9);
            EXPECT(compressed.size() < gap + 60);
        }
    }
}

TEST(picksTheSmallestBlockType) {
    EXPECT_EQUAL(firstBlockType(Deflate::deflated(randomBytes(1000, 2), 6)), 0);  // random bytes: stored
    EXPECT_EQUAL(firstBlockType(Deflate::deflated(bytesOf("abc"), 6)), 1);        // tiny: fixed
    EXPECT_EQUAL(firstBlockType(Deflate::deflated(Bytes{}, 6)), 1);               // empty: just the fixed end of block
    EXPECT_EQUAL(firstBlockType(Deflate::deflated(repeatedText(20000), 6)), 2);   // text: dynamic
    EXPECT_EQUAL(firstBlockType(Deflate::deflated(bytesOf("hello"), 0)), 0);      // level 0: always stored
    EXPECT_EQUAL(Deflate::deflated(Bytes{}, 6).size(), size_t{2});                // the 10 bits of a fixed empty block
}

TEST(compressesRepetitiveTextAndHigherLevelsNeverDoMuchWorse) {
    const Bytes text = repeatedText(300000);
    const size_t level1 = Deflate::deflated(text, 1).size();
    const size_t level6 = Deflate::deflated(text, 6).size();
    const size_t level9 = Deflate::deflated(text, 9).size();
    EXPECT(level6 * 20 < text.size());  // below 5 %
    EXPECT(level1 * 10 < text.size());
    EXPECT(level9 <= level6 + level6 / 20);

    // Words in random order have no long repeats, but plenty of short ones and few distinct letters.
    const Bytes words = shuffledWordsText(300000, 9);
    EXPECT(Deflate::deflated(words, 6).size() * 10 < words.size() * 5);
    EXPECT(Deflate::deflated(words, 9).size() <= Deflate::deflated(words, 1).size());

    // Random bytes cannot be compressed, and are stored: 5 bytes of overhead for each of the 7 blocks
    // of up to 32768 tokens that the 200 000 bytes are cut into.
    const Bytes noise = randomBytes(200000, 1);
    EXPECT(Deflate::deflated(noise, 6).size() <= noise.size() + 5 * 7 + 1);
}

TEST(writesAZlibHeaderWhoseCheckBitsHold) {
    for (int level = 0; level <= 9; ++level) {
        const Bytes stream = Deflate::zlibDeflated(bytesOf("x"), level);
        EXPECT_EQUAL(stream[0], 0x78);
        EXPECT_EQUAL((stream[0] * 256 + stream[1]) % 31, 0);
    }
    EXPECT_EQUAL(Deflate::zlibDeflated(Bytes{}, 6)[1], 0x9c);  // zlib's own default header 78 9c
    EXPECT_EQUAL(Deflate::zlibDeflated(Bytes{}, 9)[1], 0xda);  // and 78 da for level 9
    EXPECT_EQUAL(Deflate::zlibDeflated(Bytes{}, 1)[1], 0x01);  // and 78 01 for level 1
}

TEST(rejectsStreamsThatAreCutShort) {
    EXPECT_PROBLEM(Deflate::inflated(Bytes{}), "cut short");
    Bytes truncated(dynamicStream.begin(), dynamicStream.begin() + 100);
    EXPECT_PROBLEM(Deflate::zlibInflated(truncated), "cut short");
    // Without its 4-byte trailer, and with only part of it.
    Bytes noTrailer(dynamicStream.begin(), dynamicStream.end() - 4);
    EXPECT_PROBLEM(Deflate::zlibInflated(noTrailer), "cut short");
    Bytes shortTrailer(dynamicStream.begin(), dynamicStream.end() - 1);
    EXPECT_PROBLEM(Deflate::zlibInflated(shortTrailer), "cut short");
    EXPECT_PROBLEM(Deflate::zlibInflated(Bytes{0x78}), "cut short");
    // A stored block that promises 5 bytes and has 2.
    EXPECT_PROBLEM(Deflate::inflated(Bytes{0x01, 0x05, 0x00, 0xFA, 0xFF, 'h', 'e'}), "cut short");
}

TEST(rejectsABrokenBlock) {
    // BFINAL 1, BTYPE 11: reserved.
    EXPECT_PROBLEM(Deflate::inflated(Bytes{0x07}), "block type 3");
    // LEN = 5 but NLEN = 0xFEFA is not its complement.
    EXPECT_PROBLEM(Deflate::inflated(Bytes{0x01, 0x05, 0x00, 0xFA, 0xFE, 'h', 'e', 'l', 'l', 'o'}), "complement");
    // A fixed block that starts with a copy: nothing to copy from (length symbol 257, distance symbol 0).
    EXPECT_PROBLEM(Deflate::inflated(bytesFromBits("1 10 0000001 00000 0000000")), "before its start");
    // 'a', then a copy from distance 2 (distance symbol 1, 00001): only one byte exists.
    EXPECT_PROBLEM(Deflate::inflated(bytesFromBits("1 10 10010001 0000001 10000 0000000")), "before its start");
    // A dynamic header whose code-length code has three codes of one bit: over-subscribed.
    EXPECT_PROBLEM(Deflate::inflated(bytesFromBits("1 01 00000 00000 0000 100 100 100 000")), "over-subscribed");
    // A code-length code with a single one-bit code is incomplete, which that code may not be.
    EXPECT_PROBLEM(Deflate::inflated(bytesFromBits("1 01 00000 00000 0000 100 000 000 000")), "incomplete");
    // Length symbol 286 exists only in the fixed code's table and is not a length (code 11000110).
    EXPECT_PROBLEM(Deflate::inflated(bytesFromBits("1 10 11000110 00000")), "does not exist");
    // HLIT = 31 announces 288 literal/length symbols, more than the 286 there are.
    EXPECT_PROBLEM(Deflate::inflated(bytesFromBits("1 01 11111 00000 0000")), "more Huffman symbols");
}

TEST(rejectsABrokenZlibWrapper) {
    // The check bits: 0x78 0x9d is not a multiple of 31.
    EXPECT_PROBLEM(Deflate::zlibInflated(Bytes{0x78, 0x9d, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01}), "check bits");
    // FDICT set: 0x78 0x20 passes the check (30752 = 31 · 992) and asks for a dictionary.
    EXPECT_PROBLEM(Deflate::zlibInflated(Bytes{0x78, 0x20, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01}), "dictionary");
    // Method 9 instead of 8, with check bits that are right.
    Bytes otherMethod = {0x79, 0x00};
    otherMethod[1] = static_cast<uint8_t>((31 - (0x79 * 256) % 31) % 31);
    EXPECT_PROBLEM(Deflate::zlibInflated(otherMethod), "DEFLATE method");
    // A window of 2^16, which zlib streams may not claim: CINFO 8.
    Bytes bigWindow = {0x88, 0x00};
    bigWindow[1] = static_cast<uint8_t>((31 - (0x88 * 256) % 31) % 31);
    EXPECT_PROBLEM(Deflate::zlibInflated(bigWindow), "window");
    // The trailer's last byte changed: the Adler-32 no longer matches.
    EXPECT_PROBLEM(Deflate::zlibInflated(Bytes{0x78, 0x9c, 0x03, 0x00, 0x00, 0x00, 0x00, 0x02}), "Adler-32");
    Bytes wrongChecksum = Deflate::zlibDeflated(bytesOf("checksummed"), 6);
    wrongChecksum[wrongChecksum.size() - 3] ^= 0x10;
    EXPECT_PROBLEM(Deflate::zlibInflated(wrongChecksum), "Adler-32");
}

TEST(clampsLevelsOutsideZeroToNine) {
    const Bytes text = repeatedText(5000);
    EXPECT(Deflate::inflated(Deflate::deflated(text, -3)) == text);
    EXPECT(Deflate::inflated(Deflate::deflated(text, 42)) == text);
    EXPECT(Deflate::deflated(text, -3) == Deflate::deflated(text, 0));
}
