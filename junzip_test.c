// Basic tests for JUnzip library. Needs zlib. Should compile with
// something like gcc junzip_test.c junzip.c -lz -o junzip_test.exe

#include <stdio.h>
#include <assert.h>

#include "junzip.h"

int main() {
    int ok = 1;
    printf("Verifying that all structs are correct size...\n");

    printf("JZLocalFileHeader: %d vs. 30: ", (int)sizeof(JZLocalFileHeader));
    if(sizeof(JZLocalFileHeader) == 30) puts("OK");
    else { puts("FAILED!"); ok = 0; }

    printf("JZGlobalFileHeader: %d vs. 46: ", (int)sizeof(JZGlobalFileHeader));
    if(sizeof(JZGlobalFileHeader) == 46) puts("OK");
    else { puts("FAILED!"); ok = 0; }

    printf("JZFileHeader: %d vs. 26: ", (int)sizeof(JZFileHeader));
    if(sizeof(JZFileHeader) == 26) puts("OK");
    else { puts("FAILED!"); ok = 0; }

    printf("JZEndRecord: %d vs. 22: ", (int)sizeof(JZEndRecord));
    if(sizeof(JZEndRecord) == 22) puts("OK");
    else { puts("FAILED!"); ok = 0; }

    return ok ? 0 : 1;
}
