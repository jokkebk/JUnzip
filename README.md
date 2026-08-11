JUnzip
======

Minimalistic zip/unzip library released into public domain. There is now
also simple demo for creating a zip file with or without compression!

Written by Joonas Pihlajamaa (firstname.lastname@iki.fi). 
For details about this project, see:

http://codeandlife.com/2014/01/01/unzip-library-for-c/

# Compiling

The codebase is very lean and should be easy to understand, and port to
any system. Compiling the example on most systems with zlib installed:

```
gcc -DHAVE_ZLIB junzip_demo.c junzip.c -lz -o junzip_demo
```

If you don't want the zlib dependency but need decompression, `puff.c` by
Mark Adler (part of zlib contrib suite) is included and you can include it
instead of zlib:

```
gcc -DHAVE_PUFF junzip_demo.c junzip.c puff.c -o junzip_demo
```

You can also compile a "no compression" version without either above
dependencies and use `clang` as well as `gcc`:

```
clang junzip_demo.c junzip.c -o junzip_demo
```

# Makefile

On Windows, MinGW requires `-mno-ms-bitfields` compilation flags, so you
may want to use the supplied Makefile and build the `junzip_demo` and other
utilities in one go:

```
make
make check
```

You can make the zlib-enabled versions easily as well to get fast
decompression and compression support to `jzip_demo` (not just store):

```
make clean
make HAVE_ZLIB=1
make HAVE_ZLIB=1 check
```

Or the lighter but bit slower `puff.c` powered decompression without the
zlib dependency (and no compression, just store in `jzip_demo`):

```
make clean
make HAVE_PUFF=1
make HAVE_PUFF=1 check
```

The security suite is also available under both sanitizers:

```
make sanitize
```

`make sanitize` builds and runs the suite once with puff and once with zlib.
The Makefile uses the system zlib (`-lz`); clean between backend changes so
objects compiled with different feature defines are not mixed.

On systems with the libFuzzer runtime, `make fuzz` builds the optional
libFuzzer harness. `make fuzz-smoke` builds an ASan/UBSan standalone harness;
run it with a ZIP seed, for example `./junzip_fuzz_smoke sample.zip`, for a
400-iteration mutation/truncation smoke run.

A small `make_clang.bat` is also provided just for laughs.

# Included executables

`jzip_demo` overwrites its output archive, and `junzip_demo` overwrites
existing regular files below the current directory. The extractor refuses
absolute/traversal/control-character names, embedded NULs, symlinked path
components, ZIP64/encrypted entries, and entries exceeding its 256 MiB per-file
or 1 GiB total extraction budget. It ignores archived permission bits and
creates ordinary files with safe default permissions.

* `junzip_demo file.zip` unzips the `file.zip`
* `jzip_demo file1 file2 output.zip` creates a zip file
* `junzip_dump file.zip` dumps the headers in `file.zip`
* `junzip_test` checks if structures are the right size

Note: ZIP entries that use the data descriptor flag (general purpose bit 3)
are supported when sizes are known (e.g., from the central directory). The
library keeps central-directory sizes, consumes and verifies the descriptor,
checks exact compressed/decompressed counts, and verifies CRC32 before
returning success. ZIP64 is intentionally unsupported.

Github: https://github.com/jokkebk/JUnzip
