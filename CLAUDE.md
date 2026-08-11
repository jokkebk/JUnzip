# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
make                # Build all executables (no compression support)
make HAVE_ZLIB=1    # Build with zlib (fast decompression + compression)
make HAVE_PUFF=1    # Build with puff.c (lighter decompression, no zlib dependency)
make clean          # Remove object files and built binaries
make check          # Structure and security regression tests
make sanitize       # Security tests under ASan/UBSan for both backends
```

Run `make clean` when switching between `HAVE_ZLIB` and `HAVE_PUFF`; the
feature defines are compile-time choices. The zlib build links the system
library with `-lz`.

Single file compile example (no make):
```bash
gcc -DHAVE_PUFF junzip_demo.c junzip.c puff.c -o junzip_demo
```

On Windows/MinGW, the Makefile adds `-mno-ms-bitfields` automatically for correct packed struct sizes.

## Testing

```bash
make check              # Runs structure and in-memory security tests
make HAVE_PUFF=1 check  # Includes puff deflate coverage
make HAVE_ZLIB=1 check  # Includes zlib deflate coverage
```

There is no test framework; `junzip_test.c` checks structure sizes and
`junzip_security_test.c` builds malicious/edge-case ZIP fixtures entirely in
memory.

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
- Data descriptor support (bit 3 flag): `jzReadData` auto-consumes and verifies the trailing descriptor; callers should pre-fill `JZFileHeader` with central directory sizes when bit 3 is set
- `jzReadData` requires a caller-provided output-buffer length, enforces exact
  production, and verifies CRC32. ZIP64 and encrypted entries are rejected.
- The library has no global mutable state. Separate `JZFile` instances may be
  used concurrently; a single backend handle still requires its own
  synchronization if shared between threads.
