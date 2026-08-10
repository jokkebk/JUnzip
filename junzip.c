// JUnzip library by Joonas Pihlajamaa. See junzip.h for license and details.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "junzip.h"

// Global shared buffer. Note: this makes the library non-thread-safe and
// non-reentrant. The filename pointer passed to JZRecordCallback also points
// into this buffer and is only valid until the next library call.
unsigned char jzBuffer[JZ_BUFFER_SIZE];

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

// Read ZIP file end record. Will move within file.
int jzReadEndRecord(JZFile *zip, JZEndRecord *endRecord) {
    long fileSize, readBytes, i;
    JZEndRecord *er;

    if(zip->seek(zip, 0, SEEK_END)) {
        fprintf(stderr, "Couldn't go to end of zip file!");
        return Z_ERRNO;
    }

    if((fileSize = zip->tell(zip)) <= sizeof(JZEndRecord)) {
        fprintf(stderr, "Too small file to be a zip!");
        return Z_ERRNO;
    }

    readBytes = (fileSize < sizeof(jzBuffer)) ? fileSize : sizeof(jzBuffer);

    if(zip->seek(zip, fileSize - readBytes, SEEK_SET)) {
        fprintf(stderr, "Cannot seek in zip file!");
        return Z_ERRNO;
    }

    if(zip->read(zip, jzBuffer, readBytes) < readBytes) {
        fprintf(stderr, "Couldn't read end of zip file!");
        return Z_ERRNO;
    }

    // Scan backwards for end record signature, validating that the candidate
    // sits at exactly fileSize - sizeof(JZEndRecord) - zipCommentLength.
    // This rejects false positives from signatures embedded in file data or
    // ZIP comments.
    for(i = readBytes - sizeof(JZEndRecord); i >= 0; i--) {
        er = (JZEndRecord *)(jzBuffer + i);
        if(er->signature == 0x06054B50) {
            long candidatePos = fileSize - readBytes + i;
            if(candidatePos + sizeof(JZEndRecord) + er->zipCommentLength == fileSize)
                break;
        }
    }

    if(i < 0) {
        fprintf(stderr, "End record signature not found in zip!");
        return Z_ERRNO;
    }

    memcpy(endRecord, er, sizeof(JZEndRecord));

    if(endRecord->diskNumber || endRecord->centralDirectoryDiskNumber ||
            endRecord->numEntries != endRecord->numEntriesThisDisk) {
        fprintf(stderr, "Multifile zips not supported!");
        return Z_ERRNO;
    }

    if(endRecord->centralDirectoryOffset + endRecord->centralDirectorySize > fileSize) {
        fprintf(stderr, "Central directory offset/size exceeds file size!");
        return Z_ERRNO;
    }

    return Z_OK;
}

// Read ZIP file global directory. Will move within file.
int jzReadCentralDirectory(JZFile *zip, JZEndRecord *endRecord,
        JZRecordCallback callback, void *user_data) {
    JZGlobalFileHeader fileHeader;
    JZFileHeader header;
    int i;

    if(zip->seek(zip, endRecord->centralDirectoryOffset, SEEK_SET)) {
        fprintf(stderr, "Cannot seek in zip file!");
        return Z_ERRNO;
    }

    for(i=0; i<endRecord->numEntries; i++) {
        if(zip->read(zip, &fileHeader, sizeof(JZGlobalFileHeader)) <
                sizeof(JZGlobalFileHeader)) {
            fprintf(stderr, "Couldn't read file header %d!", i);
            return Z_ERRNO;
        }

        if(fileHeader.signature != 0x02014B50) {
            fprintf(stderr, "Invalid file header signature %d!", i);
            return Z_ERRNO;
        }

        if(fileHeader.fileNameLength + 1 >= JZ_BUFFER_SIZE) {
            fprintf(stderr, "Too long file name %d!", i);
            return Z_ERRNO;
        }

        if(zip->read(zip, jzBuffer, fileHeader.fileNameLength) <
                fileHeader.fileNameLength) {
            fprintf(stderr, "Couldn't read filename %d!", i);
            return Z_ERRNO;
        }

        jzBuffer[fileHeader.fileNameLength] = '\0'; // NULL terminate

        if(zip->seek(zip, fileHeader.extraFieldLength, SEEK_CUR) ||
                zip->seek(zip, fileHeader.fileCommentLength, SEEK_CUR)) {
            fprintf(stderr, "Couldn't skip extra field or file comment %d", i);
            return Z_ERRNO;
        }

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

        if(!callback(zip, i, &header, (char *)jzBuffer, user_data))
            break; // end if callback returns zero
    }

    return Z_OK;
}

// Read local ZIP file header. Silent on errors so optimistic reading possible.
int jzReadLocalFileHeaderRaw(JZFile *zip, JZLocalFileHeader *header,
        char *filename, int len) {

    if(zip->read(zip, header, sizeof(JZLocalFileHeader)) <
            sizeof(JZLocalFileHeader))
        return Z_ERRNO;

    if(header->signature != 0x04034B50)
        return Z_ERRNO;

    if(len) { // read filename
        if(header->fileNameLength >= len)
            return Z_ERRNO; // filename cannot fit

        if(zip->read(zip, filename, header->fileNameLength) <
                header->fileNameLength)
            return Z_ERRNO; // read fail

        filename[header->fileNameLength] = '\0'; // NULL terminate
    } else { // skip filename
        if(zip->seek(zip, header->fileNameLength, SEEK_CUR))
            return Z_ERRNO;
    }

    if(header->extraFieldLength) {
        if(zip->seek(zip, header->extraFieldLength, SEEK_CUR))
            return Z_ERRNO;
    }

    // For now, silently ignore bit flags and hope ZLIB can uncompress
    // if(header->generalPurposeBitFlag)
    //     return Z_ERRNO; // Flags not supported

    if(header->compressedSize == 0xFFFFFFFF ||
            header->uncompressedSize == 0xFFFFFFFF)
        return JZ_ERR_UNSUPPORTED; // ZIP64 not supported

    if(header->compressionMethod == 0 &&
            (header->compressedSize != header->uncompressedSize))
        return JZ_ERR_CORRUPT; // Method is "store" but sizes indicate otherwise

    return Z_OK;
}

int jzReadLocalFileHeader(JZFile *zip, JZFileHeader *header,
        char *filename, int len) {
    JZLocalFileHeader localHeader;
    int ret;

    if((ret = jzReadLocalFileHeaderRaw(zip, &localHeader, filename, len)) != Z_OK)
        return ret;

    header->versionNeededToExtract = localHeader.versionNeededToExtract;
    header->generalPurposeBitFlag = localHeader.generalPurposeBitFlag;
    header->compressionMethod = localHeader.compressionMethod;
    header->lastModFileTime = localHeader.lastModFileTime;
    header->lastModFileDate = localHeader.lastModFileDate;

    // When bit 3 is set, the local header carries no sizes/CRC (they follow
    // the file data in a data descriptor); keep the caller-provided central
    // directory values. Otherwise the local header is authoritative.
    if(!(localHeader.generalPurposeBitFlag & 0x08)) {
        header->crc32 = localHeader.crc32;
        header->compressedSize = localHeader.compressedSize;
        header->uncompressedSize = localHeader.uncompressedSize;
    }

    header->offset = 0; // not used in local context

    return Z_OK;
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
    int hasDataDescriptor = header->generalPurposeBitFlag & 0x08;

    if(bytesProduced == NULL || (buffer == NULL && bufferSize > 0))
        return JZ_ERR;

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
        uint32_t remaining = header->compressedSize;
        int zret = Z_OK;

        memset(&strm, 0, sizeof(strm));

        // Use inflateInit2 with negative window bits to indicate raw data
        if(inflateInit2(&strm, -MAX_WBITS) != Z_OK)
            return JZ_ERR;

        strm.next_out = (unsigned char *)buffer;
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
            if(zret != Z_OK || strm.avail_out == 0) {
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
    return ftell(handle->fp);
}

static int
stdio_read_file_handle_seek(JZFile *file, size_t offset, int whence)
{
    StdioJZFile *handle = (StdioJZFile *)file;
    return fseek(handle->fp, offset, whence);
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
