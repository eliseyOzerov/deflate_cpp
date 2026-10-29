# deflate_cpp: agent instructions

DEFLATE and zlib compression in C++20 with only the standard library, shared by several products (Marble first). Humans must
be able to read, understand and maintain every line. The rules are math_cpp's, applied to formats.

## Layout

- `Deflate/` is the library: its headers are public, and it is the one folder the static library
  and the SwiftPM target build. `Tests/` holds the tests and their runner.
- The library holds the format in its own terms and nothing of any host: no host colour, vector,
  path or problem types. A host turns what it returns into its own values.
- A damaged file is reported with the library's own `…Problem : std::runtime_error`, one readable
  sentence saying what is wrong and where.

## Self-contained

Only the C++ standard library. Write each format from its specification and cite it in the file
header (RFC, W3C, ISO or vendor note, with the section). Other implementations may be read for
structure, never pasted; never copy code from GPL projects.

## Readability

- **Functions over expressions:** an expression may appear inline only when it is a one-line,
  one-off use. A concept with a name in the format (a Huffman code length, a CSS specificity, a
  charstring operand) is a named function, written once.
- Full, textbook names; no one-letter names except loop counters where unavoidable.
- Comments explain *why*. Every public function's doc comment says what it returns, how, and its
  conventions (units, byte order, edge cases), and ends with:

```cpp
/// Use when: reading the stylesheet of an SVG exported by Figma
/// Tags: css, stylesheet, selector, specificity
/// Node: none (file format)
```

## Tests

The tests are in `Tests/*Tests.cpp` (`TEST(name) { ... }`, `Tests/Testing.h`):
hand-worked values, every error with its message, round trips where the module writes, and a
check against another implementation where one exists (say which in the test).

```bash
make test
```

## Git

Never add `Co-Authored-By` lines to commit messages.
