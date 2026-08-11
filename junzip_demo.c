// Simple demo for JUnzip library. Needs zlib. Should compile with
// something like gcc junzip_demo.c junzip.c -lz -o junzip.exe
//
// Extraction hardening notes: archived filenames are validated
// (jzFilenameIsSafe), existing symlink path components are refused,
// extraction is bounded by per-file and total budgets, archived
// permission bits are deliberately ignored (no setuid/device creation),
// and any entry failure aborts with a nonzero exit code.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "junzip.h"

#include <errno.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(d) _mkdir(d)
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#define MKDIR(d) mkdir((d), 0755)
#endif

// Zip-bomb budgets for this demo; a library consumer should pick its own.
#define MAX_FILE_SIZE ((uint64_t)256 * 1024 * 1024)  // per entry
#define MAX_TOTAL_SIZE ((uint64_t)1024 * 1024 * 1024) // whole archive

typedef struct {
    uint64_t totalBytes;
    int failed;
} ExtractState;

// Print an archived filename with control characters neutralized.
static void printName(const char *filename, char *buf, size_t bufSize) {
    size_t i;
    for(i = 0; i + 1 < bufSize && filename[i]; i++) {
        unsigned char c = (unsigned char)filename[i];
        buf[i] = (c >= 32 && c != 0x7F) ? (char)c : '?';
    }
    buf[i] = '\0';
}

// Create the directory if necessary; return 1 on success/existing
int makeDirectory(char *dir) {
    if(MKDIR(dir) == 0)
        return 1; // created ok

    if(errno == EEXIST)
        return 1; // already there

    fprintf(stderr, "Couldn't create %s (%d)\n", dir, errno);
    return 0;
}

#ifndef _WIN32
// Refuse to extract through symlinked path components: an archive (or a
// hostile pre-existing directory tree) could otherwise redirect writes
// outside the extraction root. Returns 1 if no component is a symlink.
static int pathFreeOfSymlinks(const char *filename) {
    char partial[1024];
    size_t i;
    struct stat st;

    for(i = 0; filename[i] && i + 1 < sizeof(partial); i++) {
        partial[i] = filename[i];
        if(filename[i] != '/')
            continue;
        partial[i] = '\0';
        if(lstat(partial, &st) == 0 && S_ISLNK(st.st_mode))
            return 0;
        partial[i] = '/';
    }
    return 1;
}
#endif

int writeFile(char *filename, void *data, uint32_t bytes) {
    FILE *out;
    int i;
    char safeName[1024];

    printName(filename, safeName, sizeof(safeName));

    if(!jzFilenameIsSafe(filename)) {
        fprintf(stderr, "Skipping unsafe filename: %s\n", safeName);
        return -1;
    }

#ifndef _WIN32
    if(!pathFreeOfSymlinks(filename)) {
        fprintf(stderr, "Skipping %s: path contains a symlink\n", safeName);
        return -1;
    }
#endif

    // simplistic directory creation support
    for(i=0; filename[i]; i++) {
        if(filename[i] != '/')
            continue;

        filename[i] = '\0'; // terminate string at this point

        if(!makeDirectory(filename)) {
            fprintf(stderr, "Couldn't create subdirectory %s!\n", safeName);
            return -1;
        }

#ifndef _WIN32
        // mkdir succeeding with EEXIST says nothing about the entry type;
        // an attacker-planted symlink or file must stop extraction.
        struct stat st;
        if(lstat(filename, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "%s exists and is not a directory!\n", safeName);
            return -1;
        }
#endif

        filename[i] = '/'; // Put the separator back
    }

    if(!i || filename[i-1] == '/')
        return 0; // empty filename or directory entry

    // Note: archived Unix mode bits are intentionally not honored. O_NOFOLLOW
    // prevents an existing final-component symlink from being truncated.
#ifndef _WIN32
    {
        int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW,
                0600);
        if(fd >= 0) {
            out = fdopen(fd, "wb");
            if(out == NULL)
                close(fd);
        } else {
            out = NULL;
        }
    }
#else
    out = fopen(filename, "wb");
#endif

    if(out == NULL) {
        fprintf(stderr, "Couldn't open %s for writing!\n", safeName);
        return -1;
    }

    if(bytes > 0 && fwrite(data, 1, bytes, out) != bytes) {
        fprintf(stderr, "Short write on %s (disk full?)\n", safeName);
        fclose(out);
        return -1;
    }

    if(fclose(out) != 0) {
        fprintf(stderr, "Could not close %s after writing\n", safeName);
        return -1;
    }
    return 0;
}

int processFile(JZFile *zip, JZFileHeader *centralHeader,
        const char *centralName, ExtractState *state) {
    JZFileHeader header = *centralHeader; // start with central dir values
    char filename[1024];
    char safeName[1024];
    unsigned char *data = NULL;
    uint32_t bytesProduced = 0;
    int ret;

    if(jzReadLocalFileHeader(zip, &header, filename, sizeof(filename))) {
        printf("Couldn't read local file header!");
        return -1;
    }

    if(centralName == NULL || strcmp(filename, centralName) != 0) {
        printf("Local and central filenames differ, aborting extraction\n");
        return -1;
    }

    printName(filename, safeName, sizeof(safeName));

    if(header.uncompressedSize > MAX_FILE_SIZE) {
        printf("%s exceeds per-file budget, skipping\n", safeName);
        return -1;
    }

    if(header.uncompressedSize > MAX_TOTAL_SIZE - state->totalBytes) {
        printf("Extraction exceeds total budget, aborting\n");
        return -1;
    }

    if(header.uncompressedSize > 0 &&
            (data = (unsigned char *)malloc(header.uncompressedSize)) == NULL) {
        printf("Couldn't allocate memory!");
        return -1;
    }

    printf("%s, %u / %u bytes at offset %08X\n", safeName,
            header.compressedSize, header.uncompressedSize, header.offset);

    ret = jzReadData(zip, &header, data, header.uncompressedSize,
            &bytesProduced);
    if(ret != JZ_OK) {
        printf("Couldn't read file data (%d)!", ret);
        free(data);
        return -1;
    }

    ret = writeFile(filename, data, bytesProduced);
    free(data);
    if(ret != 0)
        return -1;

    state->totalBytes += bytesProduced;
    return 0;
}

int recordCallback(JZFile *zip, int idx, JZFileHeader *header, char *filename, void *user_data) {
    ExtractState *state = (ExtractState *)user_data;
    size_t offset;
    (void)idx;

    if(state == NULL || filename == NULL)
        return 0;

    offset = zip->tell(zip); // store current position
    if(offset == SIZE_MAX) {
        state->failed = 1;
        return 0;
    }

    if(zip->seek(zip, header->offset, SEEK_SET)) {
        printf("Cannot seek in zip file!");
        state->failed = 1;
        return 0; // abort
    }

    if(processFile(zip, header, filename, state) != 0) {
        state->failed = 1;
        (void)zip->seek(zip, offset, SEEK_SET);
        return 0; // abort on first failure: never partially extract
    }

    if(zip->seek(zip, offset, SEEK_SET)) {
        state->failed = 1;
        return 0;
    }

    return 1; // continue
}

int main(int argc, char *argv[]) {
    FILE *fp;
    int retval = -1;
    JZEndRecord endRecord;
    ExtractState state = { 0, 0 };

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
    if(zip == NULL) {
        fclose(fp);
        printf("Couldn't allocate ZIP file handle!");
        return -1;
    }

    if(jzReadEndRecord(zip, &endRecord)) {
        printf("Couldn't read ZIP file end record.");
        goto endClose;
    }

    if(jzReadCentralDirectory(zip, &endRecord, recordCallback, &state)) {
        printf("Couldn't read ZIP file central record.");
        goto endClose;
    }

    if(state.failed) {
        printf("Extraction failed, aborting.\n");
        goto endClose;
    }

    printf("Extracted %llu bytes total.\n",
            (unsigned long long)state.totalBytes);
    retval = 0;

endClose:
    zip->close(zip);

    return retval;
}
