// Security regression tests for JUnzip. All fixtures are built in memory and
// parsed through an in-memory JZFile: the suite never touches the filesystem
// and never allocates more than a few kilobytes, so a regression can fail a
// test but can never exhaust memory or fill a disk (e.g. a ZIP64 sentinel
// claim must be rejected, not acted upon).
//
// Build/run via "make check" (optionally with SANITIZE=1 and HAVE_PUFF=1 or
// HAVE_ZLIB=1 to cover both decompression backends).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "junzip.h"

static int tests_run = 0, tests_failed = 0;

#if defined(HAVE_ZLIB) || defined(HAVE_PUFF)
#define EXPECT_DEFLATE_SUCCESS 1
#else
#define EXPECT_DEFLATE_SUCCESS 0
#endif

#define EXPECT(cond) do { \
        tests_run++; \
        if(!(cond)) { \
            tests_failed++; \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while(0)

// ---------------------------------------------------------------------------
// In-memory JZFile
// ---------------------------------------------------------------------------

typedef struct {
    JZFile handle;
    const unsigned char *data;
    size_t size;
    size_t pos;
} MemJZFile;

static size_t mem_read(JZFile *f, void *buf, size_t size) {
    MemJZFile *m = (MemJZFile *)f;
    size_t avail = (m->pos < m->size) ? m->size - m->pos : 0;
    size_t n = (size < avail) ? size : avail;
    memcpy(buf, m->data + m->pos, n);
    m->pos += n;
    return n;
}

static int mem_tell(JZFile *f, JZOffset *offset) {
    if(offset == NULL)
        return -1;
    *offset = ((MemJZFile *)f)->pos;
    return 0;
}

static int mem_seek(JZFile *f, int64_t offset, int whence) {
    MemJZFile *m = (MemJZFile *)f;
    int64_t base =
        (whence == SEEK_SET) ? 0 :
        (whence == SEEK_CUR) ? (int64_t)m->pos :
        (whence == SEEK_END) ? (int64_t)m->size : 0;
    int64_t newpos;

    if(whence < SEEK_SET || whence > SEEK_END ||
            (offset > 0 && base > INT64_MAX - offset) ||
            (offset < 0 && base < INT64_MIN - offset))
        return -1; // mimic fseek failing on absurd positions
    newpos = base + offset;
    if(newpos < 0 || (uint64_t)newpos > 0x7FFFFFFFULL)
        return -1;
    m->pos = (size_t)newpos; // seeking beyond end is allowed, reads then fail
    return 0;
}

static int mem_error(JZFile *f) {
    (void)f;
    return 0;
}

static void mem_close(JZFile *f) {
    (void)f;
}

static void mem_init(MemJZFile *m, const void *data, size_t size) {
    m->handle.read = mem_read;
    m->handle.tell = mem_tell;
    m->handle.seek = mem_seek;
    m->handle.error = mem_error;
    m->handle.close = mem_close;
    m->data = (const unsigned char *)data;
    m->size = size;
    m->pos = 0;
}

// ---------------------------------------------------------------------------
// ZIP fixture builder
// ---------------------------------------------------------------------------

#define ZB_CAPACITY (256 * 1024)

typedef struct {
    unsigned char bytes[ZB_CAPACITY];
    size_t len;
} ZipBuf;

static void zb_reset(ZipBuf *z) {
    z->len = 0;
}

static size_t zb_pos(ZipBuf *z) {
    return z->len;
}

static void zb_put(ZipBuf *z, const void *p, size_t n) {
    if(z->len + n > ZB_CAPACITY) {
        fprintf(stderr, "test fixture overflow, increase ZB_CAPACITY\n");
        exit(2);
    }
    memcpy(z->bytes + z->len, p, n);
    z->len += n;
}

static void zb_local(ZipBuf *z, const char *name, uint16_t version,
        uint16_t flags, uint16_t method, uint32_t crc, uint32_t csize,
        uint32_t usize) {
    JZLocalFileHeader h;
    memset(&h, 0, sizeof(h));
    h.signature = 0x04034B50;
    h.versionNeededToExtract = version;
    h.generalPurposeBitFlag = flags;
    h.compressionMethod = method;
    h.crc32 = crc;
    h.compressedSize = csize;
    h.uncompressedSize = usize;
    h.fileNameLength = (uint16_t)strlen(name);
    zb_put(z, &h, sizeof(h));
    zb_put(z, name, strlen(name));
}

static void zb_central(ZipBuf *z, const char *name, uint16_t version,
        uint16_t flags, uint16_t method, uint32_t crc, uint32_t csize,
        uint32_t usize, uint32_t offset) {
    JZGlobalFileHeader h;
    memset(&h, 0, sizeof(h));
    h.signature = 0x02014B50;
    h.versionMadeBy = 20;
    h.versionNeededToExtract = version;
    h.generalPurposeBitFlag = flags;
    h.compressionMethod = method;
    h.crc32 = crc;
    h.compressedSize = csize;
    h.uncompressedSize = usize;
    h.fileNameLength = (uint16_t)strlen(name);
    h.relativeOffsetOflocalHeader = offset;
    zb_put(z, &h, sizeof(h));
    zb_put(z, name, strlen(name));
}

static void zb_endrecord(ZipBuf *z, uint16_t numEntries, uint32_t cdSize,
        uint32_t cdOffset, uint16_t commentLen) {
    JZEndRecord h;
    memset(&h, 0, sizeof(h));
    h.signature = 0x06054B50;
    h.numEntriesThisDisk = numEntries;
    h.numEntries = numEntries;
    h.centralDirectorySize = cdSize;
    h.centralDirectoryOffset = cdOffset;
    h.zipCommentLength = commentLen;
    zb_put(z, &h, sizeof(h));
    if(commentLen) {
        static unsigned char filler[65535];
        memset(filler, 'A', commentLen);
        zb_put(z, filler, commentLen);
    }
}

static void zb_datadesc(ZipBuf *z, uint32_t crc, uint32_t csize,
        uint32_t usize) {
    uint32_t fields[4] = { 0x08074B50, crc, csize, usize };
    zb_put(z, fields, sizeof(fields));
}

// ---------------------------------------------------------------------------
// Fixture data: "hello world, hello world, hello!" and its raw deflate form
// ---------------------------------------------------------------------------

static const unsigned char fix_data[] = "hello world, hello world, hello!";
#define FIX_LEN 32
#define FIX_CRC 0x7B97CEF8

static const unsigned char fix_deflate[] = {
    0xCB, 0x48, 0xCD, 0xC9, 0xC9, 0x57, 0x28, 0xCF, 0x2F, 0xCA,
    0x49, 0xD1, 0x51, 0xC8, 0xC0, 0xE0, 0x28, 0x02, 0x00
};
#define FIX_DEFLATE_LEN 19

// ---------------------------------------------------------------------------
// Helpers driving the library the way a typical consumer would
// ---------------------------------------------------------------------------

static JZFileHeader g_header; // captured from the central directory callback
static char g_name[1024];

static int capture_cb(JZFile *zip, int index, JZFileHeader *header,
        char *filename, void *user_data) {
    (void)zip; (void)index; (void)user_data;
    g_header = *header;
    strncpy(g_name, filename, sizeof(g_name) - 1);
    g_name[sizeof(g_name) - 1] = '\0';
    return 0; // stop after first entry
}

// Parse end record + central directory, seek to the first entry's local
// header, and extract its data. Returns the jzReadData() result, or the
// first error encountered along the way.
static int run_extract_limited(const void *zipBytes, size_t zipLen, void *out,
        size_t outSize, uint32_t compressedLimit, uint32_t *produced) {
    MemJZFile m;
    JZEndRecord er;
    int ret;

    mem_init(&m, zipBytes, zipLen);

    if((ret = jzReadEndRecord(&m.handle, &er)) != JZ_OK)
        return ret;
    if((ret = jzReadCentralDirectory(&m.handle, &er, capture_cb, NULL)) != JZ_OK)
        return ret;
    if(m.handle.seek(&m.handle, (int64_t)g_header.offset, SEEK_SET))
        return JZ_ERR;
    if((ret = jzReadLocalFileHeader(&m.handle, &g_header, NULL, 0)) != JZ_OK)
        return ret;
    return jzReadDataLimited(&m.handle, &g_header, out, outSize,
            compressedLimit, produced);
}

static int run_extract(const void *zipBytes, size_t zipLen, void *out,
        size_t outSize, uint32_t *produced) {
    return run_extract_limited(zipBytes, zipLen, out, outSize, UINT32_MAX,
            produced);
}

// Build a one-entry archive: local header + payload + central + end record.
static void build_simple(ZipBuf *z, const char *name, uint16_t flags,
        uint16_t method, uint32_t crc, uint32_t csize, uint32_t usize,
        const void *payload, size_t payloadLen) {
    size_t cdOffset;
    zb_reset(z);
    zb_local(z, name, 20, flags, method, crc, csize, usize);
    zb_put(z, payload, payloadLen);
    cdOffset = zb_pos(z);
    zb_central(z, name, 20, flags, method, crc, csize, usize, 0);
    zb_endrecord(z, 1, (uint32_t)(zb_pos(z) - cdOffset), (uint32_t)cdOffset, 0);
}

// ---------------------------------------------------------------------------
// Tests: extraction path (jzReadData security contract)
// ---------------------------------------------------------------------------

static void test_store_roundtrip(void) {
    ZipBuf z;
    unsigned char out[64];
    uint32_t produced = 0;

    build_simple(&z, "a.txt", 0, 0, FIX_CRC, FIX_LEN, FIX_LEN, fix_data, FIX_LEN);
    EXPECT(run_extract(z.bytes, z.len, out, sizeof(out), &produced) == JZ_OK);
    EXPECT(produced == FIX_LEN);
    EXPECT(memcmp(out, fix_data, FIX_LEN) == 0);
}

static void test_store_size_mismatch(void) {
    ZipBuf z;
    unsigned char out[64];
    uint32_t produced;

    // Stored entry whose compressed and uncompressed sizes disagree.
    build_simple(&z, "a.txt", 0, 0, FIX_CRC, 16, FIX_LEN, fix_data, FIX_LEN);
    EXPECT(run_extract(z.bytes, z.len, out, sizeof(out), &produced) == JZ_ERR_CORRUPT);
}

static void test_store_crc_mismatch(void) {
    ZipBuf z;
    unsigned char out[64];
    uint32_t produced;

    build_simple(&z, "a.txt", 0, 0, FIX_CRC ^ 1, FIX_LEN, FIX_LEN,
            fix_data, FIX_LEN);
    EXPECT(run_extract(z.bytes, z.len, out, sizeof(out), &produced) == JZ_ERR_CORRUPT);
}

static void test_buffer_too_small(void) {
    ZipBuf z;
    unsigned char out[16];
    unsigned char fullOut[64];
    uint32_t produced;

    build_simple(&z, "a.txt", 0, 0, FIX_CRC, FIX_LEN, FIX_LEN, fix_data, FIX_LEN);
    EXPECT(run_extract(z.bytes, z.len, out, sizeof(out), &produced) == JZ_ERR_SIZE);

    EXPECT(run_extract_limited(z.bytes, z.len, fullOut, sizeof(fullOut),
            FIX_LEN - 1, &produced) == JZ_ERR_SIZE);

    build_simple(&z, "a.txt", 0, 8, FIX_CRC, FIX_DEFLATE_LEN, FIX_LEN,
            fix_deflate, FIX_DEFLATE_LEN);
    EXPECT(run_extract(z.bytes, z.len, out, sizeof(out), &produced) == JZ_ERR_SIZE);
}

static void test_deflate_roundtrip(void) {
    ZipBuf z;
    unsigned char out[64];
    uint32_t produced = 0;

    build_simple(&z, "a.txt", 0, 8, FIX_CRC, FIX_DEFLATE_LEN, FIX_LEN,
            fix_deflate, FIX_DEFLATE_LEN);
#if EXPECT_DEFLATE_SUCCESS
    EXPECT(run_extract(z.bytes, z.len, out, sizeof(out), &produced) == JZ_OK);
    EXPECT(produced == FIX_LEN);
    EXPECT(memcmp(out, fix_data, FIX_LEN) == 0);
#else
    EXPECT(run_extract(z.bytes, z.len, out, sizeof(out), &produced) ==
            JZ_ERR_UNSUPPORTED);
#endif
}

static void test_deflate_short_production(void) {
    ZipBuf z;
    unsigned char out[1024];
    uint32_t produced;

    // Deflate stream decodes to 32 bytes but 1000 are claimed: must be
    // rejected, not zero-padded or left uninitialized.
    build_simple(&z, "a.txt", 0, 8, FIX_CRC, FIX_DEFLATE_LEN, 1000,
            fix_deflate, FIX_DEFLATE_LEN);
    EXPECT(run_extract(z.bytes, z.len, out, sizeof(out), &produced) ==
            (EXPECT_DEFLATE_SUCCESS ? JZ_ERR_CORRUPT : JZ_ERR_UNSUPPORTED));
}

static void test_deflate_over_production(void) {
    ZipBuf z;
    unsigned char out[64];
    uint32_t produced;

    // Stream produces 32 bytes but only 4 are claimed (zip-bomb shape).
    build_simple(&z, "a.txt", 0, 8, FIX_CRC, FIX_DEFLATE_LEN, 4,
            fix_deflate, FIX_DEFLATE_LEN);
    EXPECT(run_extract(z.bytes, z.len, out, sizeof(out), &produced) ==
            (EXPECT_DEFLATE_SUCCESS ? JZ_ERR_CORRUPT : JZ_ERR_UNSUPPORTED));
}

static void test_deflate_trailing_garbage(void) {
    ZipBuf z;
    unsigned char out[64];
    uint32_t produced;

    // compressedSize padded beyond the actual deflate stream.
    zb_reset(&z);
    zb_local(&z, "a.txt", 20, 0, 8, FIX_CRC, FIX_DEFLATE_LEN + 4, FIX_LEN);
    zb_put(&z, fix_deflate, FIX_DEFLATE_LEN);
    zb_put(&z, "JUNK", 4);
    {
        size_t cdOffset = zb_pos(&z);
        zb_central(&z, "a.txt", 20, 0, 8, FIX_CRC, FIX_DEFLATE_LEN + 4, FIX_LEN, 0);
        zb_endrecord(&z, 1, (uint32_t)(zb_pos(&z) - cdOffset), (uint32_t)cdOffset, 0);
    }
    EXPECT(run_extract(z.bytes, z.len, out, sizeof(out), &produced) ==
            (EXPECT_DEFLATE_SUCCESS ? JZ_ERR_CORRUPT : JZ_ERR_UNSUPPORTED));
}

static void test_zip64_sentinel(void) {
    ZipBuf z;
    unsigned char out[64];
    uint32_t produced;

    // 0xFFFFFFFF claim: must be refused outright. No allocation in the
    // library may be sized by it (this suite would abort on ASan otherwise,
    // or write gigabytes in an integrating application).
    build_simple(&z, "huge.bin", 0, 0, 0, 0xFFFFFFFF, 0xFFFFFFFF, NULL, 0);
    EXPECT(run_extract(z.bytes, z.len, out, sizeof(out), &produced) == JZ_ERR_UNSUPPORTED);

    build_simple(&z, "huge.bin", 0, 8, FIX_CRC, 0xFFFFFFFF, FIX_LEN,
            fix_deflate, FIX_DEFLATE_LEN);
    EXPECT(run_extract(z.bytes, z.len, out, sizeof(out), &produced) == JZ_ERR_UNSUPPORTED);
}

static void test_datadesc_ok(void) {
    ZipBuf z;
    unsigned char out[64];
    uint32_t produced = 0;

    // Bit 3 set: local header carries zeros, descriptor carries real values.
    zb_reset(&z);
    zb_local(&z, "dd.txt", 20, 0x08, 0, 0, 0, 0);
    zb_put(&z, fix_data, FIX_LEN);
    zb_datadesc(&z, FIX_CRC, FIX_LEN, FIX_LEN);
    {
        size_t cdOffset = zb_pos(&z);
        zb_central(&z, "dd.txt", 20, 0x08, 0, FIX_CRC, FIX_LEN, FIX_LEN, 0);
        zb_endrecord(&z, 1, (uint32_t)(zb_pos(&z) - cdOffset), (uint32_t)cdOffset, 0);
    }
    EXPECT(run_extract(z.bytes, z.len, out, sizeof(out), &produced) == JZ_OK);
    EXPECT(produced == FIX_LEN);
    EXPECT(memcmp(out, fix_data, FIX_LEN) == 0);
}

static void test_datadesc_lying_size(void) {
    ZipBuf z;
    unsigned char out[64];
    JZFileHeader before;

    // Descriptor claims 1 MB where 64 bytes were delivered. Historically the
    // descriptor overwrote header->uncompressedSize and callers then read
    // far past their buffer (heap disclosure / ASan heap-buffer-overflow).
    zb_reset(&z);
    zb_local(&z, "dd.txt", 20, 0x08, 0, 0, 0, 0);
    zb_put(&z, fix_data, FIX_LEN);
    zb_datadesc(&z, FIX_CRC, FIX_LEN, 0x100000);
    {
        size_t cdOffset = zb_pos(&z);
        zb_central(&z, "dd.txt", 20, 0x08, 0, FIX_CRC, FIX_LEN, FIX_LEN, 0);
        zb_endrecord(&z, 1, (uint32_t)(zb_pos(&z) - cdOffset), (uint32_t)cdOffset, 0);
    }

    // Capture the header state jzReadData receives and verify it is intact
    // afterwards: the descriptor must not mutate caller-visible sizes.
    {
        MemJZFile m;
        JZEndRecord er;
        uint32_t dummy = 0;
        int ret;

        mem_init(&m, z.bytes, z.len);
        EXPECT(jzReadEndRecord(&m.handle, &er) == JZ_OK);
        EXPECT(jzReadCentralDirectory(&m.handle, &er, capture_cb, NULL) == JZ_OK);
        EXPECT(m.handle.seek(&m.handle, (int64_t)g_header.offset, SEEK_SET) == 0);
        EXPECT(jzReadLocalFileHeader(&m.handle, &g_header, NULL, 0) == JZ_OK);

        before = g_header;
        ret = jzReadData(&m.handle, &g_header, out, sizeof(out), &dummy);
        EXPECT(ret == JZ_ERR_CORRUPT);
        EXPECT(g_header.crc32 == before.crc32);
        EXPECT(g_header.compressedSize == before.compressedSize);
        EXPECT(g_header.uncompressedSize == before.uncompressedSize);
    }
}

static void test_datadesc_lying_crc(void) {
    ZipBuf z;
    unsigned char out[64];
    uint32_t produced;

    zb_reset(&z);
    zb_local(&z, "dd.txt", 20, 0x08, 0, 0, 0, 0);
    zb_put(&z, fix_data, FIX_LEN);
    zb_datadesc(&z, FIX_CRC ^ 1, FIX_LEN, FIX_LEN);
    {
        size_t cdOffset = zb_pos(&z);
        zb_central(&z, "dd.txt", 20, 0x08, 0, FIX_CRC, FIX_LEN, FIX_LEN, 0);
        zb_endrecord(&z, 1, (uint32_t)(zb_pos(&z) - cdOffset), (uint32_t)cdOffset, 0);
    }
    EXPECT(run_extract(z.bytes, z.len, out, sizeof(out), &produced) == JZ_ERR_CORRUPT);
}

static void test_encrypted_rejected(void) {
    ZipBuf z;
    unsigned char out[64];
    uint32_t produced;

    build_simple(&z, "secret.txt", 0x01, 0, FIX_CRC, FIX_LEN, FIX_LEN,
            fix_data, FIX_LEN);
    EXPECT(run_extract(z.bytes, z.len, out, sizeof(out), &produced) == JZ_ERR_UNSUPPORTED);
}

static void test_unknown_method_rejected(void) {
    ZipBuf z;
    unsigned char out[64];
    uint32_t produced;

    build_simple(&z, "a.txt", 0, 99, FIX_CRC, FIX_LEN, FIX_LEN,
            fix_data, FIX_LEN);
    EXPECT(run_extract(z.bytes, z.len, out, sizeof(out), &produced) == JZ_ERR_UNSUPPORTED);
}

static void test_empty_entries(void) {
    ZipBuf z;
    unsigned char out[8];
    uint32_t produced = 1;

    // Empty stored file: zero is a legitimate CRC/size, not "absent".
    build_simple(&z, "empty.txt", 0, 0, 0, 0, 0, NULL, 0);
    EXPECT(run_extract(z.bytes, z.len, out, sizeof(out), &produced) == JZ_OK);
    EXPECT(produced == 0);

    // Empty deflate file (raw deflate of empty input is the 2 bytes 03 00).
    produced = 1;
    {
        static const unsigned char empty_deflate[] = { 0x03, 0x00 };
        build_simple(&z, "empty.txt", 0, 8, 0, 2, 0, empty_deflate, 2);
#if EXPECT_DEFLATE_SUCCESS
        EXPECT(run_extract(z.bytes, z.len, out, sizeof(out), &produced) == JZ_OK);
        EXPECT(produced == 0);
#else
        EXPECT(run_extract(z.bytes, z.len, out, sizeof(out), &produced) ==
                JZ_ERR_UNSUPPORTED);
#endif
    }
}

static void test_truncated_archive(void) {
    ZipBuf z;
    unsigned char out[64];
    uint32_t produced;

    // Archive cut off entirely before the end record.
    build_simple(&z, "a.txt", 0, 8, FIX_CRC, FIX_DEFLATE_LEN, FIX_LEN,
            fix_deflate, FIX_DEFLATE_LEN);
    EXPECT(run_extract(z.bytes, 30 + 5, out, sizeof(out), &produced) != JZ_OK);

    // Structure intact, but the deflate payload is shorter than claimed:
    // reads run into the central directory and must still fail cleanly.
    zb_reset(&z);
    zb_local(&z, "a.txt", 20, 0, 8, FIX_CRC, FIX_DEFLATE_LEN, FIX_LEN);
    zb_put(&z, fix_deflate, 5); // only 5 of the claimed 19 bytes present
    {
        size_t cdOffset = zb_pos(&z);
        zb_central(&z, "a.txt", 20, 0, 8, FIX_CRC, FIX_DEFLATE_LEN, FIX_LEN, 0);
        zb_endrecord(&z, 1, (uint32_t)(zb_pos(&z) - cdOffset), (uint32_t)cdOffset, 0);
    }
    EXPECT(run_extract(z.bytes, z.len, out, sizeof(out), &produced) != JZ_OK);
}

static void test_crc32_vectors(void) {
    EXPECT(jz_crc32(0, "123456789", 9) == 0xCBF43926UL);
    EXPECT(jz_crc32(0, "", 0) == 0);
    EXPECT(jz_crc32(0, fix_data, FIX_LEN) == FIX_CRC);
    // Chunked use must match single-shot.
    EXPECT(jz_crc32(jz_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926UL);
}

// ---------------------------------------------------------------------------
// Tests: archive structure validation (end record, central directory)
// ---------------------------------------------------------------------------

static int parse_er(const void *bytes, size_t len, JZEndRecord *er) {
    MemJZFile m;
    mem_init(&m, bytes, len);
    return jzReadEndRecord(&m.handle, er);
}

static int parse_cd(const void *bytes, size_t len) {
    MemJZFile m;
    JZEndRecord er;
    int ret;
    mem_init(&m, bytes, len);
    if((ret = jzReadEndRecord(&m.handle, &er)) != JZ_OK)
        return ret;
    return jzReadCentralDirectory(&m.handle, &er, capture_cb, NULL);
}

static int continue_cb(JZFile *zip, int index, JZFileHeader *header,
        char *filename, void *user_data) {
    (void)zip;
    (void)index;
    (void)header;
    (void)filename;
    (void)user_data;
    return 1;
}

static int parse_cd_fully(const void *bytes, size_t len) {
    MemJZFile m;
    JZEndRecord er;
    int ret;

    mem_init(&m, bytes, len);
    if((ret = jzReadEndRecord(&m.handle, &er)) != JZ_OK)
        return ret;
    return jzReadCentralDirectory(&m.handle, &er, continue_cb, NULL);
}

static void test_empty_zip_valid(void) {
    ZipBuf z;
    JZEndRecord er;

    // A 22-byte end record with no entries is a valid empty archive.
    zb_reset(&z);
    zb_endrecord(&z, 0, 0, 0, 0);
    EXPECT(z.len == sizeof(JZEndRecord));
    EXPECT(parse_er(z.bytes, z.len, &er) == JZ_OK);
    EXPECT(er.numEntries == 0);
    EXPECT(parse_cd(z.bytes, z.len) == JZ_OK);
}

static void test_max_comment(void) {
    ZipBuf z;
    JZEndRecord er;
    unsigned char out[64];
    uint32_t produced = 0;

    // A maximum-length comment pushes the end record 65557 bytes from EOF;
    // the scan window must cover it.
    zb_reset(&z);
    zb_local(&z, "a.txt", 20, 0, 0, FIX_CRC, FIX_LEN, FIX_LEN);
    zb_put(&z, fix_data, FIX_LEN);
    {
        size_t cdOffset = zb_pos(&z);
        zb_central(&z, "a.txt", 20, 0, 0, FIX_CRC, FIX_LEN, FIX_LEN, 0);
        zb_endrecord(&z, 1, (uint32_t)(zb_pos(&z) - cdOffset),
                (uint32_t)cdOffset, 65535);
    }
    EXPECT(parse_er(z.bytes, z.len, &er) == JZ_OK);
    EXPECT(er.zipCommentLength == 65535);
    EXPECT(run_extract(z.bytes, z.len, out, sizeof(out), &produced) == JZ_OK);
    EXPECT(produced == FIX_LEN);
}

static void test_cd_offset_size_wrap(void) {
    ZipBuf z;
    JZEndRecord er;

    // centralDirectoryOffset + centralDirectorySize wraps in 32 bits:
    // 0xFFFFFFF0 + 0x20 == 0x10, which would pass a wrapping bounds check.
    zb_reset(&z);
    zb_put(&z, "PPPPPPPPPPPPPPPPPPPP", 20);
    zb_endrecord(&z, 1, 0x20, 0xFFFFFFF0, 0);
    EXPECT(parse_er(z.bytes, z.len, &er) == JZ_ERR_CORRUPT);
}

static void test_cd_beyond_endrecord(void) {
    ZipBuf z;
    JZEndRecord er;

    // Central directory overlapping the end record position.
    zb_reset(&z);
    zb_put(&z, "PPPPPPPPPPPPPPPPPPPP", 20);
    zb_endrecord(&z, 0, 64, 12, 0); // cd 12..76, but ER starts at 20
    EXPECT(parse_er(z.bytes, z.len, &er) == JZ_ERR_CORRUPT);
}

static void test_numentries_overcount(void) {
    ZipBuf z;
    JZEndRecord er;

    // One real entry in the directory, but the end record claims 100:
    // 100 * sizeof(JZGlobalFileHeader) cannot fit in the declared size.
    zb_reset(&z);
    zb_local(&z, "a.txt", 20, 0, 0, FIX_CRC, FIX_LEN, FIX_LEN);
    zb_put(&z, fix_data, FIX_LEN);
    {
        size_t cdOffset = zb_pos(&z);
        zb_central(&z, "a.txt", 20, 0, 0, FIX_CRC, FIX_LEN, FIX_LEN, 0);
        zb_endrecord(&z, 100, (uint32_t)(zb_pos(&z) - cdOffset),
                (uint32_t)cdOffset, 0);
    }
    EXPECT(parse_er(z.bytes, z.len, &er) == JZ_ERR_CORRUPT);
}

static void test_cd_entry_overrun(void) {
    ZipBuf z;
    JZEndRecord er;

    // Central header claims a 1000-byte filename, but the declared
    // directory only holds the fixed-size header.
    zb_reset(&z);
    zb_local(&z, "a.txt", 20, 0, 0, FIX_CRC, FIX_LEN, FIX_LEN);
    zb_put(&z, fix_data, FIX_LEN);
    {
        JZGlobalFileHeader gh;
        size_t cdOffset = zb_pos(&z);
        memset(&gh, 0, sizeof(gh));
        gh.signature = 0x02014B50;
        gh.versionMadeBy = 20;
        gh.versionNeededToExtract = 20;
        gh.crc32 = FIX_CRC;
        gh.compressedSize = FIX_LEN;
        gh.uncompressedSize = FIX_LEN;
        gh.fileNameLength = 1000;
        zb_put(&z, &gh, sizeof(gh)); // no filename bytes follow
        zb_endrecord(&z, 1, (uint32_t)sizeof(gh), (uint32_t)cdOffset, 0);
    }
    EXPECT(parse_er(z.bytes, z.len, &er) == JZ_OK); // structurally fine
    EXPECT(parse_cd(z.bytes, z.len) == JZ_ERR_CORRUPT); // iteration bounded
}

static void test_zip64_endrecord(void) {
    ZipBuf z;
    JZEndRecord er;

    zb_reset(&z);
    zb_endrecord(&z, 0xFFFF, 0, 0, 0);
    EXPECT(parse_er(z.bytes, z.len, &er) == JZ_ERR_UNSUPPORTED);

    zb_reset(&z);
    zb_endrecord(&z, 0, 0xFFFFFFFF, 0, 0);
    EXPECT(parse_er(z.bytes, z.len, &er) == JZ_ERR_UNSUPPORTED);
}

static void test_version_rejected(void) {
    ZipBuf z;
    unsigned char out[64];
    uint32_t produced;

    // versionNeededToExtract 51 implies features (e.g. strong encryption)
    // this library does not implement; fail early and loudly.
    zb_reset(&z);
    zb_local(&z, "a.txt", 51, 0, 0, FIX_CRC, FIX_LEN, FIX_LEN);
    zb_put(&z, fix_data, FIX_LEN);
    {
        size_t cdOffset = zb_pos(&z);
        zb_central(&z, "a.txt", 51, 0, 0, FIX_CRC, FIX_LEN, FIX_LEN, 0);
        zb_endrecord(&z, 1, (uint32_t)(zb_pos(&z) - cdOffset),
                (uint32_t)cdOffset, 0);
    }
    EXPECT(parse_cd(z.bytes, z.len) == JZ_ERR_UNSUPPORTED);
    EXPECT(run_extract(z.bytes, z.len, out, sizeof(out), &produced) == JZ_ERR_UNSUPPORTED);
}

static int check_local_header(const void *bytes, size_t len) {
    MemJZFile m;
    JZEndRecord er;
    int ret;

    mem_init(&m, bytes, len);
    if((ret = jzReadEndRecord(&m.handle, &er)) != JZ_OK)
        return ret;
    if((ret = jzReadCentralDirectory(&m.handle, &er, capture_cb, NULL)) != JZ_OK)
        return ret;
    if(m.handle.seek(&m.handle, (int64_t)g_header.offset, SEEK_SET))
        return JZ_ERR;
    return jzReadLocalFileHeaderChecked(&m.handle, &g_header, g_name);
}

static void test_central_local_mismatch(void) {
    ZipBuf z;
    JZLocalFileHeader *local;

    build_simple(&z, "a.txt", 0, 0, FIX_CRC, FIX_LEN, FIX_LEN,
            fix_data, FIX_LEN);
    local = (JZLocalFileHeader *)z.bytes;
    local->compressedSize = FIX_LEN - 1;
    EXPECT(check_local_header(z.bytes, z.len) == JZ_ERR_CORRUPT);

    build_simple(&z, "a.txt", 0, 0, FIX_CRC, FIX_LEN, FIX_LEN,
            fix_data, FIX_LEN);
    local = (JZLocalFileHeader *)z.bytes;
    z.bytes[sizeof(JZLocalFileHeader)] = 'b';
    EXPECT(check_local_header(z.bytes, z.len) == JZ_ERR_CORRUPT);

    // Under bit 3, zero local values are allowed, but contradictory nonzero
    // values must not be accepted in place of the central-directory bounds.
    zb_reset(&z);
    zb_local(&z, "dd.txt", 20, 0x08, 0, 1, 0, 0);
    zb_put(&z, fix_data, FIX_LEN);
    zb_datadesc(&z, FIX_CRC, FIX_LEN, FIX_LEN);
    {
        size_t cdOffset = zb_pos(&z);
        zb_central(&z, "dd.txt", 20, 0x08, 0, FIX_CRC, FIX_LEN,
                FIX_LEN, 0);
        zb_endrecord(&z, 1, (uint32_t)(zb_pos(&z) - cdOffset),
                (uint32_t)cdOffset, 0);
    }
    EXPECT(check_local_header(z.bytes, z.len) == JZ_ERR_CORRUPT);
}

static void test_unsupported_entry_layout(void) {
    ZipBuf z;
    size_t cdOffset;
    JZGlobalFileHeader *central;

    build_simple(&z, "a.txt", 0, 0, FIX_CRC, FIX_LEN, FIX_LEN,
            fix_data, FIX_LEN);
    cdOffset = sizeof(JZLocalFileHeader) + strlen("a.txt") + FIX_LEN;
    central = (JZGlobalFileHeader *)(z.bytes + cdOffset);

    central->diskNumberStart = 1;
    EXPECT(parse_cd(z.bytes, z.len) == JZ_ERR_UNSUPPORTED);

    central->diskNumberStart = 0;
    central->relativeOffsetOflocalHeader = 0xFFFFFFFF;
    EXPECT(parse_cd(z.bytes, z.len) == JZ_ERR_UNSUPPORTED);

    central->relativeOffsetOflocalHeader = (uint32_t)cdOffset;
    EXPECT(parse_cd(z.bytes, z.len) == JZ_ERR_CORRUPT);
}

static void test_trailing_central_bytes(void) {
    ZipBuf z;
    size_t cdOffset;
    uint32_t cdSize;
    size_t endRecordOffset;

    build_simple(&z, "a.txt", 0, 0, FIX_CRC, FIX_LEN, FIX_LEN,
            fix_data, FIX_LEN);
    cdOffset = sizeof(JZLocalFileHeader) + strlen("a.txt") + FIX_LEN;
    cdSize = (uint32_t)(sizeof(JZGlobalFileHeader) + strlen("a.txt"));
    endRecordOffset = z.len - sizeof(JZEndRecord);
    memmove(z.bytes + endRecordOffset + 1, z.bytes + endRecordOffset,
            sizeof(JZEndRecord));
    z.bytes[endRecordOffset] = 'X';
    z.len++;
    ((JZEndRecord *)(z.bytes + endRecordOffset + 1))->centralDirectorySize =
        cdSize + 1;
    EXPECT(parse_cd_fully(z.bytes, z.len) == JZ_ERR_CORRUPT);
    EXPECT(cdOffset + cdSize + 1 == endRecordOffset + 1);
}

static void test_filename_safety(void) {
    static const char *safe[] = {
        "file.txt", "dir/file.txt", "a/b/c/d.e", "dir/",
        "a..b", "..foo/bar", "foo.", NULL
    };
    static const char *unsafe[] = {
        "", "/abs.txt", "\\abs.txt", "C:/win.txt", "c:\\win.txt",
        "../evil", "..\\evil", "a/../b", "a\\..\\b", "a/./b",
        "foo/..", "foo/.", "..", ".", "a//b", "dir//", "bad\tname",
        "bad\x7Fname", "file:stream", NULL
    };
    int i;

    for(i = 0; safe[i]; i++)
        EXPECT(jzFilenameIsSafe(safe[i]) == 1);
    for(i = 0; unsafe[i]; i++)
        EXPECT(jzFilenameIsSafe(unsafe[i]) == 0);
}

// ---------------------------------------------------------------------------

int main(void) {
    test_crc32_vectors();
    test_store_roundtrip();
    test_store_size_mismatch();
    test_store_crc_mismatch();
    test_buffer_too_small();
    test_deflate_roundtrip();
    test_deflate_short_production();
    test_deflate_over_production();
    test_deflate_trailing_garbage();
    test_zip64_sentinel();
    test_datadesc_ok();
    test_datadesc_lying_size();
    test_datadesc_lying_crc();
    test_encrypted_rejected();
    test_unknown_method_rejected();
    test_empty_entries();
    test_truncated_archive();
    test_empty_zip_valid();
    test_max_comment();
    test_cd_offset_size_wrap();
    test_cd_beyond_endrecord();
    test_numentries_overcount();
    test_cd_entry_overrun();
    test_zip64_endrecord();
    test_version_rejected();
    test_central_local_mismatch();
    test_unsupported_entry_layout();
    test_trailing_central_bytes();
    test_filename_safety();

    printf("%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
