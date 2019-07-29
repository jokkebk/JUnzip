// Basic tests for JUnzip library. Needs zlib. Should compile with
// something like gcc junzip_test.c junzip.c -lz -o junzip_test.exe

#include <stdio.h>
#include <assert.h>

#include "junzip.h"

int recordCallback(JZFile *zip, int idx, JZFileHeader *header, char *filename, void *user_data) {
    printf("[%08X] Global file header for \"%s\" (%d compressed bytes)\n",
            (int)zip->tell(zip), filename, header->compressedSize);
}

int main(int argc, char *argv[]) {
    FILE *fp;
    JZFile *zip;
    JZFileHeader jzHeader;
    JZEndRecord jzEndRecord;
    char filename[640]; // should be enough for everyone
    long fpos, fsize;

    if(argc < 2) {
        puts("Usage: junzip_dump file.zip");
        return 0;
    }

    if(!(fp = fopen(argv[1], "rb"))) {
        printf("Couldn't open \"%s\"!", argv[1]);
        return -1;
    }

    zip = jzfile_from_stdio_file(fp);

    puts("Reading files...");
    jzEndRecord.numEntries = 0; // use a fake end record to count files

    do {
        // Central dir should be where first local header read fails
        jzEndRecord.centralDirectoryOffset = zip->tell(zip); 

        if(jzReadLocalFileHeader(zip, &jzHeader, filename, sizeof(filename))) {
            puts("Read of local file header failed, assuming file data ended.");
            break; // next step after the loop will seek back to pos
        }

        printf("[%08X] File \"%s\" found, %d bytes (%d compressed), skipping data...\n",
                (int)jzEndRecord.centralDirectoryOffset, filename,
                jzHeader.uncompressedSize, jzHeader.compressedSize);
        zip->seek(zip, jzHeader.compressedSize, SEEK_CUR); // skip data
        jzEndRecord.numEntries++; // count files
    } while(1);

    jzReadCentralDirectory(zip, &jzEndRecord, recordCallback, zip);

    fpos = zip->tell(zip);
    zip->seek(zip, 0, SEEK_END);
    fsize = zip->tell(zip);

    if(fsize - fpos < sizeof(JZEndRecord)) {
        printf("[%08X] Only %ld bytes remain, end record is 22 bytes, aborting!\n", (int)fpos, fsize - fpos);
        return 0;
    }

    // We could use jzReadEndRecord but let's just be blindly optimistic here
    zip->seek(zip, fpos, SEEK_SET);
    zip->read(zip, &jzEndRecord, sizeof(JZEndRecord));
    printf("[%08X] End record read, signature %08X\n", (int)fpos, jzEndRecord.signature);

    return 0;
}
