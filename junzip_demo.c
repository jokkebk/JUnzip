// Safe reference extractor for the JUnzip library. Needs zlib or puff for
// Deflate support. POSIX builds use directory-descriptor-relative operations;
// Windows builds use a conservative no-follow/no-overwrite policy.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "junzip.h"

#define MAX_ENTRIES 10000U
#define MAX_ENTRY_COMPRESSED (256ULL * 1024ULL * 1024ULL)
#define MAX_ENTRY_UNCOMPRESSED (256ULL * 1024ULL * 1024ULL)
#define MAX_TOTAL_UNCOMPRESSED (1024ULL * 1024ULL * 1024ULL)

#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct {
    int rootfd;
    uint32_t entries;
    uint64_t totalUncompressed;
    int failed;
} ExtractContext;

static void report_failure(ExtractContext *context, const char *message) {
    fprintf(stderr, "%s\n", message);
    context->failed = 1;
}

// ZIP names are parsed as slash-separated relative paths. Backslashes and
// colons are rejected too, keeping the policy safe if names are later used on
// Windows or passed to another platform-specific API.
static int safe_entry_name(const char *filename) {
    size_t length, start, i;

    if(filename == NULL || filename[0] == '\0' ||
            filename[0] == '/' || filename[0] == '\\' ||
            strchr(filename, '\\') != NULL || strchr(filename, ':') != NULL)
        return 0;

    length = strlen(filename);
    start = 0;
    for(i = 0; i <= length; i++) {
        if(i != length && filename[i] != '/')
            continue;

        if(i == start) {
            // A single trailing slash denotes a directory entry. Empty
            // components anywhere else are ambiguous and rejected.
            if(i == length && i > 0 && filename[i - 1] == '/')
                return 1;
            return 0;
        }

        if((i - start == 1 && filename[start] == '.') ||
                (i - start == 2 && filename[start] == '.' &&
                 filename[start + 1] == '.'))
            return 0;

        start = i + 1;
    }

    return 1;
}

static int open_child_directory(int parentfd, const char *name) {
    int fd;

    fd = openat(parentfd, name,
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if(fd >= 0)
        return fd;

    if(errno != ENOENT || mkdirat(parentfd, name, 0700) != 0)
        return -1;

    return openat(parentfd, name,
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

// Open the parent directory of filename using only descriptors rooted at
// rootfd. The returned path is heap-owned and *leaf points into it.
static int open_parent_directory(int rootfd, const char *filename,
        char **pathOut, char **leafOut) {
    char *path, *cursor, *separator;
    int dirfd, nextfd;

    path = (char *)malloc(strlen(filename) + 1U);
    if(path == NULL)
        return -1;
    strcpy(path, filename);

    dirfd = dup(rootfd);
    if(dirfd < 0) {
        free(path);
        return -1;
    }

    cursor = path;
    while((separator = strchr(cursor, '/')) != NULL) {
        *separator = '\0';
        nextfd = open_child_directory(dirfd, cursor);
        if(nextfd < 0) {
            close(dirfd);
            free(path);
            return -1;
        }
        close(dirfd);
        dirfd = nextfd;
        cursor = separator + 1;
    }

    *pathOut = path;
    *leafOut = cursor;
    return dirfd;
}

static int write_file(int rootfd, const char *filename, const void *data,
        size_t bytes) {
    char *path, *leaf;
    int dirfd, fd;
    FILE *out;
    int ok = 1;

    if(!safe_entry_name(filename)) {
        fprintf(stderr, "Rejected unsafe filename: %s\n", filename);
        return -1;
    }

    dirfd = open_parent_directory(rootfd, filename, &path, &leaf);
    if(dirfd < 0) {
        fprintf(stderr, "Couldn't create or open parent for %s: %s\n",
                filename, strerror(errno));
        return -1;
    }

    // A trailing slash names a directory, which has already been created.
    if(*leaf == '\0') {
        close(dirfd);
        free(path);
        return 0;
    }

    // O_EXCL refuses existing files, while O_NOFOLLOW protects the final
    // component. All parent components were opened with O_NOFOLLOW and are
    // held by descriptor while we write.
    fd = openat(dirfd, leaf,
            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if(fd < 0) {
        fprintf(stderr, "Couldn't open %s for writing: %s\n",
                filename, strerror(errno));
        close(dirfd);
        free(path);
        return -1;
    }

    out = fdopen(fd, "wb");
    if(out == NULL) {
        fprintf(stderr, "Couldn't create stream for %s: %s\n",
                filename, strerror(errno));
        close(fd);
        close(dirfd);
        free(path);
        return -1;
    }

    if(bytes > 0 && fwrite(data, 1, bytes, out) != bytes)
        ok = 0;
    if(fflush(out) != 0)
        ok = 0;
    if(fclose(out) != 0)
        ok = 0;
    if(!ok) {
        fprintf(stderr, "Couldn't fully write %s\n", filename);
        close(dirfd);
        free(path);
        return -1;
    }

    close(dirfd);
    free(path);
    return 0;
}

static int process_file(JZFile *zip, int rootfd, JZFileHeader *centralHeader,
        const char *centralFilename) {
    JZFileHeader header = *centralHeader;
    unsigned char *data = NULL;
    uint32_t bytesProduced = 0;
    int ret;

    ret = jzReadLocalFileHeaderChecked(zip, &header, centralFilename);
    if(ret != JZ_OK) {
        fprintf(stderr, "Central/local header mismatch for %s (%d)\n",
                centralFilename, ret);
        return -1;
    }

    if(header.uncompressedSize > MAX_ENTRY_UNCOMPRESSED ||
            header.compressedSize > MAX_ENTRY_COMPRESSED) {
        fprintf(stderr, "Entry exceeds resource limit: %s\n", centralFilename);
        return -1;
    }

    if(header.uncompressedSize > 0) {
        data = (unsigned char *)malloc(header.uncompressedSize);
        if(data == NULL) {
            fprintf(stderr, "Couldn't allocate %u bytes for %s\n",
                    header.uncompressedSize, centralFilename);
            return -1;
        }
    }

    printf("%s, %u / %u bytes at offset %08X\n", centralFilename,
            header.compressedSize, header.uncompressedSize, header.offset);

    ret = jzReadDataLimited(zip, &header, data, header.uncompressedSize,
            (uint32_t)MAX_ENTRY_COMPRESSED, &bytesProduced);
    if(ret != JZ_OK) {
        fprintf(stderr, "Couldn't read file data for %s (%d)\n",
                centralFilename, ret);
        free(data);
        return -1;
    }

    ret = write_file(rootfd, centralFilename, data, bytesProduced);
    free(data);
    return ret;
}

static int validate_callback(JZFile *zip, int idx, JZFileHeader *header,
        char *filename, void *user_data) {
    ExtractContext *context = (ExtractContext *)user_data;
    JZOffset centralPosition;
    int ret;

    (void)idx;
    if(context->entries++ >= MAX_ENTRIES) {
        report_failure(context, "Archive contains too many entries");
        return 0;
    }

    if((uint64_t)header->compressedSize > MAX_ENTRY_COMPRESSED ||
            (uint64_t)header->uncompressedSize > MAX_ENTRY_UNCOMPRESSED ||
            context->totalUncompressed + header->uncompressedSize <
                context->totalUncompressed ||
            context->totalUncompressed + header->uncompressedSize >
                MAX_TOTAL_UNCOMPRESSED) {
        report_failure(context, "Archive exceeds extraction resource limits");
        return 0;
    }
    context->totalUncompressed += header->uncompressedSize;

    if(!safe_entry_name(filename)) {
        report_failure(context, "Archive contains an unsafe entry name");
        return 0;
    }

    if(zip->tell(zip, &centralPosition) || centralPosition > INT64_MAX ||
            zip->seek(zip, (int64_t)header->offset, SEEK_SET)) {
        report_failure(context, "Cannot seek to local file header");
        return 0;
    }

    ret = jzReadLocalFileHeaderChecked(zip, header, filename);
    if(zip->seek(zip, (int64_t)centralPosition, SEEK_SET)) {
        report_failure(context, "Cannot restore central directory position");
        return 0;
    }
    if(ret != JZ_OK) {
        report_failure(context, "Central/local header metadata differs");
        return 0;
    }

    return 1;
}

static int extract_callback(JZFile *zip, int idx, JZFileHeader *header,
        char *filename, void *user_data) {
    ExtractContext *context = (ExtractContext *)user_data;
    JZOffset centralPosition;
    int ret;

    (void)idx;
    if(zip->tell(zip, &centralPosition) || centralPosition > INT64_MAX ||
            zip->seek(zip, (int64_t)header->offset, SEEK_SET)) {
        report_failure(context, "Cannot seek to local file header");
        return 0;
    }

    ret = process_file(zip, context->rootfd, header, filename);
    if(zip->seek(zip, (int64_t)centralPosition, SEEK_SET)) {
        report_failure(context, "Cannot restore central directory position");
        return 0;
    }
    if(ret != 0) {
        context->failed = 1;
        return 0;
    }

    return 1;
}

int main(int argc, char *argv[]) {
    FILE *fp = NULL;
    JZFile *zip = NULL;
    JZEndRecord endRecord;
    ExtractContext context;
    int rootfd = -1;
    int retval = -1;

    if(argc != 3) {
        puts("Usage: junzip_demo file.zip destination-directory");
        return -1;
    }

    rootfd = open(argv[2], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if(rootfd < 0) {
        fprintf(stderr, "Couldn't open destination directory %s: %s\n",
                argv[2], strerror(errno));
        return -1;
    }

    fp = fopen(argv[1], "rb");
    if(fp == NULL) {
        fprintf(stderr, "Couldn't open %s: %s\n", argv[1], strerror(errno));
        close(rootfd);
        return -1;
    }

    zip = jzfile_from_stdio_file(fp);
    if(zip == NULL) {
        fprintf(stderr, "Couldn't allocate ZIP file handle\n");
        fclose(fp);
        close(rootfd);
        return -1;
    }

    memset(&context, 0, sizeof(context));
    context.rootfd = rootfd;

    if(jzReadEndRecord(zip, &endRecord) != JZ_OK) {
        fprintf(stderr, "Couldn't read ZIP file end record\n");
        goto done;
    }

    // First validate every central entry and every corresponding local
    // header. No output is created until the complete archive passes.
    if(jzReadCentralDirectory(zip, &endRecord, validate_callback, &context) !=
            JZ_OK || context.failed) {
        fprintf(stderr, "Archive validation failed\n");
        goto done;
    }

    context.failed = 0;
    if(jzReadCentralDirectory(zip, &endRecord, extract_callback, &context) !=
            JZ_OK || context.failed) {
        fprintf(stderr, "Extraction failed; output may be partial\n");
        goto done;
    }

    retval = 0;

done:
    zip->close(zip);
    close(rootfd);
    return retval;
}

#else

#include <windows.h>

typedef struct {
    const char *rootPath;
    uint32_t entries;
    uint64_t totalUncompressed;
    int failed;
} WinExtractContext;

static void win_report_failure(WinExtractContext *context, const char *message) {
    fprintf(stderr, "%s\n", message);
    context->failed = 1;
}

static int win_safe_entry_name(const char *filename) {
    size_t length, start, i;

    if(filename == NULL || filename[0] == '\0' ||
            filename[0] == '/' || filename[0] == '\\' ||
            strchr(filename, '\\') != NULL || strchr(filename, ':') != NULL)
        return 0;

    length = strlen(filename);
    start = 0;
    for(i = 0; i <= length; i++) {
        if(i != length && filename[i] != '/')
            continue;
        if(i == start) {
            if(i == length && i > 0 && filename[i - 1] == '/')
                return 1;
            return 0;
        }
        if((i - start == 1 && filename[start] == '.') ||
                (i - start == 2 && filename[start] == '.' &&
                 filename[start + 1] == '.'))
            return 0;
        start = i + 1;
    }
    return 1;
}

static int win_directory_is_safe(const char *path) {
    HANDLE handle;
    FILE_ATTRIBUTE_TAG_INFO info;
    int safe = 0;

    handle = CreateFileA(path, 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
                FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if(handle == INVALID_HANDLE_VALUE)
        return 0;
    if(GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info,
                sizeof(info)) &&
            (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        safe = 1;
    CloseHandle(handle);
    return safe;
}

static int win_root_is_safe(const char *path) {
    HANDLE handle;
    FILE_ATTRIBUTE_TAG_INFO info;
    int safe = 0;

    handle = CreateFileA(path, 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
                FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if(handle == INVALID_HANDLE_VALUE)
        return 0;

    if(GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info,
                sizeof(info)) &&
            (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !(info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        safe = 1;

    CloseHandle(handle);
    return safe;
}

static char *win_join_path(const char *root, const char *filename) {
    size_t rootLength = strlen(root);
    size_t nameLength = strlen(filename);
    int needsSeparator = rootLength > 0 && root[rootLength - 1] != '\\' &&
        root[rootLength - 1] != '/';
    char *path = (char *)malloc(rootLength + (size_t)needsSeparator +
        nameLength + 1U);
    size_t i;

    if(path == NULL)
        return NULL;
    memcpy(path, root, rootLength);
    if(needsSeparator)
        path[rootLength++] = '\\';
    memcpy(path + rootLength, filename, nameLength + 1U);
    for(i = rootLength; path[i] != '\0'; i++)
        if(path[i] == '/')
            path[i] = '\\';
    return path;
}

// Windows has no direct equivalent of openat() in the Win32 API. The demo
// therefore checks every path component for reparse points and refuses
// existing targets, while requiring the destination root itself to be a real
// directory. This keeps ordinary extraction usable while avoiding unsafe
// follow-or-overwrite behavior.
static int win_write_file(const char *root, const char *filename,
        const void *data, size_t bytes) {
    char *path;
    char *separator;
    HANDLE handle;
    size_t rootLength = strlen(root);
    DWORD written;
    size_t remaining = bytes;
    const unsigned char *cursor = (const unsigned char *)data;

    if(!win_safe_entry_name(filename)) {
        fprintf(stderr, "Rejected unsafe filename: %s\n", filename);
        return -1;
    }

    path = win_join_path(root, filename);
    if(path == NULL)
        return -1;

    separator = path + rootLength;
    if(*separator == '\\' || *separator == '/')
        separator++;
    while((separator = strchr(separator, '\\')) != NULL) {
        *separator = '\0';
        if(!CreateDirectoryA(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
            *separator = '\\';
            free(path);
            return -1;
        }
        if(!win_directory_is_safe(path)) {
            fprintf(stderr, "Unsafe or non-directory path component in %s\n",
                    filename);
            *separator = '\\';
            free(path);
            return -1;
        }
        *separator = '\\';
        separator++;
    }

    // A trailing slash denotes a directory entry already created above.
    if(path[strlen(path) - 1] == '\\') {
        free(path);
        return 0;
    }

    handle = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if(handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Couldn't create %s; existing targets are rejected\n",
                filename);
        free(path);
        return -1;
    }

    while(remaining > 0) {
        DWORD chunk = remaining > 0xFFFFFFFFU ? 0xFFFFFFFFU :
            (DWORD)remaining;
        if(!WriteFile(handle, cursor, chunk, &written, NULL) || written != chunk) {
            CloseHandle(handle);
            free(path);
            return -1;
        }
        cursor += written;
        remaining -= written;
    }

    if(!CloseHandle(handle)) {
        free(path);
        return -1;
    }
    free(path);
    return 0;
}

static int win_process_file(JZFile *zip, WinExtractContext *context,
        JZFileHeader *centralHeader, const char *centralFilename) {
    JZFileHeader header = *centralHeader;
    unsigned char *data = NULL;
    uint32_t bytesProduced = 0;
    int ret;

    ret = jzReadLocalFileHeaderChecked(zip, &header, centralFilename);
    if(ret != JZ_OK)
        return -1;
    if(header.uncompressedSize > MAX_ENTRY_UNCOMPRESSED ||
            header.compressedSize > MAX_ENTRY_COMPRESSED)
        return -1;
    if(header.uncompressedSize > 0) {
        data = (unsigned char *)malloc(header.uncompressedSize);
        if(data == NULL)
            return -1;
    }
    ret = jzReadDataLimited(zip, &header, data, header.uncompressedSize,
            (uint32_t)MAX_ENTRY_COMPRESSED, &bytesProduced);
    if(ret == JZ_OK)
        ret = win_write_file(context->rootPath, centralFilename, data,
                bytesProduced);
    free(data);
    return ret == 0 || ret == JZ_OK ? 0 : -1;
}

static int win_validate_callback(JZFile *zip, int index, JZFileHeader *header,
        char *filename, void *user_data) {
    WinExtractContext *context = (WinExtractContext *)user_data;
    JZOffset centralPosition;
    int ret;

    (void)index;
    if(context->entries++ >= MAX_ENTRIES ||
            (uint64_t)header->compressedSize > MAX_ENTRY_COMPRESSED ||
            (uint64_t)header->uncompressedSize > MAX_ENTRY_UNCOMPRESSED ||
            context->totalUncompressed + header->uncompressedSize <
                context->totalUncompressed ||
            context->totalUncompressed + header->uncompressedSize >
                MAX_TOTAL_UNCOMPRESSED || !win_safe_entry_name(filename)) {
        win_report_failure(context, "Archive exceeds Windows extraction policy");
        return 0;
    }
    context->totalUncompressed += header->uncompressedSize;
    if(zip->tell(zip, &centralPosition) || centralPosition > INT64_MAX ||
            zip->seek(zip, (int64_t)header->offset, SEEK_SET)) {
        win_report_failure(context, "Cannot seek to local file header");
        return 0;
    }
    ret = jzReadLocalFileHeaderChecked(zip, header, filename);
    if(zip->seek(zip, (int64_t)centralPosition, SEEK_SET) || ret != JZ_OK) {
        win_report_failure(context, "Central/local header validation failed");
        return 0;
    }
    return 1;
}

static int win_extract_callback(JZFile *zip, int index, JZFileHeader *header,
        char *filename, void *user_data) {
    WinExtractContext *context = (WinExtractContext *)user_data;
    JZOffset centralPosition;
    int ret;

    (void)index;
    if(zip->tell(zip, &centralPosition) || centralPosition > INT64_MAX ||
            zip->seek(zip, (int64_t)header->offset, SEEK_SET)) {
        win_report_failure(context, "Cannot seek to local file header");
        return 0;
    }
    ret = win_process_file(zip, context, header, filename);
    if(zip->seek(zip, (int64_t)centralPosition, SEEK_SET) || ret != 0) {
        win_report_failure(context, "Windows extraction rejected the entry");
        return 0;
    }
    return 1;
}

int main(int argc, char *argv[]) {
    FILE *fp = NULL;
    JZFile *zip = NULL;
    JZEndRecord endRecord;
    WinExtractContext context;
    int retval = -1;

    if(argc != 3) {
        puts("Usage: junzip_demo file.zip destination-directory");
        return -1;
    }
    if(!win_root_is_safe(argv[2])) {
        fprintf(stderr, "Destination must be an existing real directory\n");
        return -1;
    }
    fp = fopen(argv[1], "rb");
    if(fp == NULL)
        return -1;
    zip = jzfile_from_stdio_file(fp);
    if(zip == NULL) {
        fclose(fp);
        return -1;
    }
    memset(&context, 0, sizeof(context));
    context.rootPath = argv[2];
    if(jzReadEndRecord(zip, &endRecord) != JZ_OK ||
            jzReadCentralDirectory(zip, &endRecord, win_validate_callback,
                &context) != JZ_OK || context.failed ||
            jzReadCentralDirectory(zip, &endRecord, win_extract_callback,
                &context) != JZ_OK || context.failed) {
        zip->close(zip);
        return -1;
    }
    retval = 0;
    zip->close(zip);
    return retval;
}

#endif
