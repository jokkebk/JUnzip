/**
 * JUnzip library by Joonas Pihlajamaa (firstname.lastname@iki.fi).
 * Released into public domain. https://github.com/jokkebk/JUnzip
 */

#ifndef __JUNZIP_H
#define __JUNZIP_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <stdint.h>
#include <stddef.h>

// Enable compiling without Zlib as well
// (no compression support, only "store")
#ifdef HAVE_ZLIB
#include <zlib.h>
#else
#define Z_OK 0
#define Z_ERRNO -1
#endif

// JUnzip return codes. JZ_OK is zero and equal to Z_OK; all errors are
// negative, so legacy `if(ret)` and `ret != Z_OK` checks keep working.
#define JZ_OK 0               // success
#define JZ_ERR (-1)           // I/O or generic error (same value as Z_ERRNO)
#define JZ_ERR_UNSUPPORTED (-2) // valid ZIP feature not implemented (ZIP64, encryption, method)
#define JZ_ERR_CORRUPT (-3)   // malformed, truncated or internally inconsistent archive
#define JZ_ERR_SIZE (-4)      // claimed size exceeds the caller-provided buffer

#ifdef HAVE_PUFF
#include "puff.h"
#endif

// If you don't have stdint.h, the following two lines should work for most 32/64 bit systems
// typedef unsigned int uint32_t;
// typedef unsigned short uint16_t;

#define JZHOUR(t) ((t)>>11)
#define JZMINUTE(t) (((t)>>5) & 63)
#define JZSECOND(t) (((t) & 31) * 2)
#define JZTIME(h,m,s) (((h)<<11) + ((m)<<5) + (s)/2)

#define JZYEAR(t) (((t)>>9) + 1980)
#define JZMONTH(t) (((t)>>5) & 15)
#define JZDAY(t) ((t) & 31)
#define JZDATE(y,m,d) ((((y)-1980)<<9) + ((m)<<5) + (d))

typedef struct JZFile JZFile;

// File positions are always represented as unsigned 64-bit values.  Seek
// offsets are signed because SEEK_CUR may legally use a negative offset.
typedef uint64_t JZOffset;

struct JZFile {
    size_t (*read)(JZFile *file, void *buf, size_t size);
    int (*tell)(JZFile *file, JZOffset *offset);
    int (*seek)(JZFile *file, int64_t offset, int whence);
    int (*error)(JZFile *file);
    void (*close)(JZFile *file);
};

JZFile *
jzfile_from_stdio_file(FILE *fp);

typedef struct __attribute__ ((__packed__)) {
    uint32_t signature; // 0x04034B50
    uint16_t versionNeededToExtract; // unsupported
    uint16_t generalPurposeBitFlag; // unsupported
    uint16_t compressionMethod;
    uint16_t lastModFileTime;
    uint16_t lastModFileDate;
    uint32_t crc32;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint16_t fileNameLength;
    uint16_t extraFieldLength; // unsupported
} JZLocalFileHeader;

typedef struct __attribute__ ((__packed__)) {
    uint32_t signature; // 0x02014B50
    uint16_t versionMadeBy; // unsupported
    uint16_t versionNeededToExtract; // unsupported
    uint16_t generalPurposeBitFlag; // unsupported
    uint16_t compressionMethod;
    uint16_t lastModFileTime;
    uint16_t lastModFileDate;
    uint32_t crc32;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint16_t fileNameLength;
    uint16_t extraFieldLength; // unsupported
    uint16_t fileCommentLength; // unsupported
    uint16_t diskNumberStart; // unsupported
    uint16_t internalFileAttributes; // unsupported
    uint32_t externalFileAttributes; // unsupported
    uint32_t relativeOffsetOflocalHeader;
} JZGlobalFileHeader;

typedef struct __attribute__ ((__packed__)) {
    uint16_t versionNeededToExtract;
    uint16_t generalPurposeBitFlag; // bit 3 indicates data descriptor follows
    uint16_t compressionMethod;
    uint16_t lastModFileTime;
    uint16_t lastModFileDate;
    uint32_t crc32;
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint32_t offset;
} JZFileHeader;

typedef struct __attribute__ ((__packed__)) {
    uint32_t signature; // 0x06054b50
    uint16_t diskNumber; // unsupported
    uint16_t centralDirectoryDiskNumber; // unsupported
    uint16_t numEntriesThisDisk; // unsupported
    uint16_t numEntries;
    uint32_t centralDirectorySize;
    uint32_t centralDirectoryOffset;
    uint16_t zipCommentLength;
    // Followed by .ZIP file comment (variable size)
} JZEndRecord;

// Callback prototype for central and local file record reading functions.
// The filename pointer is only valid for the duration of the callback;
// copy it if you need it later.
typedef int (*JZRecordCallback)(JZFile *zip, int index, JZFileHeader *header,
        char *filename, void *user_data);

// Maximum ZIP filename length representable by the classic ZIP format.
#define JZ_MAX_FILENAME_LENGTH 65535

// Read ZIP file end record. Will move within file.
int jzReadEndRecord(JZFile *zip, JZEndRecord *endRecord);

// Read ZIP file global directory. Will move within file.
// Callback is called for each record, until callback returns zero
int jzReadCentralDirectory(JZFile *zip, JZEndRecord *endRecord,
        JZRecordCallback callback, void *user_data);

// Read local ZIP file header. This legacy function updates header with local
// values when the data-descriptor flag is clear. Callers that have validated
// central-directory metadata should use jzReadLocalFileHeaderChecked instead.
int jzReadLocalFileHeader(JZFile *zip, JZFileHeader *header,
        char *filename, int len);

// Same as above but returns the full raw header
int jzReadLocalFileHeaderRaw(JZFile *zip, JZLocalFileHeader *header,
        char *filename, int len);

// Read a local header and verify it against the already validated central
// header. The central header is never modified. The local filename is checked
// byte-for-byte against centralFilename, as are the method, flags, version,
// and (when bit 3 is clear) CRC and sizes.
int jzReadLocalFileHeaderChecked(JZFile *zip, JZFileHeader *centralHeader,
        const char *centralFilename);

// Standard CRC-32 (ZIP polynomial 0xEDB88320), available regardless of
// which decompression backend (if any) is compiled in. Compatible with
// zlib's crc32() calling convention: pass crc=0 for a single-shot checksum.
uint32_t jz_crc32(uint32_t crc, const void *data, size_t size);

// Read data from file stream, described by header, to preallocated buffer.
// Returns JZ_OK (zero) on success, negative JZ_ERR_* code on failure.
//
// Security contract:
// - bufferSize is the hard bound: entries claiming uncompressedSize larger
//   than bufferSize are rejected with JZ_ERR_SIZE before any data is read.
// - On success *bytesProduced == header->uncompressedSize; decompression
//   that under- or over-produces, or a store entry whose compressed and
//   uncompressed sizes differ, is rejected as JZ_ERR_CORRUPT.
// - CRC32 of the produced bytes is verified against header->crc32, and a
//   data descriptor (bit 3), when present, must agree with the header
//   values that bounded the read. The header is never mutated.
// - Encrypted entries, ZIP64 sentinel sizes and unknown compression methods
//   are rejected with JZ_ERR_UNSUPPORTED.
// - When bit 3 is set, sizes/CRC must be pre-filled from the central
//   directory entry (local header fields are zero in that case).
//
// Resource contract: callers must independently bound compressedSize,
// uncompressedSize, entry count, and aggregate output before allocating or
// decoding. Use jzReadDataLimited when the compressed-size bound must be
// enforced by the library before a backend reads or allocates input. The
// whole-entry API is not a streaming resource limit.
int jzReadData(JZFile *zip, JZFileHeader *header, void *buffer,
        size_t bufferSize, uint32_t *bytesProduced);

// Bounded variant for callers that also need to cap archive-controlled input
// work/allocation (required by the whole-input Puff backend).
int jzReadDataLimited(JZFile *zip, JZFileHeader *header, void *buffer,
        size_t bufferSize, uint32_t compressedSizeLimit,
        uint32_t *bytesProduced);

#ifdef __cplusplus
};
#endif /* __cplusplus */

#endif
