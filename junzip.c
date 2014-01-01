// JUnzip library by Joonas Pihlajamaa. See junzip.h for license and details.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "junzip.h"

unsigned char jzBuffer[JZ_BUFFER_SIZE]; // limits maximum zip descriptor size

// Read ZIP file end record. Will move within file.
int jzReadEndRecord(FILE *zip, JZEndRecord *endRecord) {
    long fileSize, readBytes, i;
    JZEndRecord *er;

    if(fseek(zip, 0, SEEK_END)) {
        fprintf(stderr, "Couldn't go to end of zip file!");
        return Z_ERRNO;
    }

    if((fileSize = ftell(zip)) <= sizeof(JZEndRecord)) {
        fprintf(stderr, "Too small file to be a zip!");
        return Z_ERRNO;
    }

    readBytes = (fileSize < sizeof(jzBuffer)) ? fileSize : sizeof(jzBuffer);

    if(fseek(zip, fileSize - readBytes, SEEK_SET)) {
        fprintf(stderr, "Cannot seek in zip file!");
        return Z_ERRNO;
    }

    if(fread(jzBuffer, 1, readBytes, zip) < readBytes) {
        fprintf(stderr, "Couldn't read end of zip file!");
        return Z_ERRNO;
    }

    // Naively assume signature can only be found in one place...
    for(i = readBytes - sizeof(JZEndRecord); i >= 0; i--) {
        er = (JZEndRecord *)(jzBuffer + i);
        if(er->signature == 0x06054B50)
            break;
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

    return Z_OK;
}

// Read ZIP file global directory. Will move within file.
int jzReadCentralDirectory(FILE *zip, JZEndRecord *endRecord,
        JZRecordCallback callback) {
    JZGlobalFileHeader fileHeader;
    JZFileHeader header;
    long totalSize = 0;
    int i;

    if(fseek(zip, endRecord->centralDirectoryOffset, SEEK_SET)) {
        fprintf(stderr, "Cannot seek in zip file!");
        return Z_ERRNO;
    }

    for(i=0; i<endRecord->numEntries; i++) {
        if(fread(&fileHeader, 1, sizeof(JZGlobalFileHeader), zip) <
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

        if(fread(jzBuffer, 1, fileHeader.fileNameLength, zip) <
                fileHeader.fileNameLength) {
            fprintf(stderr, "Couldn't read filename %d!", i);
            return Z_ERRNO;
        }

        jzBuffer[fileHeader.fileNameLength] = '\0'; // NULL terminate

        if(fseek(zip, fileHeader.extraFieldLength, SEEK_CUR) ||
                fseek(zip, fileHeader.fileCommentLength, SEEK_CUR)) {
            fprintf(stderr, "Couldn't skip extra field or file comment %d", i);
            return Z_ERRNO;
        }

        // Construct JZFileHeader from global file header
        memcpy(&header, &fileHeader.compressionMethod, sizeof(header));
        header.offset = fileHeader.relativeOffsetOflocalHeader;

        totalSize += fileHeader.uncompressedSize;

        if(!callback(zip, i, &header, (char *)jzBuffer))
            break; // end if callback returns zero
    }

    return Z_OK;
}

// Read local ZIP file header. Silent on errors so optimistic reading possible.
int jzReadLocalFileHeader(FILE *zip, JZFileHeader *header,
        char *filename, int len) {
    JZLocalFileHeader localHeader;

    if(fread(&localHeader, 1, sizeof(JZLocalFileHeader), zip) <
            sizeof(JZLocalFileHeader))
        return Z_ERRNO;

    if(localHeader.signature != 0x04034B50)
        return Z_ERRNO;

    if(len) { // read filename
        if(localHeader.fileNameLength >= len)
            return Z_ERRNO; // filename cannot fit

        if(fread(filename, 1, localHeader.fileNameLength, zip) <
                localHeader.fileNameLength)
            return Z_ERRNO; // read fail

        filename[localHeader.fileNameLength] = '\0'; // NULL terminate
    } else { // skip filename
        if(fseek(zip, localHeader.fileNameLength, SEEK_CUR))
            return Z_ERRNO;
    }

    if(localHeader.extraFieldLength) {
        if(fseek(zip, localHeader.extraFieldLength, SEEK_CUR))
            return Z_ERRNO;
    }

    if(localHeader.generalPurposeBitFlag)
        return Z_ERRNO; // Flags not supported

    if(localHeader.compressionMethod == 0 &&
            (localHeader.compressedSize != localHeader.uncompressedSize))
        return Z_ERRNO; // Method is "store" but sizes indicate otherwise, abort

    memcpy(header, &localHeader.compressionMethod, sizeof(JZFileHeader));
    header->offset = 0; // not used in local context

    return Z_OK;
}

// Read data from file stream, described by header, to preallocated buffer
int jzReadData(FILE *zip, JZFileHeader *header, void *buffer) {
    unsigned char *bytes = (unsigned char *)buffer; // cast
    long compressedLeft, uncompressedLeft;
    z_stream strm;
    int ret;

    if(header->compressionMethod == 0) { // Store - just read it
        if(fread(buffer, 1, header->uncompressedSize, zip) <
                header->uncompressedSize || ferror(zip))
            return Z_ERRNO;
    } else if(header->compressionMethod == 8) { // Deflate - using zlib
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;

        strm.avail_in = 0;
        strm.next_in = Z_NULL;

        // Use inflateInit2 with negative window bits to indicate raw data
        if((ret = inflateInit2(&strm, -MAX_WBITS)) != Z_OK)
            return ret; // Zlib errors are negative

        // Inflate compressed data
        for(compressedLeft = header->compressedSize,
                uncompressedLeft = header->uncompressedSize;
                compressedLeft && uncompressedLeft && ret != Z_STREAM_END;
                compressedLeft -= strm.avail_in) {
            // Read next chunk
            strm.avail_in = fread(jzBuffer, 1,
                    (sizeof(jzBuffer) < compressedLeft) ?
                    sizeof(jzBuffer) : compressedLeft, zip);

            if(strm.avail_in == 0 || ferror(zip)) {
                inflateEnd(&strm);
                return Z_ERRNO;
            }

            strm.next_in = jzBuffer;
            strm.avail_out = uncompressedLeft;
            strm.next_out = bytes;

            compressedLeft -= strm.avail_in; // inflate will change avail_in

            ret = inflate(&strm, Z_NO_FLUSH);

            if(ret == Z_STREAM_ERROR) return ret; // shouldn't happen

            switch (ret) {
                case Z_NEED_DICT:
                    ret = Z_DATA_ERROR;     /* and fall through */
                case Z_DATA_ERROR: case Z_MEM_ERROR:
                    (void)inflateEnd(&strm);
                    return ret;
            }

            bytes += uncompressedLeft - strm.avail_out; // bytes uncompressed
            uncompressedLeft = strm.avail_out;
        }

        inflateEnd(&strm);
    } else {
        return Z_ERRNO;
    }

    return Z_OK;
}
