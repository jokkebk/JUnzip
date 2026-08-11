# Security Analysis of JUnzip

**Date:** 2026-08-10
**Scope:** All code paths in `junzip.c`, `junzip.h`, `junzip_demo.c`, `jzip_demo.c`, `junzip_dump.c`, `junzip_test.c`, and the bundled `puff.c`/`puff.h`.
**Method:** Manual line-by-line review, plus dynamic verification with ASan/UBSan builds (`-fsanitize=address,undefined`) of both decompression backends (zlib and puff), 15 hand-crafted malicious/edge-case archives, and a 400-iteration bit-flip/truncation fuzz run against the zlib build.

## Executive summary

JUnzip's header-parsing paths are in decent shape for hobby code: short reads are checked almost everywhere, filename lengths are bounded before copies, unknown compression methods are rejected, and the demo has basic Zip Slip protection. A 400-iteration fuzz run produced no crashes in header parsing.

However, the **data extraction path is not safe to run on untrusted archives**. The fuzzer found a genuine out-of-bounds heap read within 128 iterations, and targeted PoCs confirmed three additional serious issues. The common root cause: the library treats attacker-controlled size fields (local header, data descriptor) as authoritative, never verifies how much data was actually produced, and its central API (`jzReadData`) has no output-buffer-length parameter.

If this library were a major software dependency processing untrusted zips, the issues below would need fixing. Everything listed is small in scope — the codebase is compact and the fixes are mostly additive validation. Section 4 contains a prioritized remediation plan.

### Severity overview

| # | Finding | Severity | Status |
|---|---------|----------|--------|
| 1 | Data descriptor values blindly trusted; returned to caller → OOB heap read | **High** | Confirmed (ASan + PoC) |
| 2 | `jzReadData` succeeds when decompression produces fewer bytes than claimed → uninitialized/OOB heap written to disk | **High** | Confirmed (PoC) |
| 3 | Local header silently overrides central directory sizes; API has no buffer-length parameter → heap overflow in plausible callers | **High** (latent) | Confirmed behavior |
| 4 | ZIP64 sentinel sizes (`0xFFFFFFFF`) not rejected → 4 GB allocations/writes per entry | **Medium-High** | Confirmed (PoC: 4 GB file from ~500 B zip) |
| 5 | `centralDirectoryOffset + centralDirectorySize` validation overflows uint32 | Low-Medium | Confirmed (PoC) |
| 6 | No CRC32 verification anywhere | Medium | By design (documented) |
| 7 | Central directory iteration not bounded by `centralDirectorySize` | Low-Medium | Confirmed (PoC) |
| 8 | Demo extractor: deny-list Zip Slip only; no symlink protection; unbounded output | Medium | Confirmed |
| 9 | Global mutable state (`jzBuffer`, puff statics) → not thread-safe/reentrant | Medium (API) | Documented in code |
| 10 | Correctness issues with security-adjacent impact | Low | Confirmed |

## 0. Implementation status

The remediation work described below is implemented in the current working
tree. The library now treats central-directory sizes as the input contract,
requires a caller-provided output bound, verifies local headers and data
descriptors instead of adopting their values, enforces exact decompression,
rejects unsupported ZIP64/encryption/version features, verifies CRC32, and
keeps parser state reentrant. The extractor adds component-wise filename
validation, NUL/control-character rejection, POSIX `O_NOFOLLOW` output
handling, budgets, escaped logging, and checked writes.

Build and regression verification is reproducible with `make check`,
`make HAVE_PUFF=1 check`, `make HAVE_ZLIB=1 check`, and `make sanitize`.
An optional libFuzzer harness (`junzip_fuzz.c`) and a sanitizer-backed
400-iteration mutation/truncation smoke mode are included. The repository
does not claim a 24-hour fuzz campaign; the permanent suite is an in-memory
regression suite and the sanitizer target covers both decompression backends.

---

## 1. Confirmed vulnerabilities

### 1.1 [HIGH] Data descriptor is blindly trusted and mutates the caller's size fields

**Location:** `jzReadDataDescriptor()` (junzip.c:204-228), called from `jzReadData()` (junzip.c:246-249, 287-290, 307-310).

When bit 3 of `generalPurposeBitFlag` is set, sizes and CRC are stored in a data descriptor *after* the file data. `jzReadData` reads the data using the pre-descriptor sizes, then parses the descriptor and **overwrites `header->crc32`, `header->compressedSize`, and `header->uncompressedSize` with descriptor values, with no consistency check** against the sizes that were actually consumed or the buffer that was actually filled.

Any caller that uses `header->uncompressedSize` after `jzReadData` returns — the natural thing to do — is handed an attacker-controlled length that no longer corresponds to the buffer contents.

**Demonstrated impact:** The demo does exactly this at `junzip_demo.c:105`:

```c
writeFile(filename, data, header.uncompressedSize);  // header mutated by descriptor
```

A PoC archive (stored entry, 64-byte payload, descriptor claiming `uncompressedSize = 0x100000`) produced a **245,760-byte output file from a 64-byte heap allocation** — an out-of-bounds heap read whose contents are written to the extracted file. The randomized fuzzer independently rediscovered this and ASan caught it directly:

```
ERROR: AddressSanitizer: heap-buffer-overflow
READ of size 2080
    #2 fwrite
    #3 writeFile junzip_demo.c:69
    #4 processFile junzip_demo.c:105
    #5 jzReadCentralDirectory junzip.c:129
```

(The deterministic PoC evaded ASan because `fwrite` of a very large block bypasses the instrumented `memcpy` path and goes straight to `write(2)`, which reads whatever heap pages happen to be mapped — silent heap disclosure instead of a crash. On a long-running process, those pages contain stale secrets.)

The same flaw exists on the deflate paths (zlib and puff): the descriptor is parsed *after* decompression and likewise overwrites the header.

**Fix:** Validate descriptor values against the values actually used. They must match the sizes that bounded the read/decompression; on mismatch, return an error and do not modify the caller's header. Ideally `jzReadData` should never mutate size fields after the fact — treat the descriptor as a checksum-like trailer to verify, not a source of new truth. Note that a lying descriptor claiming a *smaller* size is equally bad (caller truncates real data silently).

### 1.2 [HIGH] `jzReadData` reports success when decompression produced fewer bytes than claimed

**Location:** zlib path junzip.c:280-285; puff path junzip.c:303-305.

```c
ret = inflate(&strm, Z_FINISH);
inflateEnd(&strm);
free(comp);
if(ret != Z_STREAM_END)
    return (ret == Z_OK) ? Z_ERRNO : ret;
```

`Z_STREAM_END` only means the deflate stream terminated — not that `uncompressedSize` bytes were produced. `strm.total_out` is never compared against `header->uncompressedSize`. The puff path has the same gap: `puff()` returns 0 and updates `destlen` to the actual byte count, but `destlen` is never checked.

**Demonstrated impact:** A PoC claiming `uncompressedSize = 1000` with a deflate stream that actually decodes to 32 bytes extracted successfully (exit 0), and the demo wrote the claimed 1000 bytes — **968 bytes of uninitialized heap memory landed in the extracted file** (visible as ASan's `0xBE` malloc-fill pattern in the output). Combined with finding 1.4, a ~500-byte zip produced a **4 GB output file**, 32 bytes of which were real data.

Impact is twofold: information disclosure (stale heap contents written to disk) and integrity (silent truncation accepted as success).

**Fix:** Require `strm.total_out == header->uncompressedSize` (zlib) and `destlen == header->uncompressedSize` (puff) before returning `Z_OK`. Return the actual byte count to the caller (new out-param or return convention) so callers cannot write more than was produced.

### 1.3 [HIGH, latent] Local header silently overrides central directory sizes; `jzReadData` has no buffer-length parameter

**Location:** `jzReadLocalFileHeader()` junzip.c:190-197; `jzReadData()` junzip.c:233.

```c
if(localHeader.compressedSize)
    header->compressedSize = localHeader.compressedSize;
if(localHeader.uncompressedSize)
    header->uncompressedSize = localHeader.uncompressedSize;
```

A well-formed zip has identical sizes in the central directory and local headers. JUnzip silently lets the local header (attacker-controlled, like everything else in the file) **override** the central values. `jzReadData` then trusts `header->uncompressedSize` as both the number of bytes to read (store path: junzip.c:242) and the implicit size of the caller's buffer — yet the API gives the caller no way to state how large that buffer actually is.

The shipped demo survives because it allocates *after* the merge (`junzip_demo.c:91`). But a completely reasonable integration pattern — allocate based on the central directory entry, then read the local header and call `jzReadData` — is a **heap buffer overflow write**: PoC with central `uncompressedSize = 32` and local `uncompressedSize = 5000` confirms the merge happens (demo allocated and read 5000 bytes where the central directory said 32). In the store path those 5000 bytes are read straight into the caller's buffer.

**Fix (defense in depth):**

1. Add a buffer-length parameter: `int jzReadData(JZFile *zip, JZFileHeader *header, void *buffer, size_t bufferSize)` and fail if `uncompressedSize > bufferSize` (store) or set `avail_out = min(uncompressedSize, bufferSize)` *and* require exact-size completion (1.2).
2. Cross-validate local vs. central header fields (sizes, CRC, method, filename) and **fail on mismatch** instead of silently overriding. Zero is a legitimate value for all of these fields (empty file has CRC 0), so the "if nonzero, override" heuristic is also semantically wrong — see 3.4.

### 1.4 [MEDIUM-HIGH] ZIP64 sentinel values not rejected → 4 GB allocation and write per entry

**Location:** `jzReadData()` size handling; demo allocation `junzip_demo.c:91`.

ZIP64 encodes real sizes in an extra field and leaves `0xFFFFFFFF` in the 32-bit size fields. JUnzip neither parses ZIP64 extra fields nor rejects the sentinel. `0xFFFFFFFF` flows into `malloc()` (demo) and into `fwrite` of the same length (with 1.2 making the read "succeed").

**Demonstrated impact:** PoC with `uncompressedSize = 0xFFFFFFFF` and a 19-byte deflate stream: demo `malloc(4 GB)` succeeded (macOS overcommit), inflate produced 32 bytes, `jzReadData` returned success, and `writeFile` wrote a **4,294,967,295-byte file**. Multiply by `numEntries` for arbitrary disk exhaustion, plus the heap-disclosure angle of 1.2. `numEntries = 0xFFFF` and `centralDirectorySize = 0xFFFFFFFF` have analogous sentinel ambiguity.

**Fix:** Reject any entry whose size fields equal `0xFFFFFFFF` (and treat `numEntries == 0xFFFF` / `centralDirectoryOffset == 0xFFFFFFFF` as unsupported) with a clear "ZIP64 not supported" error, or implement ZIP64 properly. Add a configurable per-entry and total extraction size cap in the demo.

### 1.5 [LOW-MEDIUM] End-record validation overflows in 32-bit arithmetic

**Location:** `jzReadEndRecord()` junzip.c:67.

```c
if(endRecord->centralDirectoryOffset + endRecord->centralDirectorySize > fileSize)
```

Both operands are `uint32_t`; the sum is computed in 32 bits and wraps. PoC with `offset = 0xFFFFFFF0`, `size = 0x20`: sum wraps to `0x10`, the check passes, and processing continues with a garbage offset. In practice the subsequent `fseek`/`fread` fails and the library returns an error (verified — no crash), so impact is limited to a bypassed validation layer. On a hypothetical `JZFile` backend whose `seek` wraps or clamps instead of failing, the bypass would matter.

**Fix:** `(uint64_t)endRecord->centralDirectoryOffset + endRecord->centralDirectorySize > (uint64_t)fileSize`. Also validate `centralDirectoryOffset + centralDirectorySize <= positionOf(endRecord)` so the directory cannot overlap the end record or extend into trailing junk.

### 1.6 [MEDIUM] No CRC32 verification

**Location:** `jzReadData()` — documented in junzip.h:137.

Corrupted or maliciously modified payload data is accepted silently. CRC32 is not a security primitive, but in the ZIP format it is the *only* integrity signal, and it would have caught findings 1.1/1.2 in most real-world corruptions. Note the project already ships a public-domain CRC32 implementation (in `jzip_demo.c`, Björn Samuelsson's) that could move into the library for non-zlib builds.

**Fix:** Compute CRC32 over the produced output (streaming as it is written) and fail extraction on mismatch. Make it opt-out rather than opt-in.

### 1.7 [LOW-MEDIUM] Central directory iteration not bounded by `centralDirectorySize`

**Location:** `jzReadCentralDirectory()` junzip.c:87-131.

The loop trusts `numEntries` and never checks that reads stay within `[centralDirectoryOffset, centralDirectoryOffset + centralDirectorySize)`. PoC claiming 100 entries with room for 1: the first entry was **extracted successfully**, then parsing ran off the end and errored. So a truncated/malicious archive yields partial extraction of an attacker-chosen prefix — extraction results and error reporting diverge from what the directory declares. Cheap sanity bound also available: `numEntries * sizeof(JZGlobalFileHeader) <= centralDirectorySize`.

**Fix:** Track position against the declared directory bounds before each header read; fail the whole operation (do not partially extract) on inconsistency.

### 1.8 [MEDIUM] Demo extractor: partial Zip Slip protection, no symlink defense, unbounded output

**Location:** `junzip_demo.c:33-74`.

What exists: rejection of leading `/`, any `../` substring, and on Windows leading `\`, `..\`, and drive letters. Verified against PoCs — all caught.

What is missing for hostile archives:

- **Symlink components.** If any path component of the extraction target is (or becomes) a symlink, writes escape the target tree. The demo never checks. `mkdir()` returning `EEXIST` is treated as success even when the existing object is a file or symlink. Robust extraction needs `open(..., O_NOFOLLOW | O_CREAT | O_EXCL)`-style handling or a `realpath()` prefix check against the extraction root before every write.
- **Component-wise validation.** The check is a deny-list substring match. Cleaner and safer: split the name on `/` (and `\` on Windows), reject empty components, `.`, `..`, NUL bytes (C-string truncation of a name whose ZIP length disagrees), and non-portable characters; reject Windows reserved device names (`CON`, `NUL`, `COM1`…) on that platform.
- **No resource budget.** File count, per-file size, and total extracted size are unbounded (see 1.4). Classic zip-bomb mitigation is a configurable budget.
- **Filenames are printed to the terminal unsanitized** (`junzip_demo.c:96`): control characters/escape sequences in a hostile name can forge log output. Print escaped or quoted names.
- `writeFile` ignores `fwrite` short writes ("best effort") — disk-full becomes a silently truncated output file. At minimum warn; for a security posture, fail.

**Note:** the demo also writes with default permissions and does not honor archive-stored modes — that is the *safe* direction (no setuid/device-node creation is possible). Keep it that way; document it as intentional.

### 1.9 [MEDIUM, API] Library is not thread-safe or reentrant

**Location:** `jzBuffer` global (junzip.c:12); `puff.c` `fixed()` static `virgin` flag and static tables (puff.c:538-541).

The callback filename pointer aliases the global buffer, and `fixed()` lazily initializes shared statics without synchronization — two threads inflating concurrently race. This is documented in a code comment but not in the public header, where a "major dependency" consumer would look.

**Fix:** Either (a) make state explicit — a `JZContext`/per-call buffer passed through the API, and compile-time-initialized tables in puff — or (b) state the limitation prominently in `junzip.h` and README. (a) is the real fix; the library is small enough that threading the state through is a modest refactor.

---

## 2. Lower-severity / correctness issues with security-adjacent impact

### 2.1 Valid empty archives are rejected

`jzReadEndRecord` (junzip.c:24) uses `fileSize <= sizeof(JZEndRecord)`, rejecting the perfectly valid 22-byte empty zip. Should be `<`. Availability/correctness.

### 2.2 End-record scan window too small for maximum-length comments

The scan reads the last `min(fileSize, 65536)` bytes, but a maximal comment (65535) plus the 22-byte record needs 65557. Valid archives with comments longer than 65,514 bytes are rejected (confirmed by PoC). Grow the buffer to 65557+ or scan the tail in two chunks.

### 2.3 Zero-as-absent merge heuristic corrupts legitimate values

`jzReadLocalFileHeader` (junzip.c:192-197) treats a zero local CRC/size as "absent, keep central value". An empty stored file legitimately has `crc32 = 0`, so the heuristic conflates "absent" with "zero". The correct discriminator is bit 3 of `generalPurposeBitFlag`, not zero-ness. Replace the heuristic with: if bit 3 set → keep central values; else → require local == central (see 1.3).

### 2.4 Encrypted entries are not rejected explicitly

Bit 0 of `generalPurposeBitFlag` (encryption) is never checked; comment at junzip.c:166-168 says flags are ignored "and hope ZLIB can uncompress". Encrypted data fed to inflate produces garbage-or-error depending on chance — fail fast instead. Similarly, reject `versionNeededToExtract` values above what the library actually implements.

### 2.5 Store path ignores `compressedSize`

The store branch (junzip.c:242) reads `uncompressedSize` bytes and never checks `compressedSize == uncompressedSize` when sizes came from the central directory (the raw-header check at junzip.c:170-172 only sees local values, and is skipped when both are zero under bit 3). A mismatch means the reader consumes bytes belonging to subsequent structures — the extracted file then contains raw archive bytes. Enforce equality for method 0 after descriptor parsing too.

### 2.6 Portability/robustness nits

- `(JZEndRecord *)(jzBuffer + i)` (junzip.c:46) relies on `__attribute__((packed))` semantics for unaligned access — fine on GCC/Clang, but the codebase already knows it is compiler-sensitive (Makefile has `-mno-ms-bitfields` for MinGW). A `memcmp`+field-by-field parse would be bulletproof.
- `JZFile.tell` returns `size_t` but stdio `ftell` returns `long`; an error (-1) becomes a huge `size_t`. Current callers survive via the `<= sizeof(...)` comparison, but a `long`/`int64_t`-based API with explicit error signaling would be safer.
- 32-bit platforms: `long` file positions cap archives at 2 GB; combined with no ZIP64, large archives are out of scope and should be rejected explicitly rather than failing coincidentally.
- `printf`/`fprintf` use `%d` for `uint32_t` sizes (demo prints `-1` bytes for the 4 GB PoC) — cosmetic, but confusing during incident analysis.
- `junzip_dump.c` duplicates parsing logic instead of using the library (it has its own 640-byte filename buffer and checks). Duplicated parsers drift; prefer one hardened path. It also returns `Z_ERRNO` (-1) from `main`.
- `jzip_demo.c` (writer): `stat()` return unchecked before `localtime(&st.st_mtime)`; TOCTOU between the CRC pass and the copy pass can produce archives whose headers disagree with their payload; `fwrite` returns unchecked; archived names embed the full input path. The writer only runs on trusted local files, so severity is low — but a malformed-writer is how you accidentally ship broken zips.

---

## 3. What is already solid

- Header-parse reads are size-checked; no attacker-length `memcpy` exists anywhere in the library.
- Filename lengths are bounded before copy in both the library (`JZ_BUFFER_SIZE` check, junzip.c:99) and the dump tool (junzip_dump.c:59).
- Unsupported compression methods are rejected, not attempted (junzip.c:313-315).
- The over-expansion case (deflate stream larger than claimed) *is* caught on both backends (PoC-verified: inflate returns non-`Z_STREAM_END`, puff returns 1) — output writes are correctly bounded by the claimed size; the gap is only the under-production case (1.2).
- `puff.c` itself is mature reference code (Mark Adler, v2.3): input exhaustion handled via `longjmp`, all copies bounded by `outlen`, invalid distances rejected. No issues found. Just never compile it with `INFLATE_ALLOW_INVALID_DISTANCE_TOOFAR_ARRR` for ZIP use.
- The short-fuzz campaign (400 iterations of bit-flips and truncations on a data-descriptor archive, ASan+UBSan) found exactly one memory-safety bug class (1.1) and nothing in header parsing — the parsing skeleton is a good base to harden.
- The demo refuses to honor archived Unix permission bits — no setuid/device-file creation vector.

---

## 4. Remediation plan — what "major software dependency" hardening takes

Ordered by priority; each item is small. Items R1-R4 together close every confirmed memory-safety/integrity hole.

- **R1 (API break, fixes 1.3).** Change `jzReadData` to take the caller's buffer length and return the actual bytes produced: `int jzReadData(JZFile *zip, JZFileHeader *header, void *buffer, size_t bufferSize, uint32_t *bytesProduced)`. Fail when the claimed size exceeds the buffer.
- **R2 (fixes 1.2).** Require exact production: store → `read == uncompressedSize`; zlib → `total_out == uncompressedSize`; puff → `destlen == uncompressedSize`. Errors otherwise.
- **R3 (fixes 1.1, 2.3, 2.5).** Treat local header and data descriptor as *verification*, not *truth*: when bit 3 is clear, require local == central for method/sizes/CRC; when set, verify descriptor values equal the central sizes and the actual counts consumed/produced. Never mutate the caller's size fields post-read. Enforce `compressedSize == uncompressedSize` for method 0.
- **R4 (fixes 1.4).** Reject ZIP64 sentinels (`0xFFFFFFFF`, ambiguous `0xFFFF`) with a distinct "unsupported" error until/unless ZIP64 is implemented.
- **R5 (fixes 1.5, 1.7).** Do all offset arithmetic in `uint64_t`; bound central-directory iteration by `centralDirectorySize`; require the directory to fit before the end record; sanity-check `numEntries * 46 <= centralDirectorySize`.
- **R6 (fixes 1.6).** Verify CRC32 of extracted output by default; move the public-domain CRC32 from `jzip_demo.c` into the library for non-zlib builds.
- **R7 (fixes 1.8).** Harden the demo extractor: component-wise path validation, `O_NOFOLLOW`-style symlink defense or `realpath` root-prefix check, per-file/total extraction budgets, fail on short `fwrite`, escape filenames in logs. Keep ignoring archived permission bits.
- **R8 (fixes 2.4).** Reject encrypted entries (flag bit 0) and `versionNeededToExtract` above the implemented level, explicitly.
- **R9 (fixes 1.9).** Decide the thread-safety story: either a context struct threaded through the API (removes `jzBuffer`) plus compile-time puff tables, or loud documentation in `junzip.h`. For "major dependency" status, do the refactor.
- **R10 (fixes 2.1, 2.2).** Accept 22-byte empty archives; enlarge the end-record scan to cover a maximal comment (65557 bytes).
- **R11 (process).** Make the verification loop that produced this report permanent: CI builds with `-fsanitize=address,undefined -fno-sanitize-recover=all` for both backends, `-Wall -Wextra -Wconversion -Wsign-compare` clean, the PoC corpus as regression tests, and a libFuzzer/AFL++ harness (`LLVMFuzzerTestOneInput` wrapping `jzReadEndRecord`/`jzReadCentralDirectory`/`jzReadData` over an in-memory `JZFile`). The 128-iteration time-to-discovery says fuzzing pays for itself here. Add `-fstack-protector-strong` and `_FORTIFY_SOURCE=2` to release flags.
- **R12 (API hygiene).** Document every precondition in `junzip.h` (buffer sizing, callback lifetime, single-threadedness until R9, "verify CRC" until R6); give the library its own error-code enum instead of borrowing zlib's; consider `int64_t` file positions.

### Suggested acceptance criteria

1. The full PoC corpus from this analysis is rejected with errors (or extracted byte-exactly, for the valid cases) under ASan/UBSan.
2. 24 hours of libFuzzer on an in-memory harness, both backends, zero sanitizer findings.
3. No code path where a size read from the archive influences a write without first being validated against a caller-supplied buffer bound.
4. CRC mismatch on any modified payload byte → extraction error, test enforced.

---

## Appendix: PoC index

All PoCs are minimal hand-built archives (generator script available on request); each was run against ASan/UBSan builds of the zlib and puff variants:

| PoC | Trigger | Result |
|-----|---------|--------|
| Claimed `uncompressedSize` 1000, real 32 | 1.2 | Exit 0; 968 uninitialized heap bytes written to file |
| Descriptor lies (`64` → `0x100000`) | 1.1 | 245,760-byte file from 64-byte buffer; ASan heap-buffer-overflow via fuzz variant |
| Central 32 vs. local 5000 (store) | 1.3 | Merge confirmed; overflow pattern in buffer-from-central callers |
| `uncompressedSize = 0xFFFFFFFF` | 1.4 + 1.2 | 4 GB output file from ~500-byte archive |
| `cdOffset + cdSize` wrap | 1.5 | Validation bypassed; graceful failure afterward |
| `numEntries = 100`, 1 real entry | 1.7 | Partial extraction of entry 0, then error |
| `../`, absolute, `..\\`, mixed traversal names | 1.8 | All rejected on POSIX build |
| 22-byte empty zip | 2.1 | Rejected (spec-valid file) |
| 65,535-byte comment | 2.2 | End record not found (spec-valid file) |
| Over-expanding deflate stream | control | Correctly rejected on both backends |
| 400-iteration bit-flip/truncate fuzz | all | 1 ASan finding (→ 1.1), zero crashes elsewhere |
