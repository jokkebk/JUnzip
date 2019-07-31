/* Simple "zip" utility WITHOUT COMPRESSIONS (stores files).
 * Should compile with something like gcc jzip_store.c junzip.c -o junzip.exe
 * CRC32 routine is from Björn Samuelsson's public domain implementation at
 * http://home.thep.lu.se/~bjorn/crc/ 
 */

#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "junzip.h"

uint32_t crc32_for_byte(uint32_t r) {
    for(int j = 0; j < 8; ++j)
        r = (r & 1? 0: (uint32_t)0xEDB88320L) ^ r >> 1;
    return r ^ (uint32_t)0xFF000000L;
}

void crc32(const void *data, size_t n_bytes, uint32_t* crc) {
    static uint32_t table[256];
    if(!*table) for(size_t i = 0; i < 0x100; ++i) table[i] = crc32_for_byte(i);
    for(size_t i = 0; i < n_bytes; ++i)
        *crc = table[(uint8_t)*crc ^ ((uint8_t*)data)[i]] ^ *crc >> 8;
}

int main(int argc, char *argv[]) {
    FILE *in, *out;
    char buf[1<<15];
    int files = 0, i; // actual files and argv indices of them
    char **filenames;

    if(argc < 3) {
        puts("Usage: jzip_store file1 [file2 file3 ...] output.zip");
        return 0;
    }

    out = fopen(argv[argc-1], "wb");

    JZGlobalFileHeader *globalHeader = (JZGlobalFileHeader *)calloc((argc-2),
            sizeof(JZGlobalFileHeader));
    JZEndRecord jzEndRecord;

    if(globalHeader == NULL) {
        puts("Failed to allocate buffers to headers!");
        return 0;
    }

    if((filenames = malloc(sizeof(char *) * (argc-2))) == NULL) {
        puts("Failed to allocate pointers to filenames!");
        return 0;
    }

    for(int i = 0; i < argc-2; i++) {
        JZLocalFileHeader header;
        struct stat st;
        struct tm * timeinfo;

        filenames[files] = argv[i+1]; // needed due to possible skipped arguments
        if((in = fopen(filenames[files], "rb")) == NULL) {
            printf("File \"%s\" not found, just skipping...\n", filenames[files]);
            continue;
        }

        while(!feof(in) && !ferror(in))
            crc32(buf, fread(buf, 1, sizeof(buf), in), &header.crc32);
        if(ferror(in)) {
            puts("Error while calculating CRC, skipping store.");
        } else {
            JZGlobalFileHeader *gf = globalHeader + files;

            stat(filenames[files], &st);
            timeinfo = localtime(&st.st_mtime);

            header.signature = 0x04034B50;
            gf->signature = 0x02014B50;
            gf->versionMadeBy = 10; // random stuff
            gf->versionNeededToExtract = header.versionNeededToExtract = 10;
            gf->generalPurposeBitFlag = header.generalPurposeBitFlag = 0;
            gf->compressionMethod = header.compressionMethod = 0;
            gf->lastModFileTime = header.lastModFileTime = JZTIME(
                    timeinfo->tm_hour,
                    timeinfo->tm_min,
                    timeinfo->tm_sec);
            gf->lastModFileDate = header.lastModFileDate = JZDATE(
                    timeinfo->tm_year+1900,
                    timeinfo->tm_mon+1,
                    timeinfo->tm_mday);
            gf->crc32 = header.crc32;
            gf->compressedSize = header.compressedSize =
                gf->uncompressedSize = header.uncompressedSize = ftell(in);
            gf->fileNameLength = header.fileNameLength = strlen(filenames[files]);
            gf->extraFieldLength = header.extraFieldLength = 0;
            gf->fileCommentLength = 0;
            gf->diskNumberStart = 0;
            gf->internalFileAttributes = 0;
            gf->externalFileAttributes = 0x20; // whatever this is
            gf->relativeOffsetOflocalHeader = ftell(out);

            printf("[%08X] ", (int)ftell(out));
            printf("Writing local header for %s\n", filenames[files]);
            fwrite(&header, 1, sizeof(header), out);
            printf("[%08X] ", (int)ftell(out));
            printf("Writing filename\n");
            fwrite(filenames[files], 1, strlen(filenames[files]), out);

            fseek(in, 0, SEEK_SET); // rewind
            while(!feof(in) && !ferror(in)) {
                size_t bytes = fread(buf, 1, sizeof(buf), in);
                fwrite(buf, 1, bytes, out);
            }

            files++;
        }

        fclose(in);
    }

    jzEndRecord.signature = 0x06054b50;
    jzEndRecord.diskNumber = 0;
    jzEndRecord.centralDirectoryDiskNumber = 0;
    jzEndRecord.numEntriesThisDisk = files;
    jzEndRecord.numEntries = files;
    jzEndRecord.centralDirectoryOffset = ftell(out);
    for(i = 0; i < files; i++) {
        printf("[%08X] ", (int)ftell(out));
        printf("Writing global header for %s (%d + %d bytes)...\n", filenames[i],
                (int)sizeof(JZGlobalFileHeader), (int)strlen(filenames[i]));
        fwrite(globalHeader + i, 1, sizeof(JZGlobalFileHeader), out);
        fwrite(filenames[i], 1, strlen(filenames[i]), out);
    }
    jzEndRecord.centralDirectorySize = ftell(out) - jzEndRecord.centralDirectoryOffset;
    jzEndRecord.zipCommentLength = 0;

    printf("[%08X] ", (int)ftell(out));
    puts("Writing end record.");
    fwrite(&jzEndRecord, 1, sizeof(jzEndRecord), out);

    fclose(out);
    free(globalHeader);

    return 0;
}
