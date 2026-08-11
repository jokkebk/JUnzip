// Optional libFuzzer harness for the parser and extraction contract.
// Build with `make fuzz` on a compiler that provides -fsanitize=fuzzer.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "junzip.h"

typedef struct {
    JZFile handle;
    const unsigned char *data;
    size_t size;
    size_t position;
} FuzzFile;

static size_t fuzz_read(JZFile *file, void *buffer, size_t size) {
    FuzzFile *fuzz = (FuzzFile *)file;
    size_t available = fuzz->position < fuzz->size ?
            fuzz->size - fuzz->position : 0;
    size_t count = size < available ? size : available;

    if(count > 0)
        memcpy(buffer, fuzz->data + fuzz->position, count);
    fuzz->position += count;
    return count;
}

static int fuzz_tell(JZFile *file, JZOffset *offset) {
    if(offset == NULL)
        return -1;
    *offset = ((FuzzFile *)file)->position;
    return 0;
}

static int fuzz_seek(JZFile *file, int64_t offset, int whence) {
    FuzzFile *fuzz = (FuzzFile *)file;
    int64_t base;
    int64_t position;

    if(whence == SEEK_SET)
        base = 0;
    else if(whence == SEEK_CUR)
        base = (int64_t)fuzz->position;
    else if(whence == SEEK_END)
        base = (int64_t)fuzz->size;
    else
        return -1;

    if((offset > 0 && base > INT64_MAX - offset) ||
            (offset < 0 && base < INT64_MIN - offset))
        return -1;
    position = base + offset;
    if(position < 0 || (uint64_t)position > SIZE_MAX)
        return -1;
    fuzz->position = (size_t)position;
    return 0;
}

static int fuzz_error(JZFile *file) {
    (void)file;
    return 0;
}

static void fuzz_close(JZFile *file) {
    (void)file;
}

static int fuzz_callback(JZFile *zip, int index, JZFileHeader *header,
        char *filename, void *user_data) {
    FuzzFile *fuzz = (FuzzFile *)zip;
    JZOffset centralPosition = fuzz->position;
    unsigned char *output = NULL;
    uint32_t produced;
    uint32_t maxOutput = 1024 * 1024;

    (void)index;
    (void)user_data;

    // The harness must never turn an archive claim into an unbounded fuzzing
    // allocation. The production API still enforces the caller's exact bound.
    if(header->uncompressedSize > maxOutput ||
            header->compressedSize > maxOutput ||
            header->offset > fuzz->size ||
            zip->seek(zip, (int64_t)header->offset, SEEK_SET))
        goto done;

    if(jzReadLocalFileHeaderChecked(zip, header, filename) != JZ_OK)
        goto done;

    if(header->uncompressedSize > 0) {
        output = (unsigned char *)malloc(header->uncompressedSize);
        if(output == NULL)
            goto done;
    }
    (void)jzReadDataLimited(zip, header, output, header->uncompressedSize,
            maxOutput, &produced);

done:
    free(output);
    if(centralPosition <= INT64_MAX)
        (void)zip->seek(zip, (int64_t)centralPosition, SEEK_SET);
    return 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    FuzzFile fuzz;
    JZEndRecord endRecord;

    memset(&fuzz, 0, sizeof(fuzz));
    fuzz.handle.read = fuzz_read;
    fuzz.handle.tell = fuzz_tell;
    fuzz.handle.seek = fuzz_seek;
    fuzz.handle.error = fuzz_error;
    fuzz.handle.close = fuzz_close;
    fuzz.data = data;
    fuzz.size = size;

    if(jzReadEndRecord(&fuzz.handle, &endRecord) == JZ_OK)
        (void)jzReadCentralDirectory(&fuzz.handle, &endRecord,
                fuzz_callback, NULL);
    return 0;
}

#ifdef JUNZIP_FUZZ_STANDALONE
int main(int argc, char **argv) {
    FILE *input;
    unsigned char *seed;
    unsigned char *mutated;
    long length;
    size_t size;
    int iteration;

    if(argc != 2)
        return 2;
    input = fopen(argv[1], "rb");
    if(input == NULL || fseek(input, 0, SEEK_END) != 0)
        return 2;
    length = ftell(input);
    if(length < 0 || fseek(input, 0, SEEK_SET) != 0) {
        fclose(input);
        return 2;
    }
    size = (size_t)length;
    seed = size == 0 ? NULL : (unsigned char *)malloc(size);
    mutated = size == 0 ? NULL : (unsigned char *)malloc(size);
    if((size > 0 && (seed == NULL || mutated == NULL)) ||
            (size > 0 && fread(seed, 1, size, input) != size)) {
        free(seed);
        free(mutated);
        fclose(input);
        return 2;
    }
    fclose(input);

    for(iteration = 0; iteration < 400; iteration++) {
        size_t mutatedSize = size;
        if(size > 0) {
            size_t index = (size_t)iteration % size;
            memcpy(mutated, seed, size);
            mutated[index] ^= (unsigned char)(1U << (iteration % 8));
            if((iteration % 3) == 0)
                mutatedSize = size - ((size_t)iteration % (size + 1));
        }
        LLVMFuzzerTestOneInput(size > 0 ? mutated : seed, mutatedSize);
    }

    free(seed);
    free(mutated);
    return 0;
}
#endif
