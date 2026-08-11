# Security Review

Date: 2026-08-11
Reviewed tree: reconciled local `master`, combining `e6c7509` with remote PR #7
(`c31243a`, `8c5b6d4`, merge `a5667b6`)
Scope: ZIP parsing and extraction APIs, Store/Puff/zlib data paths, reference
extractor and writer, regression/fuzz tooling, and the security-relevant Git
history.

## Verdict

The earlier critical and high-risk defects are fixed in the reconciled tree. I
found no remaining confirmed critical/high memory-safety or archive-integrity
vulnerability in the reviewed classic-ZIP paths.

The core is now a defensible bounded decoder for untrusted classic, single-disk
ZIP input when callers honor its documented resource contract. The POSIX demo
is also a reasonable reference extractor when its destination is privately
owned and cannot be modified concurrently. This is not a claim that arbitrary
ZIP features are supported, nor that extraction is transactional.

The main residual risk is filesystem concurrency: neither platform promises
containment if an attacker can rename or replace destination components while
extraction is in progress. The Windows implementation has additional path
normalization/device-name concerns and has not received native Windows dynamic
testing. Those constraints are now documented in `README.md`.

## Git reconciliation

Before reconciliation, local `master` and `origin/master` had both advanced
from `6b357b2`:

- Local `e6c7509` contained the stronger destination-rooted extractor,
  central/local two-pass validation, extraction budgets, bounded input API,
  and 64-bit file-offset interface.
- Remote PR #7 added the corrected raw-Deflate writer, permanent fuzz harness,
  sanitizer/fuzz Makefile targets, test exit-status fix, and historical
  `SecurityAnalysis.md`.

Both lines contain relevant work; neither was discarded. The reconciled tree
retains the stronger local implementations where the changes overlap and ports
the remote checks and tooling to the 64-bit API. The deleted remote feature
branch was pruned, and `master` is the only local branch.

## Residual findings

### SR-01 — Medium — Concurrent destination mutation can defeat containment

The POSIX extractor resolves components with directory-relative
`openat()`/`mkdirat()`, `O_NOFOLLOW`, `O_DIRECTORY`, and `O_EXCL`. This blocks
the reproduced pre-existing-symlink escape and prevents overwriting existing
targets. However, an attacker who can rename a directory after it has been
opened can relocate that directory outside the extraction root while the held
descriptor remains valid. A later descriptor-relative create can then appear
outside the intended pathname.

Windows checks each component for reparse points before later opening a child.
Those separate operations have a more direct check/use race. Windows path
normalization also gives special meaning to reserved device names and trailing
dots/spaces; the current lexical policy does not model every such alias.

Required operating rule: extract only into an existing destination controlled
exclusively by the extraction process. If hostile concurrent writers are in
scope, platform-specific stronger containment or process isolation is needed;
ordinary path checks are insufficient.

### SR-02 — Low — Resource safety remains partly caller policy

`jzReadDataLimited()` enforces both the caller's output buffer and compressed
input limit before decoding. The reference demo additionally caps entries at
10,000, each compressed/uncompressed entry at 256 MiB, and aggregate output at
1 GiB.

The convenience `jzReadData()` deliberately permits the full classic-ZIP
compressed-size range, and the API decodes whole entries rather than streaming.
A caller that allocates directly from archive metadata or chooses unsuitable
limits can still suffer memory, CPU, or disk denial of service. Service-facing
integrations should use `jzReadDataLimited()`, lower context-specific budgets,
and external time/process limits when necessary.

### SR-03 — Low — Failed extraction can leave a partial output tree

The demo validates all central/local headers before writing, stops on the first
extraction or output error, returns failure, checks writes/closes, and never
overwrites an existing target. It does not roll back files already created if a
later runtime error, duplicate name, file/directory conflict, or disk failure
occurs.

This is safe fail-closed behavior for callers that honor the exit status, but it
is not all-or-nothing extraction. Applications requiring transactional results
should extract into a fresh private staging directory and publish it only after
success.

### SR-04 — Informational — Deliberate format and compatibility limits

The parser rejects ZIP64, multi-disk archives, encryption, unknown methods and
unsupported flags. It supports Store and Deflate only. It also rejects trailing
central-directory records it does not understand. These choices reduce attack
surface but may reject otherwise valid archives.

A signatureless data descriptor whose CRC equals the optional descriptor
signature `0x08074b50` is interpreted as signed and rejected. This is a rare
valid-input compatibility edge, not a demonstrated security bypass.

## Previously reported issues now resolved

- Caller-sized output bounds, exact compressed/uncompressed consumption, CRC32,
  and data-descriptor verification prevent the prior over-read, overflow and
  uninitialized-output classes.
- Central and local filenames, versions, flags, methods, CRCs and sizes are
  reconciled before extraction; central-directory bounds remain authoritative.
- ZIP64 sentinels, multi-disk fields, encryption, unsupported flags/methods,
  embedded NULs, central-directory overruns and metadata-overlapping local
  offsets are rejected.
- Parser arithmetic and stdio seeking use checked 64-bit offsets.
- Shared mutable decompression state was removed.
- The reference extractor validates before output, enforces resource budgets,
  anchors POSIX traversal at a destination descriptor, refuses existing files,
  checks output errors, and returns failure on any entry error.
- `jzip_demo` now writes method 8 with raw Deflate and checks relevant I/O and
  classic-ZIP size limits.
- Store-only, Puff and zlib expectations are separated so every advertised test
  configuration passes.

## Verification performed on the reconciled tree

- `make check`: 88 checks, 0 failed (Store-only).
- `make check-puff`: 91 checks, 0 failed.
- `make check-zlib`: 91 checks, 0 failed.
- `make sanitize`: Puff and zlib suites both completed with ASan/UBSan and 91
  checks, 0 failed.
- Standalone fuzz smoke: 400 deterministic bit-flip/truncation cases under
  ASan/UBSan, no sanitizer finding.
- Clang static analysis of the core, demos, writer, Puff, and fuzz harness.
- A zlib-created archive from `jzip_demo` passed the system `unzip -t` validator
  and extracted byte-for-byte through `junzip_demo`.
- A fresh pre-existing-symlink escape regression was rejected with nonzero exit
  and did not create the file outside the destination.

The installed Apple Clang does not provide the libFuzzer runtime, so a sustained
coverage-guided fuzz campaign was not run. Windows behavior was reviewed from
source but not dynamically tested on Windows. These are confidence gaps, not
evidence of a vulnerability.

## Recommendation

The reconciled core and POSIX reference extractor are acceptable for untrusted
classic ZIPs within the documented limits and private-destination threat model.
Keep the sanitizer matrix and fuzz harness in CI, add native Windows path tests,
and consider a streaming API if large untrusted entries are a real use case.
