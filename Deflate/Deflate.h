// DEFLATE compression (RFC 1951) and its zlib wrapping (RFC 1950), written from the RFCs: the
// streams inside PDF files, PNG images and zip archives are compressed this way.
#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace editor {

/// Compressing and decompressing bytes with DEFLATE, bare or wrapped as zlib.
///
/// Conventions: bare DEFLATE is the bit stream of RFC 1951 alone; zlib (RFC 1950) adds a two-byte
/// header and the Adler-32 of the uncompressed bytes after it. Decompressing accepts every block
/// type (stored, fixed Huffman, dynamic Huffman). Compressing finds repeats in a 32 KiB window with
/// hash chains and writes each block in whichever of the three types is smallest, so the output
/// may differ from zlib's byte for byte while decompressing to the same bytes.
///
/// Use when: reading a compressed PDF stream; reading or writing PNG image data
/// Tags: deflate, inflate, zlib, compress, decompress, flate, rfc 1951, rfc 1950, huffman, lz77
/// Node: none (file format)
struct Deflate {
    /// The bytes the bare DEFLATE stream `compressed` holds. Throws a `DeflateProblem` for a
    /// stream that is cut short, has a reserved block type, a stored block whose length and its
    /// complement disagree, an impossible Huffman code, or a distance reaching back before the start.
    ///
    /// Use when: a format stores a bare DEFLATE stream
    /// Tags: inflate, decompress, raw deflate, rfc 1951
    /// Node: none (file format)
    static std::vector<uint8_t> inflated(std::span<const uint8_t> compressed);

    /// `bytes` as a bare DEFLATE stream. `level` runs from 0 (stored blocks only) to 9 (longest
    /// search); 6 is zlib's default balance of size and time.
    ///
    /// Use when: a format stores a bare DEFLATE stream
    /// Tags: deflate, compress, raw deflate, rfc 1951
    /// Node: none (file format)
    static std::vector<uint8_t> deflated(std::span<const uint8_t> bytes, int level = 6);

    /// The bytes the zlib stream `compressed` holds. Throws a `DeflateProblem` for a header whose
    /// check bits fail, a method other than DEFLATE, a preset dictionary, or an Adler-32 that does
    /// not match the bytes, as well as for every problem `inflated` finds.
    ///
    /// Use when: reading a PDF `/FlateDecode` stream; reading a PNG's IDAT data
    /// Tags: zlib decompress, flatedecode, inflate, rfc 1950
    /// Node: none (file format)
    static std::vector<uint8_t> zlibInflated(std::span<const uint8_t> compressed);

    /// `bytes` as a zlib stream: header, `deflated(bytes, level)`, then the Adler-32.
    ///
    /// Use when: writing a PDF `/FlateDecode` stream; writing a PNG's IDAT data
    /// Tags: zlib compress, flatedecode, deflate, rfc 1950
    /// Node: none (file format)
    static std::vector<uint8_t> zlibDeflated(std::span<const uint8_t> bytes, int level = 6);

    /// The Adler-32 checksum of `bytes` (RFC 1950, section 8.2): two sums modulo 65521, A of the
    /// bytes plus one and B of the running values of A, packed as B · 65536 + A.
    ///
    ///     adler32("Wikipedia") = 0x11E60398
    ///
    /// Use when: checking a zlib stream
    /// Tags: adler32, checksum, zlib checksum
    /// Node: none (file format)
    static uint32_t adler32(std::span<const uint8_t> bytes);
};

/// A compressed stream that cannot be read, saying what is wrong with it.
class DeflateProblem : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

} // namespace editor
