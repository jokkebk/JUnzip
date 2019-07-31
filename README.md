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
gcc -DHAVE_ZLIB junzip_demo.c junzip.c -lz -o junzip
```

You can also compile a "no compression" version without `-DHAVE_ZLIB` and
`-lz`, and use `clang` as well:

```
clang jzip_store.c junzip.c -o jzip_store
```

On Windows, MinGW requires `-mno-ms-bitfields` compilation flags, so you
may want to use the supplied Makefile and run `make` instead
(`make HAVE_ZLIB=1` if you want compression support). A
small `make_clang.bat` is also provided just for laughs.
Included executables:

* `junzip_demo file.zip` unzips the `file.zip`
* `junzip_dump file.zip` dumps the headers in `file.zip`
* `junzip_test` checks if structures are the right size
* `jzip_store file1 file2 output.zip` creates a zip file (will silently
overwrite last parameter so be VERY careful!)



Github: https://github.com/jokkebk/JUnzip
