JUnzip
======

Minimalistic unzip library released into public domain. There is now
also simple demo for creating a zip file without compression.

Written by Joonas Pihlajamaa (firstname.lastname@iki.fi). 
For details about this project, see:

http://codeandlife.com/2014/01/01/unzip-library-for-c/

The codebase is very lean and should be easy to understand, and port to
any system. Compiling the example on most systems with zlib installed:

```
gcc junzip_demo.c junzip.c -lz -o junzip
```

You can also compile a "no compression" version with `-DNOZLIB`, or use `clang`:

```
clang -DNOZLIB jzip_store.c junzip.c -o jzip_store
```

On Windows, MinGW requires `-mno-ms-bitfields` compilation flags, so you
may want to use the supplied Makefile and run `make` instead. A simple
test suite is also available with `make test` (checks struct sizes).
A small `make_clang.bat` is also provided just for laughs.

Github: https://github.com/jokkebk/JUnzip
