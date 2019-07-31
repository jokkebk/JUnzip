JUnzip
======

Minimalistic unzip library released into public domain. There is now
also simple demo for creating a zip file without compression.

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
gcc -DHAVE_PUFF junzip_demo.c junzip.c puff.c -lz -o junzip_demo
```

You can also compile a "no compression" version without either above
dependencies and use `clang` as well as `gcc`:

```
clang jzip_store.c junzip.c -o jzip_store
```

# Makefile

On Windows, MinGW requires `-mno-ms-bitfields` compilation flags, so you
may want to use the supplied Makefile and build the `junzip_demo` and other
utilities in one go:

```
make
```

You can make the zlib-enabled versions easily as well to get fast
decompression:

```
make HAVE_ZLIB=1
```

Or the lighter but bit slower `puff.c` powered decompression without the
zlib dependency:
decompression

```
make HAVE_PUFF=1
```

A small `make_clang.bat` is also provided just for laughs.

# Included executables

* `junzip_demo file.zip` unzips the `file.zip`
* `junzip_dump file.zip` dumps the headers in `file.zip`
* `junzip_test` checks if structures are the right size
* `jzip_store file1 file2 output.zip` creates a zip file (will silently
overwrite last parameter so be VERY careful!)

Github: https://github.com/jokkebk/JUnzip
