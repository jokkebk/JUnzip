// JUnzip library by Joonas Pihlajamaa. See junzip.h for license and details.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "junzip.h"

// Standard CRC-32 (polynomial 0xEDB88320, reflected), as used by the ZIP
// format. The table is built on the stack per call: it costs ~2k simple
// operations and keeps the function reentrant.
uint32_t jz_crc32(uint32_t crc, const void *data, size_t size) {
    uint32_t table[256];
    const unsigned char *p = (const unsigned char *)data;
    size_t i;
    int j;

    for(i = 0; i < 256; i++) {
        uint32_t r = (uint32_t)i;
        for(j = 0; j < 8; j++)
            r = (r & 1) ? (r >> 1) ^ 0xEDB88320UL : (r >> 1);
        table[i] = r;
    }

    crc = crc ^ 0xFFFFFFFFUL;
    for(i = 0; i < size; i++)
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);

    return crc ^ 0xFFFFFFFFUL;
}

int jzFilenameIsSafe(const char *filename) {
    const char *p = filename;
    size_t componentLen = 0;
    int previousWasSeparator = 0;

    if(p == NULL || *p == '\0')
        return 0; // empty name

    if(*p == '/' || *p == '\\')
        return 0; // absolute path

    if(((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) && p[1] == ':')
        return 0; // drive letter

    for(; *p; p++) {
        if((unsigned char)*p < 32 || *p == 0x7F)
            return 0; // control characters (also keeps terminals sane)

        if(*p == '/' || *p == '\\') {
            if(previousWasSeparator || componentLen == 0)
                return 0; // empty path component

            // Reject "." and ".." path components anywhere in the name.
            if(componentLen == 1 || componentLen == 2) {
                const char *component = p - componentLen;
                if(component[0] == '.' &&
                        (componentLen == 1 || component[1] == '.'))
                    return 0;
            }
            componentLen = 0;
            previousWasSeparator = 1;
        } else {
            componentLen++;
            previousWasSeparator = 0;
        }
    }

    if(previousWasSeparator)
        return 1; // one trailing separator denotes a directory entry

    // Trailing component must not be "." or ".." either ("foo/..").
    if(componentLen == 1 || componentLen == 2) {
        const char *component = p - componentLen;
        if(component[0] == '.' && (componentLen == 1 || component[1] == '.'))
            return 0;
    }

    return 1;
}

// The end record sits at most 22 + 65535 (maximum comment) bytes from EOF.
#define JZ_ENDRECORD_WINDOW ((long)sizeof(JZEndRecord) + 65535)

// Read ZIP file end record. Will move within file.
int jzReadEndRecord(JZFile *zip, JZEndRecord *endRecord) {
    long fileSize, readBytes, i;
    long erPos = -1;
    unsigned char *window;
    int retval = JZ_ERR;

    if(zip == NULL || endRecord == NULL || zip->seek == NULL ||
            zip->tell == NULL || zip->read == NULL)
        return JZ_ERR;

    if(zip->seek(zip, 0, SEEK_END)) {
        fprintf(stderr, "Couldn't go to end of zip file!");
        return JZ_ERR;
    }

    // A valid empty archive is exactly sizeof(JZEndRecord) bytes.
    if((fileSize = zip->tell(zip)) < (long)sizeof(JZEndRecord)) {
        fprintf(stderr, "Too small file to be a zip!");
        return JZ_ERR;
    }

    readBytes = (fileSize < JZ_ENDRECORD_WINDOW) ? fileSize : JZ_ENDRECORD_WINDOW;

    if((window = (unsigned char *)malloc(readBytes)) == NULL) {
        fprintf(stderr, "Couldn't allocate end record scan buffer!");
        return JZ_ERR;
    }

    if(zip->seek(zip, fileSize - readBytes, SEEK_SET)) {
        fprintf(stderr, "Cannot seek in zip file!");
        goto done;
    }

    if(zip->read(zip, window, (size_t)readBytes) < (size_t)readBytes) {
        fprintf(stderr, "Couldn't read end of zip file!");
        goto done;
    }

    // Scan backwards for end record signature, validating that the candidate
    // sits at exactly fileSize - sizeof(JZEndRecord) - zipCommentLength.
    // This rejects false positives from signatures embedded in file data or
    // ZIP comments.
    for(i = readBytes - sizeof(JZEndRecord); i >= 0; i--) {
        JZEndRecord *er = (JZEndRecord *)(window + i);
        if(er->signature == 0x06054B50) {
            long candidatePos = fileSize - readBytes + i;
            if(er->zipCommentLength ==
                    fileSize - candidatePos - (long)sizeof(JZEndRecord)) {
                erPos = candidatePos;
                memcpy(endRecord, er, sizeof(JZEndRecord));
                retval = JZ_OK;
                break;
            }
        }
    }

    if(retval != JZ_OK) {
        fprintf(stderr, "End record signature not found in zip!");
        goto done;
    }

    if(endRecord->diskNumber || endRecord->centralDirectoryDiskNumber ||
            endRecord->numEntries != endRecord->numEntriesThisDisk) {
        fprintf(stderr, "Multifile zips not supported!");
        retval = JZ_ERR_UNSUPPORTED;
        goto done;
    }

    if(endRecord->numEntries == 0xFFFF ||
            endRecord->centralDirectoryOffset == 0xFFFFFFFF ||
            endRecord->centralDirectorySize == 0xFFFFFFFF) {
        fprintf(stderr, "ZIP64 archives not supported!");
        retval = JZ_ERR_UNSUPPORTED;
        goto done;
    }

    // The central directory must fit entirely before the end record. Do the
    // math in 64 bits: the 32-bit sum can wrap and bypass the check.
    if((uint64_t)endRecord->centralDirectoryOffset +
            endRecord->centralDirectorySize > (uint64_t)erPos) {
        fprintf(stderr, "Central directory exceeds archive layout!");
        retval = JZ_ERR_CORRUPT;
        goto done;
    }

    // Every entry occupies at least sizeof(JZGlobalFileHeader) bytes.
    if((uint64_t)endRecord->numEntries * sizeof(JZGlobalFileHeader) >
            endRecord->centralDirectorySize) {
        fprintf(stderr, "Central directory too small for entry count!");
        retval = JZ_ERR_CORRUPT;
        goto done;
    }

done:
    free(window);
    return retval;
}

// Read ZIP file global directory. Will move within file.
// Iteration is bounded by endRecord->centralDirectorySize: entries reaching
// past the declared directory are treated as corruption, not parsed.
int jzReadCentralDirectory(JZFile *zip, JZEndRecord *endRecord,
        JZRecordCallback callback, void *user_data) {
    JZGlobalFileHeader fileHeader;
    JZFileHeader header;
    uint64_t remaining;
    char *filename;
    int i, keepGoing;

    if(zip == NULL || endRecord == NULL || callback == NULL ||
            zip->seek == NULL || zip->read == NULL)
        return JZ_ERR;

    remaining = endRecord->centralDirectorySize;

    if(zip->seek(zip, endRecord->centralDirectoryOffset, SEEK_SET)) {
        fprintf(stderr, "Cannot seek in zip file!");
        return JZ_ERR;
    }

    for(i=0; i<endRecord->numEntries; i++) {
        if(remaining < sizeof(JZGlobalFileHeader)) {
            fprintf(stderr, "Central directory truncated at entry %d!", i);
            return JZ_ERR_CORRUPT;
        }

        if(zip->read(zip, &fileHeader, sizeof(JZGlobalFileHeader)) <
                sizeof(JZGlobalFileHeader)) {
            fprintf(stderr, "Couldn't read file header %d!", i);
            return JZ_ERR;
        }
        remaining -= sizeof(JZGlobalFileHeader);

        if(fileHeader.signature != 0x02014B50) {
            fprintf(stderr, "Invalid file header signature %d!", i);
            return JZ_ERR_CORRUPT;
        }

        if(fileHeader.generalPurposeBitFlag & 0x01) {
            fprintf(stderr, "Encrypted entry %d not supported!", i);
            return JZ_ERR_UNSUPPORTED;
        }

        if(fileHeader.versionNeededToExtract > 20) {
            fprintf(stderr, "Entry %d needs unsupported version %d!",
                    i, fileHeader.versionNeededToExtract);
            return JZ_ERR_UNSUPPORTED;
        }

        if(fileHeader.compressedSize == 0xFFFFFFFF ||
                fileHeader.uncompressedSize == 0xFFFFFFFF) {
            fprintf(stderr, "ZIP64 entry %d not supported!", i);
            return JZ_ERR_UNSUPPORTED;
        }

        if(fileHeader.relativeOffsetOflocalHeader == 0xFFFFFFFF)
            return JZ_ERR_UNSUPPORTED;
        if(fileHeader.diskNumberStart != 0 ||
                fileHeader.relativeOffsetOflocalHeader >=
                endRecord->centralDirectoryOffset) {
            fprintf(stderr, "Entry %d local header is outside this disk!", i);
            return JZ_ERR_CORRUPT;
        }

        if((uint64_t)fileHeader.fileNameLength +
                fileHeader.extraFieldLength + fileHeader.fileCommentLength >
                remaining) {
            fprintf(stderr, "Entry %d exceeds central directory!", i);
            return JZ_ERR_CORRUPT;
        }

        if((filename = (char *)malloc(fileHeader.fileNameLength + 1)) == NULL) {
            fprintf(stderr, "Couldn't allocate filename %d!", i);
            return JZ_ERR;
        }

        if(zip->read(zip, filename, fileHeader.fileNameLength) <
                fileHeader.fileNameLength) {
            fprintf(stderr, "Couldn't read filename %d!", i);
            free(filename);
            return JZ_ERR;
        }
        remaining -= fileHeader.fileNameLength;

        filename[fileHeader.fileNameLength] = '\0'; // NULL terminate

        if(memchr(filename, '\0', fileHeader.fileNameLength) != NULL) {
            fprintf(stderr, "Embedded NUL in filename %d!", i);
            free(filename);
            return JZ_ERR_CORRUPT;
        }

        if(zip->seek(zip, fileHeader.extraFieldLength, SEEK_CUR) ||
                zip->seek(zip, fileHeader.fileCommentLength, SEEK_CUR)) {
            fprintf(stderr, "Couldn't skip extra field or file comment %d", i);
            free(filename);
            return JZ_ERR;
        }
        remaining -= (uint64_t)fileHeader.extraFieldLength +
            fileHeader.fileCommentLength;

        // Construct JZFileHeader from global file header
        header.versionNeededToExtract = fileHeader.versionNeededToExtract;
        header.generalPurposeBitFlag = fileHeader.generalPurposeBitFlag;
        header.compressionMethod = fileHeader.compressionMethod;
        header.lastModFileTime = fileHeader.lastModFileTime;
        header.lastModFileDate = fileHeader.lastModFileDate;
        header.crc32 = fileHeader.crc32;
        header.compressedSize = fileHeader.compressedSize;
        header.uncompressedSize = fileHeader.uncompressedSize;
        header.offset = fileHeader.relativeOffsetOflocalHeader;

        keepGoing = callback(zip, i, &header, filename, user_data);
        free(filename);
        if(!keepGoing)
            break; // end if callback returns zero
    }

    if(i == endRecord->numEntries && remaining != 0) {
        fprintf(stderr, "Central directory has trailing data!");
        return JZ_ERR_CORRUPT;
    }

    return JZ_OK;
}

// Read local ZIP file header. Silent on errors so optimistic reading possible.
int jzReadLocalFileHeaderRaw(JZFile *zip, JZLocalFileHeader *header,
        char *filename, int len) {

    if(zip == NULL || header == NULL || zip->read == NULL ||
            zip->seek == NULL || len < 0 ||
            (len > 0 && filename == NULL))
        return JZ_ERR;

    if(zip->read(zip, header, sizeof(JZLocalFileHeader)) <
            sizeof(JZLocalFileHeader))
        return JZ_ERR;

    if(header->signature != 0x04034B50)
        return JZ_ERR_CORRUPT;

    if(header->generalPurposeBitFlag & 0x01)
        return JZ_ERR_UNSUPPORTED; // encrypted entries not supported

    if(header->versionNeededToExtract > 20)
        return JZ_ERR_UNSUPPORTED; // requires features this library lacks

    if(len) { // read filename
        if(header->fileNameLength >= len)
            return JZ_ERR_SIZE; // filename cannot fit

        if(zip->read(zip, filename, header->fileNameLength) <
                header->fileNameLength)
            return JZ_ERR; // read fail

        if(memchr(filename, '\0', header->fileNameLength) != NULL)
            return JZ_ERR_CORRUPT;

        filename[header->fileNameLength] = '\0'; // NULL terminate
    } else { // skip filename
        if(zip->seek(zip, header->fileNameLength, SEEK_CUR))
            return JZ_ERR;
    }

    if(header->extraFieldLength) {
        if(zip->seek(zip, header->extraFieldLength, SEEK_CUR))
            return JZ_ERR;
    }

    if(header->compressedSize == 0xFFFFFFFF ||
            header->uncompressedSize == 0xFFFFFFFF)
        return JZ_ERR_UNSUPPORTED; // ZIP64 not supported

    if(header->compressionMethod == 0 &&
            (header->compressedSize != header->uncompressedSize))
        return JZ_ERR_CORRUPT; // Method is "store" but sizes indicate otherwise

    return JZ_OK;
}

int jzReadLocalFileHeader(JZFile *zip, JZFileHeader *header,
        char *filename, int len) {
    JZLocalFileHeader localHeader;
    int ret;

    if(header == NULL)
        return JZ_ERR;

    if((ret = jzReadLocalFileHeaderRaw(zip, &localHeader, filename, len)) != JZ_OK)
        return ret;

    if(localHeader.versionNeededToExtract != header->versionNeededToExtract ||
            localHeader.generalPurposeBitFlag != header->generalPurposeBitFlag ||
            localHeader.compressionMethod != header->compressionMethod)
        return JZ_ERR_CORRUPT;

    if(localHeader.generalPurposeBitFlag & 0x08) {
        // A producer may repeat central values in the local header, but a
        // non-zero field must not contradict them. Normally these fields are
        // zero because the descriptor carries the values.
        if((localHeader.crc32 != 0 && localHeader.crc32 != header->crc32) ||
                (localHeader.compressedSize != 0 &&
                 localHeader.compressedSize != header->compressedSize) ||
                (localHeader.uncompressedSize != 0 &&
                 localHeader.uncompressedSize != header->uncompressedSize))
            return JZ_ERR_CORRUPT;
    } else if(localHeader.crc32 != header->crc32 ||
            localHeader.compressedSize != header->compressedSize ||
            localHeader.uncompressedSize != header->uncompressedSize) {
        return JZ_ERR_CORRUPT;
    }

    // Keep central-directory security values authoritative. Only timestamps
    // are copied from the local header for callers that display them.
    header->lastModFileTime = localHeader.lastModFileTime;
    header->lastModFileDate = localHeader.lastModFileDate;

    return JZ_OK;
}

// Read and verify a trailing data descriptor (bit 3). The descriptor is
// treated purely as a trailer to check: it must agree with the header values
// that bounded the read. The caller's header is never mutated, so sizes it
// holds always describe the data actually produced.
static int jzVerifyDataDescriptor(JZFile *zip, JZFileHeader *header) {
    uint32_t sigOrCrc, crc32, compressedSize, uncompressedSize;

    if(zip->read(zip, &sigOrCrc, sizeof(uint32_t)) < sizeof(uint32_t))
        return JZ_ERR;

    if(sigOrCrc == 0x08074B50) { // signature present
        if(zip->read(zip, &crc32, sizeof(uint32_t)) < sizeof(uint32_t))
            return JZ_ERR;
    } else {
        crc32 = sigOrCrc; // signature absent, first field was CRC
    }

    if(zip->read(zip, &compressedSize, sizeof(uint32_t)) < sizeof(uint32_t))
        return JZ_ERR;

    if(zip->read(zip, &uncompressedSize, sizeof(uint32_t)) < sizeof(uint32_t))
        return JZ_ERR;

    if(crc32 != header->crc32 ||
            compressedSize != header->compressedSize ||
            uncompressedSize != header->uncompressedSize)
        return JZ_ERR_CORRUPT; // descriptor disagrees with the values used

    return JZ_OK;
}

// Read data from file stream, described by header, to preallocated buffer.
// See junzip.h for the full security contract.
int jzReadData(JZFile *zip, JZFileHeader *header, void *buffer,
        size_t bufferSize, uint32_t *bytesProduced) {
    int hasDataDescriptor;

    if(zip == NULL || header == NULL || bytesProduced == NULL ||
            zip->read == NULL || zip->error == NULL ||
            (buffer == NULL && bufferSize > 0))
        return JZ_ERR;

    hasDataDescriptor = header->generalPurposeBitFlag & 0x08;

    *bytesProduced = 0;

    if(header->generalPurposeBitFlag & 0x01)
        return JZ_ERR_UNSUPPORTED; // encrypted entries not supported

    if(header->compressedSize == 0xFFFFFFFF ||
            header->uncompressedSize == 0xFFFFFFFF)
        return JZ_ERR_UNSUPPORTED; // ZIP64 not supported

    // Hard bound: never produce more than the caller's buffer holds.
    if(header->uncompressedSize > bufferSize)
        return JZ_ERR_SIZE;

    if(header->compressionMethod == 0) { // Store - just read it
        if(header->compressedSize != header->uncompressedSize)
            return JZ_ERR_CORRUPT; // stored entries must have equal sizes

        if(header->uncompressedSize > 0 &&
                (zip->read(zip, buffer, header->uncompressedSize) <
                 header->uncompressedSize || zip->error(zip)))
            return JZ_ERR;

        *bytesProduced = header->uncompressedSize;
    } else if(header->compressionMethod == 8) { // Deflate
        if(header->compressedSize == 0)
            return JZ_ERR_CORRUPT; // unknown size; pre-fill from central dir
#ifdef HAVE_ZLIB
        z_stream strm;
        unsigned char inbuf[16384]; // bounded input chunks, never sized by archive
        unsigned char dummyOut; // zlib rejects next_out == NULL even with 0 output
        uint32_t remaining = header->compressedSize;
        int zret = Z_OK;

        memset(&strm, 0, sizeof(strm));

        // Use inflateInit2 with negative window bits to indicate raw data
        if(inflateInit2(&strm, -MAX_WBITS) != Z_OK)
            return JZ_ERR;

        strm.next_out = header->uncompressedSize > 0 ?
            (unsigned char *)buffer : &dummyOut;
        strm.avail_out = header->uncompressedSize;

        while(remaining > 0) {
            size_t chunk = remaining < sizeof(inbuf) ?
                (size_t)remaining : sizeof(inbuf);

            if(zip->read(zip, inbuf, chunk) < chunk || zip->error(zip)) {
                inflateEnd(&strm);
                return JZ_ERR;
            }
            remaining -= (uint32_t)chunk;

            strm.next_in = inbuf;
            strm.avail_in = (uInt)chunk;

            zret = inflate(&strm, Z_NO_FLUSH);
            if(zret == Z_STREAM_END)
                break;
            if(zret != Z_OK ||
                    (strm.avail_out == 0 && header->uncompressedSize > 0)) {
                // corrupt stream, or more output than claimed
                inflateEnd(&strm);
                return JZ_ERR_CORRUPT;
            }
        }

        // The stream must terminate exactly at the declared sizes: no
        // truncation, no trailing garbage, no partial output.
        if(zret != Z_STREAM_END ||
                strm.total_out != header->uncompressedSize ||
                strm.total_in != header->compressedSize) {
            inflateEnd(&strm);
            return JZ_ERR_CORRUPT;
        }

        inflateEnd(&strm);
        *bytesProduced = header->uncompressedSize;
#else
#ifdef HAVE_PUFF
        unsigned long destlen, sourcelen;
        unsigned char *comp;
        uint64_t maxCompressed;
        int ret;

        // puff() needs the whole input in memory, so reject implausible
        // sizes before performing an archive-controlled allocation. A valid
        // deflate stream producing N bytes costs at most 15 bits per literal
        // plus a dynamic block descriptor of a few hundred bytes, i.e. well
        // under 2N + 1024; anything larger is guaranteed garbage.
        maxCompressed = (uint64_t)header->uncompressedSize * 2 + 1024;
        if(header->compressedSize > maxCompressed)
            return JZ_ERR_CORRUPT;

        comp = (unsigned char *)malloc(header->compressedSize);
        if(comp == NULL)
            return JZ_ERR;

        if(zip->read(zip, comp, header->compressedSize) <
                header->compressedSize || zip->error(zip)) {
            free(comp);
            return JZ_ERR;
        }

        destlen = header->uncompressedSize;
        sourcelen = header->compressedSize;
        ret = puff((unsigned char *)buffer, &destlen, comp, &sourcelen);
        free(comp);

        if(ret != 0 || destlen != header->uncompressedSize ||
                sourcelen != header->compressedSize)
            return JZ_ERR_CORRUPT; // bad stream, wrong output size, trailing data

        *bytesProduced = header->uncompressedSize;
#else
        return JZ_ERR_UNSUPPORTED; // no decompression backend compiled in
#endif // HAVE_PUFF
#endif
    } else {
        return JZ_ERR_UNSUPPORTED; // unknown compression method
    }

    if(hasDataDescriptor) {
        if(jzVerifyDataDescriptor(zip, header) != JZ_OK)
            return JZ_ERR_CORRUPT;
    }

    // Integrity: produced bytes must match the CRC from the central
    // directory (or local header / verified data descriptor).
    if(jz_crc32(0, buffer, *bytesProduced) != header->crc32)
        return JZ_ERR_CORRUPT;

    return JZ_OK;
}


typedef struct {
    JZFile handle;
    FILE *fp;
} StdioJZFile;

static size_t
stdio_read_file_handle_read(JZFile *file, void *buf, size_t size)
{
    StdioJZFile *handle = (StdioJZFile *)file;
    return fread(buf, 1, size, handle->fp);
}

static size_t
stdio_read_file_handle_tell(JZFile *file)
{
    StdioJZFile *handle = (StdioJZFile *)file;
    long position = ftell(handle->fp);
    return position < 0 ? SIZE_MAX : (size_t)position;
}

static int
stdio_read_file_handle_seek(JZFile *file, size_t offset, int whence)
{
    StdioJZFile *handle = (StdioJZFile *)file;
    if(offset > (size_t)LONG_MAX)
        return -1;
    return fseek(handle->fp, (long)offset, whence);
}

static int
stdio_read_file_handle_error(JZFile *file)
{
    StdioJZFile *handle = (StdioJZFile *)file;
    return ferror(handle->fp);
}

static void
stdio_read_file_handle_close(JZFile *file)
{
    StdioJZFile *handle = (StdioJZFile *)file;
    fclose(handle->fp);
    free(file);
}

JZFile *
jzfile_from_stdio_file(FILE *fp)
{
    StdioJZFile *handle = (StdioJZFile *)malloc(sizeof(StdioJZFile));

    if(handle == NULL) return NULL;

    handle->handle.read = stdio_read_file_handle_read;
    handle->handle.tell = stdio_read_file_handle_tell;
    handle->handle.seek = stdio_read_file_handle_seek;
    handle->handle.error = stdio_read_file_handle_error;
    handle->handle.close = stdio_read_file_handle_close;
    handle->fp = fp;

    return &(handle->handle);
}
