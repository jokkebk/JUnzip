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

*Warning*, both the `jzip_demo` and `junzip_demo` WILL overwrite files silently!
Zip demo assumes last parameter is the target and overwrites that file, and
unzip demo overwrites any files already within the directory with contents of the zip. Be careful!

* `junzip_demo file.zip` unzips the `file.zip`
* `jzip_demo file1 file2 output.zip` creates a zip file
* `junzip_dump file.zip` dumps the headers in `file.zip`
* `junzip_test` checks if structures are the right size

Github: https://github.com/jokkebk/JUnzip
