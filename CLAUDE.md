# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
make                # Build all executables (no compression support)
make HAVE_ZLIB=1    # Build with zlib (fast decompression + compression)
make HAVE_PUFF=1    # Build with puff.c (lighter decompression, no zlib dependency)
make check          # Run struct and security regression tests
make sanitize       # Run Puff and zlib tests under ASan/UBSan
make fuzz           # Build optional libFuzzer harness (Clang runtime required)
make fuzz-smoke     # Build standalone mutation/truncation harness
make clean          # Remove object files and built binaries
```

Single file compile example (no make):
```bash
gcc -DHAVE_PUFF junzip_demo.c junzip.c puff.c -o junzip_demo
```

On Windows/MinGW, the Makefile adds `-mno-ms-bitfields` automatically for correct packed struct sizes.

## Testing

`make check`, `make HAVE_PUFF=1 check`, and `make HAVE_ZLIB=1 check` cover the
store-only and both Deflate configurations. `junzip_security_test.c` builds
hostile archives in memory; `junzip_fuzz.c` provides libFuzzer and standalone
smoke modes. Clean before switching backends so differently compiled objects
are not reused.

## Architecture

JUnzip is a minimalistic C library for reading and writing ZIP files. The entire library is two files:

- **junzip.h** — Public API, packed struct definitions for ZIP format headers (local, global, end record), and the `JZFile` abstraction (vtable with read/tell/seek/error/close)
- **junzip.c** — Implementation: end record scanning, central directory iteration, local header reading, data extraction (store/deflate), stdio `JZFile` adapter (`jzfile_from_stdio_file`)

### Decompression backends (compile-time, mutually exclusive)

| Define | Source | Capabilities |
|--------|--------|-------------|
| (none) | — | Store only (no decompression) |
| `HAVE_PUFF` | `puff.c` | Deflate decompression via Mark Adler's puff |
| `HAVE_ZLIB` | zlib | Deflate decompression + compression in jzip_demo |

### Demo executables

- **junzip_demo.c** — POSIX safe reference extractor: validates the archive in a first pass, then extracts through descriptor-relative directory operations
- **jzip_demo.c** — Zip creator: builds ZIP from input files (store-only without zlib, compressed with zlib)
- **junzip_dump.c** — Debug tool: dumps all local + global headers and end record fields
- **junzip_test.c** — Struct size verification
- **junzip_security_test.c** — In-memory malicious-archive regression suite
- **junzip_fuzz.c** — Optional parser/extraction fuzz harness

### Key patterns

- All structs use `__attribute__((__packed__))` to match ZIP binary format exactly
- The `JZFile` vtable abstraction allows custom I/O backends (not just stdio)
- Central directory callback pattern: `jzReadCentralDirectory` calls a `JZRecordCallback` for each entry
- Data descriptor support (bit 3 flag): `jzReadData` auto-consumes the trailing descriptor; callers should pre-fill `JZFileHeader` with central directory sizes when bit 3 is set
- `JZFile` uses checked 64-bit positions; `tell` reports failure explicitly
- `jzReadLocalFileHeaderChecked` compares local metadata with the central header before extraction
- Callers must impose independent entry-count, compressed-size, uncompressed-size, and aggregate-output limits before allocating for `jzReadData`
- `SecurityAnalysis.md` records the original vulnerability analysis;
  `SecurityReview.md` records the post-reconciliation residual-risk review
