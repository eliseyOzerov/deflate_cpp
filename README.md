# deflate_cpp

DEFLATE (RFC 1951) and zlib (RFC 1950) streams compressed and decompressed: hash chains over a 32 KiB window, and each block written as the smallest of stored, fixed and dynamic Huffman (package-merge code lengths). Output decompresses with zlib byte for byte and is about the size of zlib's.

C++20 with only the standard library. One of a family of small libraries built and used the same
way: [math_cpp](https://github.com/eliseyOzerov/math_cpp), xml_cpp, deflate_cpp, css_cpp and
opentype_cpp.

## Build and test

```bash
make test
```

builds `build/lib/libdeflate_cpp.a` and runs the unit tests.

## Using it

**make or any build system:** put the folder that holds `deflate_cpp` on the include path, include
`"deflate_cpp/Deflate/<Header>.h"`, and link `libdeflate_cpp.a`.

**SwiftPM:** the product is `DeflateCpp`.

```swift
.package(path: "../deflate_cpp"),
// ...
.target(name: "App", dependencies: [.product(name: "DeflateCpp", package: "deflate_cpp")],
        swiftSettings: [.interoperabilityMode(.Cxx)])
```

Everything is in the C++ namespace `editor`, as in math_cpp. Values come back in the format's own
terms; turning them into a host's types is the host's job.

## License

MIT, see [LICENSE](LICENSE).
