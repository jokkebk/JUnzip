@echo off

rem This is an example on compiling with Clang under Windows.
rem If you are using Linux, you SHOULD know enough to adapt. :)

rem Remove "-DNOZLIB" from lines below if you have zlib installed, I did not.

clang -DNOZLIB junzip.c junzip_demo.c -o junzip_demo.exe
clang -DNOZLIB junzip.c junzip_test.c -o junzip_test.exe
