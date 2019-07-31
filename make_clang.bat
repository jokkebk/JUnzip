@echo off

rem This is an example on compiling with Clang under Windows.
rem If you are using Linux, you SHOULD know enough to adapt. :)

rem For compression support, you can add "-DHAVE_ZLIB -lz" to lines below.

clang junzip.c junzip_demo.c -o junzip_demo.exe
clang junzip.c junzip_test.c -o junzip_test.exe
clang junzip.c junzip_dump.c -o junzip_dump.exe
clang junzip.c jzip_store.c -o jzip_store.exe
