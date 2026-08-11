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
```

You can make the zlib-enabled versions easily as well to get fast
decompression and compression support to `jzip_demo` (not just store):

```
make HAVE_ZLIB=1
```

Or the lighter but bit slower `puff.c` powered decompression without the
zlib dependency (and no compression, just store in `jzip_demo`):

```
make HAVE_PUFF=1
```

A small `make_clang.bat` is also provided just for laughs.

# Included executables

*Warning*: `jzip_demo` overwrites its output file. The `junzip_demo` refuses
existing output files, symlinked path components, and reparse-point paths; it
never follows them outside the destination directory. Choose a destination
directory with appropriate ownership and permissions.

* `junzip_demo file.zip destination-directory` extracts `file.zip`
* `jzip_demo file1 file2 output.zip` creates a zip file
* `junzip_dump file.zip` dumps the headers in `file.zip`
* `junzip_test` checks if structures are the right size

Note: ZIP entries that use the data descriptor flag (general purpose bit 3)
are supported when sizes are known (e.g., from the central directory). The
demo keeps central-directory sizes and `jzReadData` consumes the descriptor
automatically so the file pointer lands on the next entry. The reference demo
validates the full central directory and all local headers before writing, and
enforces limits of 10,000 entries, 256 MiB compressed or uncompressed per
entry, and 1 GiB total uncompressed output. The whole-entry `jzReadData` API
still requires callers to apply their own equivalent resource limits;
`jzReadDataLimited` can enforce the compressed-size limit before decoding.

The POSIX demo uses `openat()`/`mkdirat()` semantics. The Windows demo also
runs, but uses a conservative policy that rejects reparse-point components and
existing targets because the Win32 API lacks a direct `openat()` equivalent.

Github: https://github.com/jokkebk/JUnzip
