// Simple naive ZIP dump utility useful for debugging zips. Should compile with
// something like gcc junzip_dump.c junzip.c -o junzip_dump.exe

#include <stdio.h>
#include <assert.h>

#include "junzip.h"

int main(int argc, char *argv[]) {
    FILE *fp;
    JZFile *zip;
    JZGlobalFileHeader jzGlobalHeader;
    JZEndRecord jzEndRecord;
    long fpos, fsize;
    int i;

    if(argc < 2) {
        puts("Usage: junzip_dump file.zip");
        return 0;
    }

    if(!(fp = fopen(argv[1], "rb"))) {
        printf("Couldn't open \"%s\"!", argv[1]);
        return -1;
    }

    zip = jzfile_from_stdio_file(fp);

    if(jzReadEndRecord(zip, &jzEndRecord)) {
        printf("Couldn't read ZIP file end record.\n");
        goto endClose;
    }

    puts("Reading central directory and local headers...");

    if(zip->seek(zip, jzEndRecord.centralDirectoryOffset, SEEK_SET)) {
        fprintf(stderr, "Cannot seek to central directory!\n");
        goto endClose;
    }

    for(i = 0; i < jzEndRecord.numEntries; i++) {
        long centralPos = zip->tell(zip);
        JZLocalFileHeader jzHeader;
        char filename[640]; // should be enough for everyone

        printf("[%08X] ", (int)centralPos);

        if(zip->read(zip, &jzGlobalHeader, sizeof(JZGlobalFileHeader)) <
                sizeof(JZGlobalFileHeader)) {
            fprintf(stderr, "Couldn't read file header %d!", i);
            return Z_ERRNO;
        }

        if(jzGlobalHeader.signature != 0x02014B50) {
            fprintf(stderr, "Invalid file header signature %d!", i);
            return Z_ERRNO;
        }

        if(jzGlobalHeader.fileNameLength + 1 >= sizeof(filename)) {
            fprintf(stderr, "Too long file name %d!", i);
            return Z_ERRNO;
        }

        if(zip->read(zip, filename, jzGlobalHeader.fileNameLength) <
                jzGlobalHeader.fileNameLength) {
            fprintf(stderr, "Couldn't read filename %d!", i);
            return Z_ERRNO;
        }

        filename[jzGlobalHeader.fileNameLength] = '\0'; // NULL terminate

        if(zip->seek(zip, jzGlobalHeader.extraFieldLength, SEEK_CUR) ||
                zip->seek(zip, jzGlobalHeader.fileCommentLength, SEEK_CUR)) {
            fprintf(stderr, "Couldn't skip extra field or file comment %d", i);
            return Z_ERRNO;
        }

        // Read the corresponding local header via its relative offset
        fpos = zip->tell(zip); // store current central dir position

        if(zip->seek(zip, jzGlobalHeader.relativeOffsetOflocalHeader, SEEK_SET)) {
            fprintf(stderr, "Couldn't seek to local header %d!", i);
            return Z_ERRNO;
        }

        if(jzReadLocalFileHeaderRaw(zip, &jzHeader, filename, sizeof(filename)) != Z_OK) {
            fprintf(stderr, "Couldn't read local header %d!", i);
            return Z_ERRNO;
        }

        printf("Local file header for \"%s\" (%d compressed bytes, flag %d)\n",
                filename, jzHeader.compressedSize, jzHeader.generalPurposeBitFlag);
        printf("    signature: 0x%08X\n", jzHeader.signature);
        printf("    versionNeededToExtract: %d\n", jzHeader.versionNeededToExtract);
        printf("    generalPurposeBitFlag: %d\n", jzHeader.generalPurposeBitFlag);
        printf("    compressionMethod: %d\n", jzHeader.compressionMethod);
        printf("    lastModFileTime: 0x%02X (%02d:%02d:%02d)\n", jzHeader.lastModFileTime,
               JZHOUR(jzHeader.lastModFileTime),
               JZMINUTE(jzHeader.lastModFileTime),
               JZSECOND(jzHeader.lastModFileTime));
        printf("    lastModFileDate: 0x%02X (%04d/%02d/%02d)\n", jzHeader.lastModFileDate,
               JZYEAR(jzHeader.lastModFileDate),
               JZMONTH(jzHeader.lastModFileDate),
               JZDAY(jzHeader.lastModFileDate));
        printf("    crc32: 0x%08X\n", jzHeader.crc32);
        printf("    compressedSize: %d\n", jzHeader.compressedSize);
        printf("    uncompressedSize: %d\n", jzHeader.uncompressedSize);
        printf("    fileNameLength: %d\n", jzHeader.fileNameLength);
        printf("    extraFieldLength: %d\n", jzHeader.extraFieldLength);

        // Return to central directory to read next entry
        if(zip->seek(zip, fpos, SEEK_SET)) {
            fprintf(stderr, "Couldn't return to central directory %d!", i);
            return Z_ERRNO;
        }

        // Position printed earlier
        printf("Global file header for \"%s\" (%d compressed bytes)\n",
                filename, jzGlobalHeader.compressedSize);
        printf("    signature: 0x%08X\n", jzGlobalHeader.signature);
        printf("    versionMadeBy: %d\n", jzGlobalHeader.versionMadeBy);
        printf("    versionNeededToExtract: %d\n", jzGlobalHeader.versionNeededToExtract);
        printf("    generalPurposeBitFlag: %d\n", jzGlobalHeader.generalPurposeBitFlag);
        printf("    compressionMethod: %d\n", jzGlobalHeader.compressionMethod);
        printf("    lastModFileTime: %d (%02d:%02d:%02d)\n", jzGlobalHeader.lastModFileTime,
               JZHOUR(jzGlobalHeader.lastModFileTime),
               JZMINUTE(jzGlobalHeader.lastModFileTime),
               JZSECOND(jzGlobalHeader.lastModFileTime));
        printf("    lastModFileDate: %d (%04d/%02d/%02d)\n", jzGlobalHeader.lastModFileDate,
               JZYEAR(jzGlobalHeader.lastModFileDate),
               JZMONTH(jzGlobalHeader.lastModFileDate),
               JZDAY(jzGlobalHeader.lastModFileDate));
        printf("    crc32: 0x%08X\n", jzGlobalHeader.crc32);
        printf("    compressedSize: %d\n", jzGlobalHeader.compressedSize);
        printf("    uncompressedSize: %d\n", jzGlobalHeader.uncompressedSize);
        printf("    fileNameLength: %d\n", jzGlobalHeader.fileNameLength);
        printf("    extraFieldLength: %d\n", jzGlobalHeader.extraFieldLength);
        printf("    fileCommentLength: %d\n", jzGlobalHeader.fileCommentLength);
        printf("    diskNumberStart: %d\n", jzGlobalHeader.diskNumberStart);
        printf("    internalFileAttributes: 0x%04X\n", jzGlobalHeader.internalFileAttributes);
        printf("    externalFileAttributes: 0x%08X\n", jzGlobalHeader.externalFileAttributes);
        printf("    relativeOffsetOflocalHeader: [%08X]\n", jzGlobalHeader.relativeOffsetOflocalHeader);
    }

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
    printf("[%08X] End record read\n", (int)fpos);
    printf("    signature: 0x%08X\n", jzEndRecord.signature);
    printf("    diskNumber: %d\n", jzEndRecord.diskNumber);
    printf("    centralDirectoryDiskNumber: %d\n", jzEndRecord.centralDirectoryDiskNumber);
    printf("    numEntriesThisDisk: %d\n", jzEndRecord.numEntriesThisDisk);
    printf("    numEntries: %d\n", jzEndRecord.numEntries);
    printf("    centralDirectorySize: %d\n", jzEndRecord.centralDirectorySize);
    printf("    centralDirectoryOffset: [%08X]\n", jzEndRecord.centralDirectoryOffset);
    printf("    zipCommentLength: %d\n", jzEndRecord.zipCommentLength);

endClose:
    if(zip)
        zip->close(zip);

    return 0;
}
