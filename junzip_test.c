// Basic tests for JUnzip library. Needs zlib. Should compile with
// something like gcc junzip_test.c junzip.c -lz -o junzip_test.exe

#include <stdio.h>
#include <assert.h>

#include <zlib.h>

#include "junzip.h"

int main() {
    printf("Verifying JZLocalFileHeader (%d bytes)\n",
            sizeof(JZLocalFileHeader));
    assert(sizeof(JZLocalFileHeader) == 30);

    printf("Verifying JZGlobalFileHeader (%d bytes)\n",
            sizeof(JZGlobalFileHeader));
    assert(sizeof(JZGlobalFileHeader) == 46);

    printf("Verifying JZFileHeader (%d bytes)\n",
            sizeof(JZFileHeader));
    assert(sizeof(JZFileHeader) == 22);

    printf("Verifying JZEndRecord (%d bytes)\n",
            sizeof(JZEndRecord));
    assert(sizeof(JZEndRecord) == 22);

    return 0;
}
