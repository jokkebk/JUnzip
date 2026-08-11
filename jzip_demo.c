/* Simple ZIP writer. Files are stored without compression unless
 * HAVE_ZLIB is enabled, in which case entries use raw deflate. */

#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>

#include "junzip.h"

static int writeBytes(FILE *file, const void *data, size_t size) {
    return size == 0 || fwrite(data, 1, size, file) == size;
}

static int filePosition(FILE *file, uint32_t *position) {
    long value = ftell(file);

    if(value < 0 || (uint64_t)value > UINT32_MAX)
        return 0;
    *position = (uint32_t)value;
    return 1;
}

static const char *archiveName(const char *path) {
    const char *name = path + strlen(path);

    while(name > path && name[-1] != '/' && name[-1] != '\\')
        name--;
    return name;
}

#ifdef HAVE_ZLIB
static int compressFile(FILE *input, uint32_t inputSize,
        unsigned char **compressedOut, uint32_t *compressedSizeOut) {
    unsigned long capacity = compressBound(inputSize);
    unsigned char *data = NULL;
    unsigned char *compressed;
    unsigned char empty = 0;
    z_stream stream;
    size_t bytes;

    if(capacity > UINT_MAX)
        return 0;

    compressed = (unsigned char *)malloc(capacity);
    if(compressed == NULL)
        return 0;

    if(inputSize > 0) {
        data = (unsigned char *)malloc(inputSize);
        if(data == NULL) {
            free(compressed);
            return 0;
        }
    }

    if(fseek(input, 0, SEEK_SET) != 0) {
        free(data);
        free(compressed);
        return 0;
    }

    bytes = inputSize == 0 ? 0 : fread(data, 1, inputSize, input);
    if(bytes != inputSize || ferror(input)) {
        free(data);
        free(compressed);
        return 0;
    }

    memset(&stream, 0, sizeof(stream));
    if(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
            -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        free(data);
        free(compressed);
        return 0;
    }

    stream.next_in = inputSize == 0 ? &empty : data;
    stream.avail_in = inputSize;
    stream.next_out = compressed;
    stream.avail_out = (uInt)capacity;

    if(deflate(&stream, Z_FINISH) != Z_STREAM_END ||
            stream.total_out > UINT32_MAX) {
        deflateEnd(&stream);
        free(data);
        free(compressed);
        return 0;
    }

    *compressedSizeOut = (uint32_t)stream.total_out;
    *compressedOut = compressed;
    deflateEnd(&stream);
    free(data);
    return 1;
}
#endif

int main(int argc, char *argv[]) {
    FILE *out = NULL;
    FILE *in;
    unsigned char buf[1 << 15];
    int files = 0;
    int requestedFiles;
    int i;
    int status = -1;
    char **filenames = NULL;
    JZGlobalFileHeader *globalHeader = NULL;
    JZEndRecord endRecord;
    uint32_t centralOffset;

    if(argc < 3) {
        puts("Usage: jzip_demo file1 [file2 file3 ...] output.zip");
        return 1;
    }

    requestedFiles = argc - 2;
    if((uint64_t)requestedFiles > UINT16_MAX) {
        puts("Too many input files for classic ZIP format.");
        return 1;
    }

    // Avoid the most obvious self-truncation case before opening output.
    for(i = 1; i < argc - 1; i++) {
        if(strcmp(argv[i], argv[argc - 1]) == 0) {
            puts("An input file cannot also be the output archive.");
            return 1;
        }
    }

    out = fopen(argv[argc - 1], "wb");
    if(out == NULL) {
        printf("Couldn't open \"%s\" for writing!\n", argv[argc - 1]);
        return 1;
    }

    globalHeader = (JZGlobalFileHeader *)calloc((size_t)requestedFiles,
            sizeof(JZGlobalFileHeader));
    filenames = (char **)calloc((size_t)requestedFiles, sizeof(char *));
    if(globalHeader == NULL || filenames == NULL) {
        puts("Failed to allocate buffers for ZIP headers.");
        goto cleanup;
    }

    for(i = 0; i < requestedFiles; i++) {
        JZLocalFileHeader localHeader;
        JZGlobalFileHeader *global;
        struct stat st;
        struct tm *timeinfo;
        const char *name = archiveName(argv[i + 1]);
        size_t nameLength = strlen(name);
        uint32_t inputSize;
        uint32_t offset;
        uint32_t outputOffset;

        if(!jzFilenameIsSafe(name) || nameLength > UINT16_MAX) {
            printf("Skipping unsafe or too-long archive name \"%s\".\n",
                    name);
            continue;
        }

        if((in = fopen(argv[i + 1], "rb")) == NULL) {
            printf("File \"%s\" not found, skipping.\n", argv[i + 1]);
            continue;
        }

        if(stat(argv[i + 1], &st) != 0 || st.st_size < 0 ||
                (uint64_t)st.st_size > UINT32_MAX) {
            printf("Cannot use \"%s\" in a classic ZIP archive.\n",
                    argv[i + 1]);
            fclose(in);
            continue;
        }
        inputSize = (uint32_t)st.st_size;

        timeinfo = localtime(&st.st_mtime);
        if(timeinfo == NULL) {
            puts("Could not convert input file timestamp.");
            fclose(in);
            goto cleanup;
        }

        if(!filePosition(out, &outputOffset)) {
            puts("Output archive is too large for classic ZIP format.");
            fclose(in);
            goto cleanup;
        }

        memset(&localHeader, 0, sizeof(localHeader));
        localHeader.signature = 0x04034B50;
        localHeader.versionNeededToExtract = 20;
        localHeader.compressionMethod = 0;
        localHeader.lastModFileTime = (uint16_t)JZTIME(
                timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
        localHeader.lastModFileDate = (uint16_t)JZDATE(
                timeinfo->tm_year + 1900, timeinfo->tm_mon + 1,
                timeinfo->tm_mday);
        localHeader.crc32 = 0;
        localHeader.fileNameLength = (uint16_t)nameLength;

        // Calculate the CRC before writing either header or payload.
        if(fseek(in, 0, SEEK_SET) != 0) {
            fclose(in);
            goto cleanup;
        }
        while(!feof(in) && !ferror(in)) {
            size_t bytes = fread(buf, 1, sizeof(buf), in);
            localHeader.crc32 = jz_crc32(localHeader.crc32, buf, bytes);
        }
        if(ferror(in)) {
            puts("Error while calculating CRC.");
            fclose(in);
            goto cleanup;
        }

        if(!filePosition(in, &offset) || offset != inputSize) {
            puts("Input changed while it was being read.");
            fclose(in);
            goto cleanup;
        }

        localHeader.uncompressedSize = inputSize;

        global = globalHeader + files;
        memset(global, 0, sizeof(*global));
        global->signature = 0x02014B50;
        global->versionMadeBy = 20;
        global->versionNeededToExtract = localHeader.versionNeededToExtract;
        global->compressionMethod = localHeader.compressionMethod;
        global->lastModFileTime = localHeader.lastModFileTime;
        global->lastModFileDate = localHeader.lastModFileDate;
        global->crc32 = localHeader.crc32;
        global->uncompressedSize = inputSize;
        global->fileNameLength = localHeader.fileNameLength;
        global->externalFileAttributes = 0x20;
        global->relativeOffsetOflocalHeader = outputOffset;

#ifdef HAVE_ZLIB
        {
            unsigned char *compressed = NULL;
            uint32_t compressedSize;

            if(!compressFile(in, inputSize, &compressed, &compressedSize)) {
                puts("Compression failed.");
                fclose(in);
                goto cleanup;
            }
            localHeader.compressionMethod = 8;
            localHeader.compressedSize = compressedSize;
            global->compressionMethod = 8;
            global->compressedSize = compressedSize;
            printf("Compressed %s from %u to %u bytes.\n", name,
                    inputSize, compressedSize);

            if(!writeBytes(out, &localHeader, sizeof(localHeader)) ||
                    !writeBytes(out, name, nameLength) ||
                    !writeBytes(out, compressed, compressedSize)) {
                free(compressed);
                fclose(in);
                puts("Error while writing an archive entry.");
                goto cleanup;
            }
            free(compressed);
        }
#else
        localHeader.compressedSize = inputSize;
        global->compressedSize = inputSize;
        if(fseek(in, 0, SEEK_SET) != 0 ||
                !writeBytes(out, &localHeader, sizeof(localHeader)) ||
                !writeBytes(out, name, nameLength)) {
            fclose(in);
            puts("Error while writing an archive entry.");
            goto cleanup;
        }
        while(!feof(in) && !ferror(in)) {
            size_t bytes = fread(buf, 1, sizeof(buf), in);
            if(!writeBytes(out, buf, bytes)) {
                fclose(in);
                puts("Error while writing an archive entry.");
                goto cleanup;
            }
        }
        if(ferror(in)) {
            fclose(in);
            puts("Error while reading an archive entry.");
            goto cleanup;
        }
#endif

        filenames[files++] = (char *)name;
        fclose(in);
    }

    if(!filePosition(out, &centralOffset)) {
        puts("Output archive is too large for classic ZIP format.");
        goto cleanup;
    }

    for(i = 0; i < files; i++) {
        size_t nameLength = strlen(filenames[i]);
        if(!writeBytes(out, globalHeader + i, sizeof(JZGlobalFileHeader)) ||
                !writeBytes(out, filenames[i], nameLength)) {
            puts("Error while writing the central directory.");
            goto cleanup;
        }
    }

    memset(&endRecord, 0, sizeof(endRecord));
    endRecord.signature = 0x06054B50;
    endRecord.numEntriesThisDisk = (uint16_t)files;
    endRecord.numEntries = (uint16_t)files;
    endRecord.centralDirectoryOffset = centralOffset;
    {
        uint32_t endPosition;
        if(!filePosition(out, &endPosition) || endPosition < centralOffset) {
            puts("Invalid central directory position.");
            goto cleanup;
        }
        endRecord.centralDirectorySize = endPosition - centralOffset;
    }

    if(!writeBytes(out, &endRecord, sizeof(endRecord)) || fclose(out) != 0) {
        out = NULL;
        puts("Error while writing the end record.");
        goto cleanup;
    }
    out = NULL;
    status = 0;

cleanup:
    if(out != NULL)
        fclose(out);
    free(globalHeader);
    free(filenames);
    return status;
}
