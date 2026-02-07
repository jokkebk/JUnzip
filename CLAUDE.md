# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
make                # Build all executables (no compression support)
make HAVE_ZLIB=1    # Build with zlib (fast decompression + compression)
make HAVE_PUFF=1    # Build with puff.c (lighter decompression, no zlib dependency)
make clean          # Remove object files and built binaries
```

Single file compile example (no make):
```bash
gcc -DHAVE_PUFF junzip_demo.c junzip.c puff.c -o junzip_demo
```

On Windows/MinGW, the Makefile adds `-mno-ms-bitfields` automatically for correct packed struct sizes.

## Testing

```bash
make && ./junzip_test   # Verifies struct sizes match ZIP spec (30, 46, 26, 22 bytes)
```

There is no test framework; `junzip_test.c` is a simple size-check executable.

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

- **junzip_demo.c** — Unzip: reads central directory, seeks to each entry, extracts files (creates directories as needed)
- **jzip_demo.c** — Zip creator: builds ZIP from input files (store-only without zlib, compressed with zlib)
- **junzip_dump.c** — Debug tool: dumps all local + global headers and end record fields
- **junzip_test.c** — Struct size verification

### Key patterns

- All structs use `__attribute__((__packed__))` to match ZIP binary format exactly
- The `JZFile` vtable abstraction allows custom I/O backends (not just stdio)
- Central directory callback pattern: `jzReadCentralDirectory` calls a `JZRecordCallback` for each entry
- Data descriptor support (bit 3 flag): `jzReadData` auto-consumes the trailing descriptor; callers should pre-fill `JZFileHeader` with central directory sizes when bit 3 is set
- `jzBuffer` is a global 64KB buffer used internally for reading and decompression
