// Simple demo for JUnzip library. Needs zlib. Should compile with
// something like gcc junzip_demo.c junzip.c -lz -o junzip.exe

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "junzip.h"

#include <errno.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(d) _mkdir(d)
#else
#include <sys/stat.h>
#include <sys/types.h>
#define MKDIR(d) mkdir((d), 0755)
#endif

// Create the directory if necessary; return 1 on success/existing
int makeDirectory(char *dir) {
    if(MKDIR(dir) == 0)
        return 1; // created ok

    if(errno == EEXIST)
        return 1; // already there

    fprintf(stderr, "Couldn't create %s (%d)\n", dir, errno);
    return 0;
}

void writeFile(char *filename, void *data, long bytes) {
    FILE *out;
    int i;

    // Reject unsafe filenames (Zip Slip protection)
    if(filename[0] == '/' || strstr(filename, "../")
#ifdef _WIN32
            || filename[0] == '\\' || strstr(filename, "..\\")
            || (filename[0] && filename[1] == ':')
#endif
            ) {
        fprintf(stderr, "Skipping unsafe filename: %s\n", filename);
        return;
    }

    // simplistic directory creation support
    for(i=0; filename[i]; i++) {
        if(filename[i] != '/')
            continue;

        filename[i] = '\0'; // terminate string at this point

        if(!makeDirectory(filename)) {
            fprintf(stderr, "Couldn't create subdirectory %s!\n", filename);
            return;
        }

        filename[i] = '/'; // Put the separator back
    }

    if(!i || filename[i-1] == '/')
        return; // empty filename or directory entry

    out = fopen(filename, "wb");

    if(out != NULL) {
        fwrite(data, 1, bytes, out); // best effort is enough here
        fclose(out);
    } else {
        fprintf(stderr, "Couldn't open %s for writing!\n", filename);
    }
}

int processFile(JZFile *zip, JZFileHeader *centralHeader) {
    JZFileHeader header = *centralHeader; // start with central dir values
    char filename[1024];
    unsigned char *data;

    if(jzReadLocalFileHeader(zip, &header, filename, sizeof(filename))) {
        printf("Couldn't read local file header!");
        return -1;
    }

    if(header.uncompressedSize == 0) {
        printf("Unknown uncompressed size for %s, cannot allocate buffer.\n", filename);
        return -1;
    }

    if((data = (unsigned char *)malloc(header.uncompressedSize)) == NULL) {
        printf("Couldn't allocate memory!");
        return -1;
    }

    printf("%s, %d / %d bytes at offset %08X\n", filename,
            header.compressedSize, header.uncompressedSize, header.offset);

    if(jzReadData(zip, &header, data) != Z_OK) {
        printf("Couldn't read file data!");
        free(data);
        return -1;
    }

    writeFile(filename, data, header.uncompressedSize);
    free(data);

    return 0;
}

int recordCallback(JZFile *zip, int idx, JZFileHeader *header, char *filename, void *user_data) {
    long offset;

    offset = zip->tell(zip); // store current position

    if(zip->seek(zip, header->offset, SEEK_SET)) {
        printf("Cannot seek in zip file!");
        return 0; // abort
    }

    processFile(zip, header); // alters file offset

    zip->seek(zip, offset, SEEK_SET); // return to position

    return 1; // continue
}

int main(int argc, char *argv[]) {
    FILE *fp;
    int retval = -1;
    JZEndRecord endRecord;

    JZFile *zip;

    if(argc < 2) {
        puts("Usage: junzip_demo file.zip");
        return -1;
    }

    if(!(fp = fopen(argv[1], "rb"))) {
        printf("Couldn't open \"%s\"!", argv[1]);
        return -1;
    }

    zip = jzfile_from_stdio_file(fp);

    if(jzReadEndRecord(zip, &endRecord)) {
        printf("Couldn't read ZIP file end record.");
        goto endClose;
    }

    if(jzReadCentralDirectory(zip, &endRecord, recordCallback, NULL)) {
        printf("Couldn't read ZIP file central record.");
        goto endClose;
    }

    retval = 0;

endClose:
    zip->close(zip);

    return retval;
}
