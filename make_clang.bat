@SET CC=clang

@REM This is an example on compiling with Clang under Windows.
@REM If you are using Linux, you SHOULD know enough to adapt. :)

@REM Uncomment one of the below options if you want (de)compression support
@REM SET LIB=-DHAVE_PUFF puff.c
@REM SET LIB=-DHAVE_ZLIB -I..\zlib -L..\zlib -lz THIS DOESN'T PROBABLY WORK

%CC% %LIB% junzip.c junzip_demo.c -o junzip_demo.exe
%CC% %LIB% junzip.c junzip_test.c -o junzip_test.exe
%CC% %LIB% junzip.c junzip_dump.c -o junzip_dump.exe
%CC% %LIB% junzip.c jzip_demo.c -o jzip_demo.exe
