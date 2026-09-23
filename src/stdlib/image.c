/*
 * image.c — Implementation of the std.image standard library module.
 *
 * PNG encode/decode are implemented in pure C99 without any external library.
 * The PNG decoder supports:
 *   - Colour types: Greyscale (0), RGB (2), RGBA (6), Greyscale+Alpha (4)
 *   - Bit depth: 8 bits per channel only
 *   - Filter types 0-4 (None, Sub, Up, Average, Paeth) per RFC 2083
 *   - DEFLATE inflate: stored blocks (BTYPE=00), fixed Huffman (BTYPE=01),
 *     and dynamic Huffman (BTYPE=10) per RFC 1951
 * The PNG encoder uses DEFLATE stored (uncompressed) blocks, which is valid
 * per RFC 1951 and avoids a Huffman/LZ77 compression implementation.
 *
 * BMP encode outputs 24-bit uncompressed (BI_RGB) files — no external
 * dependencies.  Alpha channels are dropped; grayscale is expanded to BGR.
 *
 * JPEG and WebP are intentionally stubbed.  Functions for those formats
 * return is_err=1 with a message directing the caller to link the appropriate
 * library.
 *
 * No external dependencies beyond libc.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code.  Callers own all returned heap pointers.
 *
 * Story: 18.1.6
 */

#include "image.h"
#define _USE_MATH_DEFINES
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Utility helpers
 * ========================================================================= */

static uint32_t read_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}

static void write_u32_be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8);
    p[3] = (uint8_t)(v);
}

/* =========================================================================
 * CRC-32 (ISO 3309) — used by PNG chunk checksums
 * ========================================================================= */

static uint32_t crc32_table[256];
static int      crc32_table_ready = 0;

static void crc32_init(void)
{
    uint32_t c;
    int n, k;
    for (n = 0; n < 256; n++) {
        c = (uint32_t)n;
        for (k = 0; k < 8; k++) {
            if (c & 1u)
                c = 0xEDB88320u ^ (c >> 1);
            else
                c >>= 1;
        }
        crc32_table[n] = c;
    }
    crc32_table_ready = 1;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *buf, uint64_t len)
{
    if (!crc32_table_ready) crc32_init();
    crc ^= 0xFFFFFFFFu;
    for (uint64_t i = 0; i < len; i++)
        crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

/* =========================================================================
 * Adler-32 — used by zlib wrapper inside PNG IDAT
 * ========================================================================= */

static uint32_t adler32_update(uint32_t adler, const uint8_t *buf, uint64_t len)
{
    uint32_t s1 = adler & 0xFFFF;
    uint32_t s2 = (adler >> 16) & 0xFFFF;
    for (uint64_t i = 0; i < len; i++) {
        s1 = (s1 + buf[i]) % 65521u;
        s2 = (s2 + s1)     % 65521u;
    }
    return (s2 << 16) | s1;
}

/* =========================================================================
 * DEFLATE inflate (RFC 1951) — used by PNG decoder
 * ========================================================================= */

/* Bit-stream reader */
typedef struct {
    const uint8_t *src;
    uint64_t       src_len;
    uint64_t       byte_pos;
    uint32_t       bits;       /* bit buffer */
    uint32_t       bits_avail; /* number of valid bits in buf */
    int            error;
} BitStream;

static void bs_init(BitStream *bs, const uint8_t *src, uint64_t len)
{
    bs->src        = src;
    bs->src_len    = len;
    bs->byte_pos   = 0;
    bs->bits       = 0;
    bs->bits_avail = 0;
    bs->error      = 0;
}

static void bs_fill(BitStream *bs)
{
    while (bs->bits_avail < 24 && bs->byte_pos < bs->src_len) {
        bs->bits |= (uint32_t)bs->src[bs->byte_pos++] << bs->bits_avail;
        bs->bits_avail += 8;
    }
}

static uint32_t bs_peek(BitStream *bs, uint32_t n)
{
    if (bs->bits_avail < n) bs_fill(bs);
    if (bs->bits_avail < n) { bs->error = 1; return 0; }
    return bs->bits & ((1u << n) - 1u);
}

static uint32_t bs_read(BitStream *bs, uint32_t n)
{
    uint32_t v = bs_peek(bs, n);
    bs->bits       >>= n;
    bs->bits_avail  -= n;
    return v;
}

/* Align to next byte boundary (for stored blocks) */
static void bs_align(BitStream *bs)
{
    uint32_t discard = bs->bits_avail & 7u;
    bs->bits       >>= discard;
    bs->bits_avail  -= discard;
}

/* Read a byte directly from the stream (byte-aligned) */
static uint8_t __attribute__((unused)) bs_read_byte(BitStream *bs)
{
    if (bs->bits_avail == 0 && bs->byte_pos < bs->src_len)
        return bs->src[bs->byte_pos++];
    return (uint8_t)bs_read(bs, 8);
}

/* -------------------------------------------------------------------------
 * Canonical Huffman code decoder
 * ------------------------------------------------------------------------- */

#define INFLATE_MAXBITS  15
#define INFLATE_MAXSYMS  288

typedef struct {
    uint16_t counts[INFLATE_MAXBITS + 1]; /* number of codes with each length */
    uint16_t symbols[INFLATE_MAXSYMS];    /* symbols sorted by code */
} HuffTree;

/* Build a canonical Huffman tree from code lengths.
 * Returns 0 on success, -1 on error. */
static int hufftree_build(HuffTree *tree, const uint8_t *lengths, int n)
{
    int i;
    uint16_t offsets[INFLATE_MAXBITS + 1];

    memset(tree->counts, 0, sizeof(tree->counts));
    memset(tree->symbols, 0, sizeof(tree->symbols));

    for (i = 0; i < n; i++)
        if (lengths[i] > INFLATE_MAXBITS) return -1;

    for (i = 0; i < n; i++)
        tree->counts[lengths[i]]++;

    tree->counts[0] = 0;

    offsets[0] = 0;
    for (i = 1; i <= INFLATE_MAXBITS; i++)
        offsets[i] = (uint16_t)(offsets[i-1] + tree->counts[i-1]);

    for (i = 0; i < n; i++) {
        if (lengths[i] != 0)
            tree->symbols[offsets[lengths[i]]++] = (uint16_t)i;
    }
    return 0;
}

/* Decode one symbol from the bit stream using the Huffman tree.
 * Returns the symbol or -1 on error. */
static int hufftree_decode(BitStream *bs, const HuffTree *tree)
{
    int code  = 0;
    int first = 0;
    int index = 0;
    int len;

    for (len = 1; len <= INFLATE_MAXBITS; len++) {
        code  = (code << 1) | (int)bs_read(bs, 1);
        if (bs->error) return -1;
        int count = (int)tree->counts[len];
        if (code - count < first) {
            return (int)tree->symbols[index + (code - first)];
        }
        index += count;
        first  = (first + count) << 1;
    }
    return -1; /* bad code */
}

/* -------------------------------------------------------------------------
 * Fixed Huffman trees (RFC 1951 §3.2.6)
 * ------------------------------------------------------------------------- */

static HuffTree fixed_litlen_tree;
static HuffTree fixed_dist_tree;
static int      fixed_trees_built = 0;

static void build_fixed_trees(void)
{
    uint8_t lengths[288];
    int i;

    /* Literal/length: 0-143=8, 144-255=9, 256-279=7, 280-287=8 */
    for (i =   0; i <= 143; i++) lengths[i] = 8;
    for (i = 144; i <= 255; i++) lengths[i] = 9;
    for (i = 256; i <= 279; i++) lengths[i] = 7;
    for (i = 280; i <= 287; i++) lengths[i] = 8;
    hufftree_build(&fixed_litlen_tree, lengths, 288);

    /* Distance: all 5 bits */
    for (i = 0; i < 30; i++) lengths[i] = 5;
    hufftree_build(&fixed_dist_tree, lengths, 30);

    fixed_trees_built = 1;
}

/* -------------------------------------------------------------------------
 * Length/distance extra-bits tables (RFC 1951 §3.2.5)
 * ------------------------------------------------------------------------- */

static const uint16_t length_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
    35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t length_extra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
    3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t dist_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
    257,385,513,769,1025,1537,2049,3073,4097,6145,
    8193,12289,16385,24577
};
static const uint8_t dist_extra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
    7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

/* -------------------------------------------------------------------------
 * Dynamic output buffer
 * ------------------------------------------------------------------------- */

typedef struct {
    uint8_t  *buf;
    uint64_t  len;
    uint64_t  cap;
    int       error;
} OutBuf;

static void outbuf_init(OutBuf *ob)
{
    ob->buf   = NULL;
    ob->len   = 0;
    ob->cap   = 0;
    ob->error = 0;
}

static void outbuf_push(OutBuf *ob, uint8_t byte)
{
    if (ob->error) return;
    if (ob->len == ob->cap) {
        uint64_t newcap = ob->cap ? ob->cap * 2 : 4096;
        uint8_t *newbuf = (uint8_t *)realloc(ob->buf, newcap);
        if (!newbuf) { ob->error = 1; return; }
        ob->buf = newbuf;
        ob->cap = newcap;
    }
    ob->buf[ob->len++] = byte;
}

static void outbuf_push_backref(OutBuf *ob, uint32_t dist, uint32_t length)
{
    if (ob->error) return;
    if (dist > ob->len) { ob->error = 1; return; }
    for (uint32_t i = 0; i < length; i++)
        outbuf_push(ob, ob->buf[ob->len - dist]);
}

/* -------------------------------------------------------------------------
 * Inflate one block (called after reading the 3 header bits)
 * ------------------------------------------------------------------------- */

static int inflate_stored(BitStream *bs, OutBuf *ob)
{
    /* Discard partial byte bits to byte-align the bit stream. */
    bs_align(bs);

    /* Read LEN and NLEN through the bit-stream interface so that any bytes
     * already pre-fetched into the bit buffer are consumed correctly.
     * After bs_align the buffer is byte-aligned, so bs_read(bs, 8) yields
     * the next logical byte in the correct order. */
    uint32_t len_lo  = bs_read(bs, 8);
    uint32_t len_hi  = bs_read(bs, 8);
    uint32_t nlen_lo = bs_read(bs, 8);
    uint32_t nlen_hi = bs_read(bs, 8);
    if (bs->error) return -1;

    uint16_t len  = (uint16_t)(len_lo  | (len_hi  << 8));
    uint16_t nlen = (uint16_t)(nlen_lo | (nlen_hi << 8));

    if ((uint16_t)(len ^ nlen) != 0xFFFF) return -1;

    /* The stored data bytes may still be partially in the bit buffer.
     * Read them through bs_read(bs, 8) to keep the stream consistent. */
    for (uint16_t i = 0; i < len; i++) {
        uint32_t byte_val = bs_read(bs, 8);
        if (bs->error) return -1;
        outbuf_push(ob, (uint8_t)byte_val);
    }

    return ob->error ? -1 : 0;
}

static int inflate_huffman(BitStream *bs, OutBuf *ob,
                            const HuffTree *litlen, const HuffTree *dist)
{
    for (;;) {
        int sym = hufftree_decode(bs, litlen);
        if (sym < 0 || bs->error) return -1;
        if (sym < 256) {
            outbuf_push(ob, (uint8_t)sym);
        } else if (sym == 256) {
            break; /* end of block */
        } else {
            /* Length */
            int lcode = sym - 257;
            if (lcode < 0 || lcode >= 29) return -1;
            uint32_t length = length_base[lcode] + bs_read(bs, length_extra[lcode]);
            if (bs->error) return -1;

            /* Distance */
            int dsym = hufftree_decode(bs, dist);
            if (dsym < 0 || dsym >= 30 || bs->error) return -1;
            uint32_t distance = dist_base[dsym] + bs_read(bs, dist_extra[dsym]);
            if (bs->error) return -1;

            outbuf_push_backref(ob, distance, length);
        }
        if (ob->error) return -1;
    }
    return 0;
}

static int inflate_dynamic(BitStream *bs, OutBuf *ob)
{
    uint32_t hlit  = bs_read(bs, 5) + 257;
    uint32_t hdist = bs_read(bs, 5) + 1;
    uint32_t hclen = bs_read(bs, 4) + 4;
    if (bs->error) return -1;
    if (hlit > 286 || hdist > 30 || hclen > 19) return -1;

    static const uint8_t code_length_order[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
    };

    uint8_t cl_lengths[19];
    memset(cl_lengths, 0, sizeof(cl_lengths));
    for (uint32_t i = 0; i < hclen; i++)
        cl_lengths[code_length_order[i]] = (uint8_t)bs_read(bs, 3);
    if (bs->error) return -1;

    HuffTree cl_tree;
    if (hufftree_build(&cl_tree, cl_lengths, 19) != 0) return -1;

    /* Decode literal/length + distance code lengths */
    uint32_t total = hlit + hdist;
    uint8_t  ll_lengths[286 + 30];
    memset(ll_lengths, 0, sizeof(ll_lengths));

    uint32_t i = 0;
    while (i < total) {
        int sym = hufftree_decode(bs, &cl_tree);
        if (sym < 0 || bs->error) return -1;
        if (sym < 16) {
            ll_lengths[i++] = (uint8_t)sym;
        } else if (sym == 16) {
            uint32_t rep = bs_read(bs, 2) + 3;
            if (bs->error || i == 0) return -1;
            uint8_t prev = ll_lengths[i - 1];
            for (uint32_t k = 0; k < rep && i < total; k++) ll_lengths[i++] = prev;
        } else if (sym == 17) {
            uint32_t rep = bs_read(bs, 3) + 3;
            if (bs->error) return -1;
            for (uint32_t k = 0; k < rep && i < total; k++) ll_lengths[i++] = 0;
        } else if (sym == 18) {
            uint32_t rep = bs_read(bs, 7) + 11;
            if (bs->error) return -1;
            for (uint32_t k = 0; k < rep && i < total; k++) ll_lengths[i++] = 0;
        } else {
            return -1;
        }
    }

    HuffTree litlen_tree, dist_tree;
    if (hufftree_build(&litlen_tree, ll_lengths,        (int)hlit)  != 0) return -1;
    if (hufftree_build(&dist_tree,   ll_lengths + hlit, (int)hdist) != 0) return -1;

    return inflate_huffman(bs, ob, &litlen_tree, &dist_tree);
}

/* -------------------------------------------------------------------------
 * Top-level inflate (processes all blocks)
 * Returns heap-allocated data in ob on success (caller frees), -1 on error.
 * ------------------------------------------------------------------------- */

static int inflate_all(const uint8_t *src, uint64_t src_len, OutBuf *ob)
{
    if (!fixed_trees_built) build_fixed_trees();

    BitStream bs;
    bs_init(&bs, src, src_len);
    outbuf_init(ob);

    int bfinal;
    do {
        bfinal      = (int)bs_read(&bs, 1);
        uint32_t btype = bs_read(&bs, 2);
        if (bs.error) return -1;

        int rc;
        if      (btype == 0) rc = inflate_stored(&bs, ob);
        else if (btype == 1) rc = inflate_huffman(&bs, ob, &fixed_litlen_tree, &fixed_dist_tree);
        else if (btype == 2) rc = inflate_dynamic(&bs, ob);
        else                 return -1; /* btype == 3 is reserved */

        if (rc != 0) return -1;
        if (ob->error) return -1;
    } while (!bfinal);

    return 0;
}

/* =========================================================================
 * PNG filter reconstruction (RFC 2083 §6)
 * ========================================================================= */

static uint8_t paeth_predictor(int a, int b, int c)
{
    int p  = a + b - c;
    int pa = p - a; if (pa < 0) pa = -pa;
    int pb = p - b; if (pb < 0) pb = -pb;
    int pc = p - c; if (pc < 0) pc = -pc;
    if (pa <= pb && pa <= pc) return (uint8_t)a;
    if (pb <= pc)             return (uint8_t)b;
    return (uint8_t)c;
}

/* Reconstruct all rows in-place.
 * raw: height rows of (1 + stride) bytes each (first byte = filter type).
 * out: pre-allocated height*stride bytes. */
static int png_unfilter(const uint8_t *raw, uint8_t *out,
                        uint32_t width, uint32_t height, uint8_t channels)
{
    uint32_t stride = width * channels;
    for (uint32_t y = 0; y < height; y++) {
        uint8_t filter = raw[y * (stride + 1)];
        const uint8_t *row  = raw + y * (stride + 1) + 1;
        uint8_t       *prev = (y == 0) ? NULL : out + (y - 1) * stride;
        uint8_t       *cur  = out + y * stride;

        if (filter == 0) { /* None */
            memcpy(cur, row, stride);
        } else if (filter == 1) { /* Sub */
            for (uint32_t x = 0; x < stride; x++) {
                uint8_t a = (x >= channels) ? cur[x - channels] : 0;
                cur[x] = row[x] + a;
            }
        } else if (filter == 2) { /* Up */
            for (uint32_t x = 0; x < stride; x++) {
                uint8_t b = prev ? prev[x] : 0;
                cur[x] = row[x] + b;
            }
        } else if (filter == 3) { /* Average */
            for (uint32_t x = 0; x < stride; x++) {
                uint8_t a = (x >= channels) ? cur[x - channels] : 0;
                uint8_t b = prev ? prev[x] : 0;
                cur[x] = row[x] + (uint8_t)(((int)a + b) / 2);
            }
        } else if (filter == 4) { /* Paeth */
            for (uint32_t x = 0; x < stride; x++) {
                int a = (x >= channels) ? (int)cur[x - channels] : 0;
                int b = prev ? (int)prev[x] : 0;
                int c = (prev && x >= channels) ? (int)prev[x - channels] : 0;
                cur[x] = row[x] + paeth_predictor(a, b, c);
            }
        } else {
            return -1; /* unknown filter */
        }
    }
    return 0;
}

/* =========================================================================
 * PNG decode
 * ========================================================================= */

static const uint8_t PNG_SIG[8] = {137,80,78,71,13,10,26,10};

ImgResult image_decode(const uint8_t *bytes, uint64_t len)
{
    ImgResult res;
    memset(&res, 0, sizeof(res));

    if (!bytes || len < 8) {
        res.is_err  = 1;
        res.err_msg = "image_decode: buffer too short to be a valid image";
        return res;
    }
    /* 135.5: TIFF is auto-detected here so existing callers of image.decode
     * gain it without changing.  A multi-page TIFF decodes to its FIRST page;
     * image_tiff_pages / image_tiff_decode reach the rest. */
    if ((bytes[0] == 'I' && bytes[1] == 'I' && bytes[2] == 42 && bytes[3] == 0) ||
        (bytes[0] == 'M' && bytes[1] == 'M' && bytes[2] == 0 && bytes[3] == 42)) {
        return image_tiff_decode(bytes, len, 0);
    }
    if (memcmp(bytes, PNG_SIG, 8) != 0) {
        res.is_err  = 1;
        res.err_msg = "image_decode: not a PNG or TIFF file (bad signature)";
        return res;
    }

    /* Parse chunks */
    uint32_t width = 0, height = 0;
    uint8_t  bit_depth = 0, color_type = 0, channels = 0;
    int      ihdr_seen = 0;

    /* Collect all IDAT data */
    uint8_t  *idat_buf  = NULL;
    uint64_t  idat_len  = 0;
    uint64_t  idat_cap  = 0;

    uint64_t pos = 8;
    while (pos + 12 <= len) {
        uint32_t chunk_len  = read_u32_be(bytes + pos);       pos += 4;
        uint8_t  chunk_type[4];
        memcpy(chunk_type, bytes + pos, 4);                    pos += 4;

        if (pos + chunk_len + 4 > len) {
            free(idat_buf);
            res.is_err  = 1;
            res.err_msg = "image_decode: truncated PNG chunk";
            return res;
        }

        const uint8_t *chunk_data = bytes + pos;
        /* uint32_t chunk_crc = read_u32_be(bytes + pos + chunk_len); -- we skip CRC validation */
        pos += chunk_len + 4; /* data + CRC */

        if (memcmp(chunk_type, "IHDR", 4) == 0) {
            if (chunk_len < 13) {
                free(idat_buf);
                res.is_err  = 1;
                res.err_msg = "image_decode: IHDR chunk too short";
                return res;
            }
            width      = read_u32_be(chunk_data);
            height     = read_u32_be(chunk_data + 4);
            bit_depth  = chunk_data[8];
            color_type = chunk_data[9];
            ihdr_seen  = 1;

            if (bit_depth != 8) {
                free(idat_buf);
                res.is_err  = 1;
                res.err_msg = "image_decode: only 8-bit PNG is supported";
                return res;
            }
            if      (color_type == 0) channels = 1;
            else if (color_type == 2) channels = 3;
            else if (color_type == 4) channels = 2; /* greyscale+alpha */
            else if (color_type == 6) channels = 4;
            else {
                free(idat_buf);
                res.is_err  = 1;
                res.err_msg = "image_decode: unsupported PNG color type";
                return res;
            }
        } else if (memcmp(chunk_type, "IDAT", 4) == 0) {
            if (!ihdr_seen) {
                free(idat_buf);
                res.is_err  = 1;
                res.err_msg = "image_decode: IDAT before IHDR";
                return res;
            }
            /* Append to idat_buf */
            if (idat_len + chunk_len > idat_cap) {
                uint64_t newcap = idat_cap ? idat_cap * 2 : 65536;
                while (newcap < idat_len + chunk_len) newcap *= 2;
                uint8_t *newbuf = (uint8_t *)realloc(idat_buf, newcap);
                if (!newbuf) {
                    free(idat_buf);
                    res.is_err  = 1;
                    res.err_msg = "image_decode: OOM accumulating IDAT";
                    return res;
                }
                idat_buf = newbuf;
                idat_cap = newcap;
            }
            memcpy(idat_buf + idat_len, chunk_data, chunk_len);
            idat_len += chunk_len;
        } else if (memcmp(chunk_type, "IEND", 4) == 0) {
            break;
        }
        /* All other chunks are silently skipped */
    }

    if (!ihdr_seen || idat_len == 0) {
        free(idat_buf);
        res.is_err  = 1;
        res.err_msg = "image_decode: missing IHDR or IDAT chunk";
        return res;
    }

    /* Strip zlib header (2 bytes) and Adler-32 trailer (4 bytes) */
    if (idat_len < 6) {
        free(idat_buf);
        res.is_err  = 1;
        res.err_msg = "image_decode: IDAT zlib data too short";
        return res;
    }

    const uint8_t *deflate_data = idat_buf + 2;
    uint64_t       deflate_len  = idat_len - 6;

    OutBuf ob;
    if (inflate_all(deflate_data, deflate_len, &ob) != 0) {
        free(idat_buf);
        free(ob.buf);
        res.is_err  = 1;
        res.err_msg = "image_decode: DEFLATE inflate failed";
        return res;
    }
    free(idat_buf);

    /* Expected size: height rows × (1 + width*channels) bytes */
    uint64_t expected = (uint64_t)height * ((uint64_t)width * channels + 1);
    if (ob.len != expected) {
        free(ob.buf);
        res.is_err  = 1;
        res.err_msg = "image_decode: decompressed size mismatch";
        return res;
    }

    uint64_t pixel_bytes = (uint64_t)width * height * channels;
    uint8_t *pixels = (uint8_t *)malloc(pixel_bytes);
    if (!pixels) {
        free(ob.buf);
        res.is_err  = 1;
        res.err_msg = "image_decode: OOM allocating pixel buffer";
        return res;
    }

    if (png_unfilter(ob.buf, pixels, width, height, channels) != 0) {
        free(ob.buf);
        free(pixels);
        res.is_err  = 1;
        res.err_msg = "image_decode: unsupported PNG filter type";
        return res;
    }
    free(ob.buf);

    res.ok.width    = width;
    res.ok.height   = height;
    res.ok.channels = channels;
    res.ok.data     = pixels;
    res.ok.data_len = pixel_bytes;
    return res;
}

/* =========================================================================
 * PNG encode (DEFLATE stored blocks, filter type 0 / None)
 * ========================================================================= */

/* Append a PNG chunk to a dynamic output buffer. */
static void png_write_chunk(OutBuf *ob, const char *type,
                            const uint8_t *data, uint32_t len)
{
    /* Length (4 bytes) */
    uint8_t lbuf[4];
    write_u32_be(lbuf, len);
    for (int i = 0; i < 4; i++) outbuf_push(ob, lbuf[i]);

    /* Type (4 bytes) */
    for (int i = 0; i < 4; i++) outbuf_push(ob, (uint8_t)type[i]);

    /* Data */
    for (uint32_t i = 0; i < len; i++) outbuf_push(ob, data[i]);

    /* CRC over type + data */
    uint32_t crc = crc32_update(0, (const uint8_t *)type, 4);
    if (data && len > 0) crc = crc32_update(crc, data, len);
    uint8_t cbuf[4];
    write_u32_be(cbuf, crc);
    for (int i = 0; i < 4; i++) outbuf_push(ob, cbuf[i]);
}

/* Wrap raw bytes in a zlib stream (CMF=0x78, FLG=0x01, stored blocks). */
static uint8_t *zlib_wrap_stored(const uint8_t *src, uint64_t src_len,
                                  uint64_t *out_len)
{
    /* Worst case: 2 (header) + ceil(src_len/65535)*5 (block headers) + src_len + 4 (adler) */
    uint64_t max_blocks = (src_len / 65535) + 1;
    uint64_t cap = 2 + max_blocks * 5 + src_len + 4;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) return NULL;

    uint64_t pos = 0;

    /* zlib header: CMF=0x78 (deflate, window=32K), FLG chosen so CMF*256+FLG % 31 == 0 */
    buf[pos++] = 0x78;
    buf[pos++] = 0x01;

    /* DEFLATE stored blocks */
    uint64_t remaining = src_len;
    uint64_t src_pos   = 0;
    while (remaining > 0 || src_len == 0) {
        uint16_t block_len = (remaining > 65535) ? 65535 : (uint16_t)remaining;
        int      is_last   = (remaining <= 65535) ? 1 : 0;

        buf[pos++] = (uint8_t)(is_last ? 1 : 0); /* BFINAL | BTYPE=00 */
        buf[pos++] = (uint8_t)(block_len & 0xFF);
        buf[pos++] = (uint8_t)(block_len >> 8);
        buf[pos++] = (uint8_t)((~block_len) & 0xFF);
        buf[pos++] = (uint8_t)((~block_len) >> 8);

        if (block_len > 0) {
            memcpy(buf + pos, src + src_pos, block_len);
            pos      += block_len;
            src_pos  += block_len;
            remaining -= block_len;
        }
        if (is_last) break;
    }

    /* Adler-32 of uncompressed data */
    uint32_t adler = adler32_update(1, src, src_len);
    buf[pos++] = (uint8_t)(adler >> 24);
    buf[pos++] = (uint8_t)(adler >> 16);
    buf[pos++] = (uint8_t)(adler >>  8);
    buf[pos++] = (uint8_t)(adler);

    *out_len = pos;
    return buf;
}

ImgEncResult image_encode(TkImgBuf buf, TkImgFmt fmt, uint8_t quality)
{
    ImgEncResult res;
    memset(&res, 0, sizeof(res));

    (void)quality; /* only used for lossy formats */

    if (fmt == IMG_FMT_JPEG) {
        res.is_err  = 1;
        res.err_msg = "JPEG requires libjpeg — link with -ljpeg";
        return res;
    }
    if (fmt == IMG_FMT_WEBP) {
        res.is_err  = 1;
        res.err_msg = "WebP requires libwebp — link with -lwebp";
        return res;
    }
    if (fmt == IMG_FMT_BMP) {
        /* 24-bit uncompressed BMP (no alpha). */
        if (!buf.data || buf.width == 0 || buf.height == 0 ||
            buf.channels == 0 || buf.channels > 4) {
            res.is_err  = 1;
            res.err_msg = "image_encode: invalid TkImgBuf";
            return res;
        }

        uint32_t w = buf.width;
        uint32_t h = buf.height;
        /* Each row: 3 bytes per pixel, padded to 4-byte boundary. */
        uint32_t row_bytes   = w * 3;
        uint32_t row_padding = (4 - (row_bytes % 4)) % 4;
        uint32_t row_stride  = row_bytes + row_padding;
        uint32_t pixel_size  = row_stride * h;
        uint32_t file_size   = 14 + 40 + pixel_size;

        uint8_t *out = (uint8_t *)malloc(file_size);
        if (!out) {
            res.is_err  = 1;
            res.err_msg = "image_encode: BMP allocation failed";
            return res;
        }
        memset(out, 0, file_size);

        /* --- 14-byte BMP file header --- */
        out[0] = 'B'; out[1] = 'M';
        out[2] = (uint8_t)(file_size);
        out[3] = (uint8_t)(file_size >> 8);
        out[4] = (uint8_t)(file_size >> 16);
        out[5] = (uint8_t)(file_size >> 24);
        /* bytes 6-9: reserved (0) */
        uint32_t data_offset = 14 + 40;
        out[10] = (uint8_t)(data_offset);
        out[11] = (uint8_t)(data_offset >> 8);
        out[12] = (uint8_t)(data_offset >> 16);
        out[13] = (uint8_t)(data_offset >> 24);

        /* --- 40-byte BITMAPINFOHEADER --- */
        uint32_t hdr_size = 40;
        out[14] = (uint8_t)(hdr_size);
        out[15] = (uint8_t)(hdr_size >> 8);
        out[16] = (uint8_t)(hdr_size >> 16);
        out[17] = (uint8_t)(hdr_size >> 24);
        /* width (signed 32-bit LE) */
        out[18] = (uint8_t)(w);
        out[19] = (uint8_t)(w >> 8);
        out[20] = (uint8_t)(w >> 16);
        out[21] = (uint8_t)(w >> 24);
        /* height (signed 32-bit LE, positive = bottom-up) */
        out[22] = (uint8_t)(h);
        out[23] = (uint8_t)(h >> 8);
        out[24] = (uint8_t)(h >> 16);
        out[25] = (uint8_t)(h >> 24);
        /* planes = 1 */
        out[26] = 1; out[27] = 0;
        /* bits per pixel = 24 */
        out[28] = 24; out[29] = 0;
        /* compression = 0 (BI_RGB), image size = 0, rest stays 0 */

        /* --- Pixel data: bottom-to-top, BGR order --- */
        uint8_t ch = buf.channels;
        uint32_t src_stride = w * ch;
        for (uint32_t y = 0; y < h; y++) {
            /* BMP row 0 = bottom of image = source row (h-1-y) */
            const uint8_t *src_row = buf.data + (uint64_t)(h - 1 - y) * src_stride;
            uint8_t       *dst_row = out + data_offset + (uint64_t)y * row_stride;
            for (uint32_t x = 0; x < w; x++) {
                const uint8_t *px = src_row + x * ch;
                uint8_t r, g, b;
                if (ch == 1) {
                    r = g = b = px[0];
                } else {
                    r = px[0]; g = px[1]; b = px[2];
                }
                dst_row[x * 3 + 0] = b;
                dst_row[x * 3 + 1] = g;
                dst_row[x * 3 + 2] = r;
            }
            /* padding bytes are already 0 from memset */
        }

        res.ok     = out;
        res.ok_len = file_size;
        return res;
    }

    /* PNG encode */
    if (!buf.data || buf.width == 0 || buf.height == 0 ||
        buf.channels == 0 || buf.channels > 4) {
        res.is_err  = 1;
        res.err_msg = "image_encode: invalid TkImgBuf";
        return res;
    }

    uint8_t color_type;
    if      (buf.channels == 1) color_type = 0; /* greyscale */
    else if (buf.channels == 3) color_type = 2; /* RGB */
    else if (buf.channels == 4) color_type = 6; /* RGBA */
    else {
        res.is_err  = 1;
        res.err_msg = "image_encode: unsupported channel count for PNG";
        return res;
    }

    /* Build filtered rows (filter type 0 = None for all rows) */
    uint32_t stride       = buf.width * buf.channels;
    uint64_t raw_row_len  = 1 + stride;           /* 1 filter byte + pixels */
    uint64_t raw_len      = (uint64_t)buf.height * raw_row_len;
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) {
        res.is_err  = 1;
        res.err_msg = "image_encode: OOM building filtered rows";
        return res;
    }
    for (uint32_t y = 0; y < buf.height; y++) {
        raw[y * raw_row_len] = 0; /* filter type None */
        memcpy(raw + y * raw_row_len + 1,
               buf.data + (uint64_t)y * stride, stride);
    }

    uint64_t  zlib_len;
    uint8_t  *zlib_buf = zlib_wrap_stored(raw, raw_len, &zlib_len);
    free(raw);
    if (!zlib_buf) {
        res.is_err  = 1;
        res.err_msg = "image_encode: OOM compressing IDAT";
        return res;
    }

    OutBuf ob;
    outbuf_init(&ob);

    /* PNG signature */
    for (int i = 0; i < 8; i++) outbuf_push(&ob, PNG_SIG[i]);

    /* IHDR (13 bytes) */
    uint8_t ihdr[13];
    write_u32_be(ihdr,     buf.width);
    write_u32_be(ihdr + 4, buf.height);
    ihdr[8]  = 8;          /* bit depth */
    ihdr[9]  = color_type;
    ihdr[10] = 0;          /* compression method */
    ihdr[11] = 0;          /* filter method */
    ihdr[12] = 0;          /* interlace method */
    png_write_chunk(&ob, "IHDR", ihdr, 13);

    /* IDAT */
    png_write_chunk(&ob, "IDAT", zlib_buf, (uint32_t)zlib_len);
    free(zlib_buf);

    /* IEND */
    png_write_chunk(&ob, "IEND", NULL, 0);

    if (ob.error) {
        free(ob.buf);
        res.is_err  = 1;
        res.err_msg = "image_encode: OOM building PNG output";
        return res;
    }

    res.ok     = ob.buf;
    res.ok_len = ob.len;
    return res;
}

/* =========================================================================
 * image_resize — bilinear interpolation
 * ========================================================================= */

TkImgBuf image_resize(TkImgBuf buf, uint32_t width, uint32_t height)
{
    TkImgBuf out;
    memset(&out, 0, sizeof(out));

    if (!buf.data || buf.width == 0 || buf.height == 0 || width == 0 || height == 0)
        return out;

    uint64_t out_len = (uint64_t)width * height * buf.channels;
    uint8_t *data = (uint8_t *)malloc(out_len);
    if (!data) return out;

    float x_scale = (float)buf.width  / (float)width;
    float y_scale = (float)buf.height / (float)height;

    for (uint32_t oy = 0; oy < height; oy++) {
        for (uint32_t ox = 0; ox < width; ox++) {
            float fx = ((float)ox + 0.5f) * x_scale - 0.5f;
            float fy = ((float)oy + 0.5f) * y_scale - 0.5f;

            int x0 = (int)fx; if (x0 < 0) x0 = 0;
            int y0 = (int)fy; if (y0 < 0) y0 = 0;
            int x1 = x0 + 1; if ((uint32_t)x1 >= buf.width)  x1 = (int)buf.width  - 1;
            int y1 = y0 + 1; if ((uint32_t)y1 >= buf.height) y1 = (int)buf.height - 1;

            float wx = fx - (float)x0; if (wx < 0.f) wx = 0.f;
            float wy = fy - (float)y0; if (wy < 0.f) wy = 0.f;

            uint32_t stride = buf.width * buf.channels;
            const uint8_t *p00 = buf.data + (uint64_t)y0 * stride + (uint64_t)x0 * buf.channels;
            const uint8_t *p10 = buf.data + (uint64_t)y0 * stride + (uint64_t)x1 * buf.channels;
            const uint8_t *p01 = buf.data + (uint64_t)y1 * stride + (uint64_t)x0 * buf.channels;
            const uint8_t *p11 = buf.data + (uint64_t)y1 * stride + (uint64_t)x1 * buf.channels;

            uint8_t *dst = data + ((uint64_t)oy * width + ox) * buf.channels;
            for (uint8_t c = 0; c < buf.channels; c++) {
                float v = (1.f - wx) * (1.f - wy) * p00[c]
                        +        wx  * (1.f - wy) * p10[c]
                        + (1.f - wx) *        wy  * p01[c]
                        +        wx  *        wy  * p11[c];
                dst[c] = (uint8_t)(v + 0.5f);
            }
        }
    }

    out.width    = width;
    out.height   = height;
    out.channels = buf.channels;
    out.data     = data;
    out.data_len = out_len;
    return out;
}

/* =========================================================================
 * image_crop
 * ========================================================================= */

ImgResult image_crop(TkImgBuf buf, uint32_t x, uint32_t y,
                     uint32_t width, uint32_t height)
{
    ImgResult res;
    memset(&res, 0, sizeof(res));

    if (!buf.data) {
        res.is_err  = 1;
        res.err_msg = "image_crop: null image buffer";
        return res;
    }
    if ((uint64_t)x + width > buf.width || (uint64_t)y + height > buf.height) {
        res.is_err  = 1;
        res.err_msg = "image_crop: crop rectangle exceeds image bounds";
        return res;
    }

    uint64_t out_len = (uint64_t)width * height * buf.channels;
    uint8_t *data = (uint8_t *)malloc(out_len);
    if (!data) {
        res.is_err  = 1;
        res.err_msg = "image_crop: OOM";
        return res;
    }

    uint32_t src_stride = buf.width * buf.channels;
    uint32_t dst_stride = width  * buf.channels;
    for (uint32_t row = 0; row < height; row++) {
        const uint8_t *src_row = buf.data + ((uint64_t)(y + row) * src_stride)
                                           + (uint64_t)x * buf.channels;
        memcpy(data + (uint64_t)row * dst_stride, src_row, dst_stride);
    }

    res.ok.width    = width;
    res.ok.height   = height;
    res.ok.channels = buf.channels;
    res.ok.data     = data;
    res.ok.data_len = out_len;
    return res;
}

/* =========================================================================
 * image_to_grayscale
 * ========================================================================= */

TkImgBuf image_to_grayscale(TkImgBuf buf)
{
    TkImgBuf out;
    memset(&out, 0, sizeof(out));

    if (!buf.data || buf.channels == 0) return out;

    uint64_t num_pixels = (uint64_t)buf.width * buf.height;
    uint8_t *data = (uint8_t *)malloc(num_pixels);
    if (!data) return out;

    if (buf.channels == 1) {
        /* Already greyscale — just copy */
        memcpy(data, buf.data, num_pixels);
    } else {
        for (uint64_t i = 0; i < num_pixels; i++) {
            const uint8_t *p = buf.data + i * buf.channels;
            uint8_t r = p[0];
            uint8_t g = (buf.channels >= 2) ? p[1] : p[0];
            uint8_t b = (buf.channels >= 3) ? p[2] : p[0];
            /* ITU-R BT.601 luminance */
            data[i] = (uint8_t)(0.299f * r + 0.587f * g + 0.114f * b + 0.5f);
        }
    }

    out.width    = buf.width;
    out.height   = buf.height;
    out.channels = 1;
    out.data     = data;
    out.data_len = num_pixels;
    return out;
}

/* =========================================================================
 * image_flip_h — reverse pixel order within each row
 * ========================================================================= */

TkImgBuf image_flip_h(TkImgBuf buf)
{
    TkImgBuf out;
    memset(&out, 0, sizeof(out));

    if (!buf.data || buf.width == 0 || buf.height == 0) return out;

    uint8_t *data = (uint8_t *)malloc(buf.data_len);
    if (!data) return out;

    uint32_t stride = buf.width * buf.channels;
    for (uint32_t y = 0; y < buf.height; y++) {
        const uint8_t *src_row = buf.data + (uint64_t)y * stride;
        uint8_t       *dst_row = data     + (uint64_t)y * stride;
        for (uint32_t x = 0; x < buf.width; x++) {
            const uint8_t *src_px = src_row + (uint64_t)(buf.width - 1 - x) * buf.channels;
            uint8_t       *dst_px = dst_row + (uint64_t)x * buf.channels;
            memcpy(dst_px, src_px, buf.channels);
        }
    }

    out.width    = buf.width;
    out.height   = buf.height;
    out.channels = buf.channels;
    out.data     = data;
    out.data_len = buf.data_len;
    return out;
}

/* =========================================================================
 * image_flip_v — reverse row order
 * ========================================================================= */

TkImgBuf image_flip_v(TkImgBuf buf)
{
    TkImgBuf out;
    memset(&out, 0, sizeof(out));

    if (!buf.data || buf.width == 0 || buf.height == 0) return out;

    uint8_t *data = (uint8_t *)malloc(buf.data_len);
    if (!data) return out;

    uint32_t stride = buf.width * buf.channels;
    for (uint32_t y = 0; y < buf.height; y++) {
        const uint8_t *src_row = buf.data + (uint64_t)(buf.height - 1 - y) * stride;
        uint8_t       *dst_row = data     + (uint64_t)y * stride;
        memcpy(dst_row, src_row, stride);
    }

    out.width    = buf.width;
    out.height   = buf.height;
    out.channels = buf.channels;
    out.data     = data;
    out.data_len = buf.data_len;
    return out;
}

/* =========================================================================
 * image_pixel_at
 * ========================================================================= */

PixelResult image_pixel_at(TkImgBuf buf, uint32_t x, uint32_t y)
{
    PixelResult res;
    memset(&res, 0, sizeof(res));

    if (!buf.data || x >= buf.width || y >= buf.height) {
        res.is_err  = 1;
        res.err_msg = "image_pixel_at: coordinates out of bounds";
        return res;
    }

    uint8_t *px = (uint8_t *)malloc(buf.channels);
    if (!px) {
        res.is_err  = 1;
        res.err_msg = "image_pixel_at: OOM";
        return res;
    }

    uint64_t offset = ((uint64_t)y * buf.width + x) * buf.channels;
    memcpy(px, buf.data + offset, buf.channels);

    res.ok     = px;
    res.ok_len = buf.channels;
    return res;
}

/* =========================================================================
 * image_from_raw
 * ========================================================================= */

TkImgBuf image_from_raw(const uint8_t *data, uint32_t width,
                         uint32_t height, uint8_t channels)
{
    TkImgBuf out;
    memset(&out, 0, sizeof(out));

    uint64_t len = (uint64_t)width * height * channels;
    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) return out;

    if (data && len > 0)
        memcpy(copy, data, len);
    else
        memset(copy, 0, len);

    out.width    = width;
    out.height   = height;
    out.channels = channels;
    out.data     = copy;
    out.data_len = len;
    return out;
}

/* =========================================================================
 * image_buf_free
 * ========================================================================= */

void image_buf_free(TkImgBuf *buf)
{
    if (buf && buf->data) {
        free(buf->data);
        buf->data     = NULL;
        buf->data_len = 0;
    }
}

/* =========================================================================
 * Story 34.3.1 — Transforms and filters
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * Shared helper: clamp a double to [0,255] and return as uint8_t.
 * ------------------------------------------------------------------------- */
static uint8_t clamp_u8(double v)
{
    if (v < 0.0)   return 0;
    if (v > 255.0) return 255;
    return (uint8_t)(v + 0.5);
}

/* -------------------------------------------------------------------------
 * image_rotate
 * Bilinear interpolation, inverse-mapping, same output size as input.
 * ------------------------------------------------------------------------- */

TkImgBuf image_rotate(TkImgBuf buf, double angle_deg)
{
    TkImgBuf out;
    memset(&out, 0, sizeof(out));

    if (!buf.data || buf.width == 0 || buf.height == 0) return out;

    uint64_t out_len = (uint64_t)buf.width * buf.height * buf.channels;
    uint8_t *data = (uint8_t *)malloc(out_len);
    if (!data) return out;
    memset(data, 0, out_len);

    double cx = (double)buf.width  / 2.0;
    double cy = (double)buf.height / 2.0;
    /* Inverse rotation angle (rotate source back) */
    double angle = -angle_deg * M_PI / 180.0;
    double cos_a = cos(angle);
    double sin_a = sin(angle);

    uint32_t stride = buf.width * buf.channels;

    for (uint32_t oy = 0; oy < buf.height; oy++) {
        for (uint32_t ox = 0; ox < buf.width; ox++) {
            double dx = (double)ox - cx;
            double dy = (double)oy - cy;
            double sx = cos_a * dx - sin_a * dy + cx;
            double sy = sin_a * dx + cos_a * dy + cy;

            /* Bilinear interpolation */
            int x0 = (int)sx;
            int y0 = (int)sy;
            int x1 = x0 + 1;
            int y1 = y0 + 1;

            if (x0 < 0 || y0 < 0 ||
                x1 >= (int)buf.width || y1 >= (int)buf.height) {
                /* Out of bounds — leave as 0 */
                continue;
            }

            double wx = sx - (double)x0;
            double wy = sy - (double)y0;

            const uint8_t *p00 = buf.data + (uint64_t)y0 * stride + (uint64_t)x0 * buf.channels;
            const uint8_t *p10 = buf.data + (uint64_t)y0 * stride + (uint64_t)x1 * buf.channels;
            const uint8_t *p01 = buf.data + (uint64_t)y1 * stride + (uint64_t)x0 * buf.channels;
            const uint8_t *p11 = buf.data + (uint64_t)y1 * stride + (uint64_t)x1 * buf.channels;

            uint8_t *dst = data + ((uint64_t)oy * buf.width + ox) * buf.channels;
            for (uint8_t c = 0; c < buf.channels; c++) {
                double v = (1.0 - wx) * (1.0 - wy) * (double)p00[c]
                         +        wx  * (1.0 - wy) * (double)p10[c]
                         + (1.0 - wx) *        wy  * (double)p01[c]
                         +        wx  *        wy  * (double)p11[c];
                dst[c] = clamp_u8(v);
            }
        }
    }

    out.width    = buf.width;
    out.height   = buf.height;
    out.channels = buf.channels;
    out.data     = data;
    out.data_len = out_len;
    return out;
}

/* -------------------------------------------------------------------------
 * image_blur
 * 3×3 Gaussian kernel [1,2,1;2,4,2;1,2,1]/16 applied `radius` times.
 * Clamp-to-edge for border pixels.
 * ------------------------------------------------------------------------- */

/* Apply one pass of the 3×3 Gaussian kernel to src into dst (same dims). */
static void blur_pass(const uint8_t *src, uint8_t *dst,
                      uint32_t w, uint32_t h, uint8_t ch)
{
    static const int K[3][3] = {{1,2,1},{2,4,2},{1,2,1}};

    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            for (uint8_t c = 0; c < ch; c++) {
                int acc = 0;
                for (int ky = -1; ky <= 1; ky++) {
                    for (int kx = -1; kx <= 1; kx++) {
                        /* Clamp-to-edge */
                        int sx = (int)x + kx;
                        int sy = (int)y + ky;
                        if (sx < 0)         sx = 0;
                        if (sy < 0)         sy = 0;
                        if ((uint32_t)sx >= w) sx = (int)w - 1;
                        if ((uint32_t)sy >= h) sy = (int)h - 1;
                        acc += K[ky+1][kx+1] *
                               (int)src[((uint64_t)sy * w + (uint64_t)sx) * ch + c];
                    }
                }
                dst[((uint64_t)y * w + x) * ch + c] = (uint8_t)(acc / 16);
            }
        }
    }
}

TkImgBuf image_blur(TkImgBuf buf, int radius)
{
    TkImgBuf out;
    memset(&out, 0, sizeof(out));

    if (!buf.data || buf.width == 0 || buf.height == 0) return out;
    if (radius < 1) radius = 1;

    uint64_t len  = (uint64_t)buf.width * buf.height * buf.channels;
    uint8_t *cur  = (uint8_t *)malloc(len);
    uint8_t *next = (uint8_t *)malloc(len);
    if (!cur || !next) { free(cur); free(next); return out; }

    memcpy(cur, buf.data, len);

    for (int i = 0; i < radius; i++) {
        blur_pass(cur, next, buf.width, buf.height, buf.channels);
        /* Swap buffers */
        uint8_t *tmp = cur; cur = next; next = tmp;
    }
    free(next);

    out.width    = buf.width;
    out.height   = buf.height;
    out.channels = buf.channels;
    out.data     = cur;
    out.data_len = len;
    return out;
}

/* -------------------------------------------------------------------------
 * image_sharpen
 * Kernel: [0,-1,0; -1,5,-1; 0,-1,0]
 * ------------------------------------------------------------------------- */

TkImgBuf image_sharpen(TkImgBuf buf)
{
    TkImgBuf out;
    memset(&out, 0, sizeof(out));

    if (!buf.data || buf.width == 0 || buf.height == 0) return out;

    uint64_t len = (uint64_t)buf.width * buf.height * buf.channels;
    uint8_t *data = (uint8_t *)malloc(len);
    if (!data) return out;

    uint32_t w  = buf.width;
    uint32_t h  = buf.height;
    uint8_t  ch = buf.channels;

    /* Kernel weights for (row-1..row+1, col-1..col+1) in row-major order */
    static const int K[3][3] = {{ 0,-1, 0},
                                 {-1, 5,-1},
                                 { 0,-1, 0}};

    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            for (uint8_t c = 0; c < ch; c++) {
                int acc = 0;
                for (int ky = -1; ky <= 1; ky++) {
                    for (int kx = -1; kx <= 1; kx++) {
                        int sx = (int)x + kx;
                        int sy = (int)y + ky;
                        if (sx < 0)            sx = 0;
                        if (sy < 0)            sy = 0;
                        if ((uint32_t)sx >= w) sx = (int)w - 1;
                        if ((uint32_t)sy >= h) sy = (int)h - 1;
                        acc += K[ky+1][kx+1] *
                               (int)buf.data[((uint64_t)sy * w + (uint64_t)sx) * ch + c];
                    }
                }
                data[((uint64_t)y * w + x) * ch + c] = clamp_u8((double)acc);
            }
        }
    }

    out.width    = w;
    out.height   = h;
    out.channels = ch;
    out.data     = data;
    out.data_len = len;
    return out;
}

/* -------------------------------------------------------------------------
 * image_brightness
 * Multiply each non-alpha channel by factor, clamp to [0,255].
 * Alpha (channel index 3 when channels==4) is preserved unchanged.
 * ------------------------------------------------------------------------- */

TkImgBuf image_brightness(TkImgBuf buf, double factor)
{
    TkImgBuf out;
    memset(&out, 0, sizeof(out));

    if (!buf.data || buf.width == 0 || buf.height == 0) return out;

    uint64_t len = (uint64_t)buf.width * buf.height * buf.channels;
    uint8_t *data = (uint8_t *)malloc(len);
    if (!data) return out;

    memcpy(data, buf.data, len);

    uint64_t num_pixels = (uint64_t)buf.width * buf.height;
    for (uint64_t i = 0; i < num_pixels; i++) {
        uint8_t *p = data + i * buf.channels;
        uint8_t  color_channels = (buf.channels == 4) ? 3 : buf.channels;
        for (uint8_t c = 0; c < color_channels; c++) {
            p[c] = clamp_u8((double)p[c] * factor);
        }
        /* Alpha channel (index 3) left unchanged */
    }

    out.width    = buf.width;
    out.height   = buf.height;
    out.channels = buf.channels;
    out.data     = data;
    out.data_len = len;
    return out;
}

/* -------------------------------------------------------------------------
 * image_contrast
 * new = 128 + (old - 128) * factor, clamped to [0,255].
 * Alpha channel (index 3 when channels==4) is preserved unchanged.
 * ------------------------------------------------------------------------- */

TkImgBuf image_contrast(TkImgBuf buf, double factor)
{
    TkImgBuf out;
    memset(&out, 0, sizeof(out));

    if (!buf.data || buf.width == 0 || buf.height == 0) return out;

    uint64_t len = (uint64_t)buf.width * buf.height * buf.channels;
    uint8_t *data = (uint8_t *)malloc(len);
    if (!data) return out;

    memcpy(data, buf.data, len);

    uint64_t num_pixels = (uint64_t)buf.width * buf.height;
    for (uint64_t i = 0; i < num_pixels; i++) {
        uint8_t *p = data + i * buf.channels;
        uint8_t  color_channels = (buf.channels == 4) ? 3 : buf.channels;
        for (uint8_t c = 0; c < color_channels; c++) {
            double v = 128.0 + ((double)p[c] - 128.0) * factor;
            p[c] = clamp_u8(v);
        }
    }

    out.width    = buf.width;
    out.height   = buf.height;
    out.channels = buf.channels;
    out.data     = data;
    out.data_len = len;
    return out;
}

/* -------------------------------------------------------------------------
 * image_paste
 * Composite src onto a copy of dst at (x, y).  Clips to dst bounds.
 * RGBA images use per-pixel alpha blending; RGB/grey just overwrites.
 * ------------------------------------------------------------------------- */

TkImgBuf image_paste(TkImgBuf dst, TkImgBuf src, int x, int y)
{
    TkImgBuf out;
    memset(&out, 0, sizeof(out));

    if (!dst.data || dst.width == 0 || dst.height == 0) return out;
    if (!src.data || src.width == 0 || src.height == 0) {
        /* Nothing to paste — return a copy of dst */
        uint8_t *copy = (uint8_t *)malloc(dst.data_len);
        if (!copy) return out;
        memcpy(copy, dst.data, dst.data_len);
        out = dst;
        out.data = copy;
        return out;
    }

    uint64_t len  = dst.data_len;
    uint8_t *data = (uint8_t *)malloc(len);
    if (!data) return out;
    memcpy(data, dst.data, len);

    uint32_t dst_stride = dst.width * dst.channels;
    uint32_t src_stride = src.width * src.channels;

    for (uint32_t sy = 0; sy < src.height; sy++) {
        int dy = y + (int)sy;
        if (dy < 0 || (uint32_t)dy >= dst.height) continue;

        for (uint32_t sx = 0; sx < src.width; sx++) {
            int dx = x + (int)sx;
            if (dx < 0 || (uint32_t)dx >= dst.width) continue;

            const uint8_t *sp = src.data + (uint64_t)sy * src_stride
                                          + (uint64_t)sx * src.channels;
            uint8_t       *dp = data     + (uint64_t)(uint32_t)dy * dst_stride
                                          + (uint64_t)(uint32_t)dx * dst.channels;

            uint8_t ch = (src.channels < dst.channels) ? src.channels : dst.channels;

            if (src.channels == 4 && dst.channels == 4) {
                /* Alpha blending: out = alpha*src + (1-alpha)*dst */
                double alpha = (double)sp[3] / 255.0;
                dp[0] = clamp_u8(alpha * (double)sp[0] + (1.0 - alpha) * (double)dp[0]);
                dp[1] = clamp_u8(alpha * (double)sp[1] + (1.0 - alpha) * (double)dp[1]);
                dp[2] = clamp_u8(alpha * (double)sp[2] + (1.0 - alpha) * (double)dp[2]);
                dp[3] = clamp_u8(alpha * (double)sp[3] + (1.0 - alpha) * (double)dp[3]);
            } else {
                /* Plain copy for matching or non-RGBA channels */
                for (uint8_t c = 0; c < ch; c++)
                    dp[c] = sp[c];
            }
        }
    }

    out.width    = dst.width;
    out.height   = dst.height;
    out.channels = dst.channels;
    out.data     = data;
    out.data_len = len;
    return out;
}

/* =========================================================================
 * Story 34.3.2 stubs — histogram, quantize, text_draw
 * Full implementations are deferred to Story 34.3.2.
 * ========================================================================= */

ImgHistogram image_histogram(TkImgBuf buf)
{
    ImgHistogram h;
    memset(&h, 0, sizeof(h));
    if (!buf.data) return h;

    uint64_t num_pixels = (uint64_t)buf.width * buf.height;
    for (uint64_t i = 0; i < num_pixels; i++) {
        const uint8_t *p = buf.data + i * buf.channels;
        if (buf.channels == 1) {
            h.r[p[0]]++;
        } else if (buf.channels >= 3) {
            h.r[p[0]]++;
            h.g[p[1]]++;
            h.b[p[2]]++;
            if (buf.channels == 4) h.a[p[3]]++;
        }
    }
    return h;
}

/* =========================================================================
 * image_quantize — median-cut palette reduction  (Story 34.3.2)
 * ========================================================================= */

/* Bucket descriptor for median-cut. */
typedef struct {
    uint64_t start; /* inclusive start index into working pixel array */
    uint64_t count; /* number of pixels in this bucket */
} MedBucket;

/* Channel index used by pixel_cmp; set before each qsort call. */
static int g_sort_ch = 0;

static int pixel_cmp(const void *a, const void *b)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    return (int)pa[g_sort_ch] - (int)pb[g_sort_ch];
}

TkImgBuf image_quantize(TkImgBuf buf, uint64_t ncolors)
{
    TkImgBuf zero;
    memset(&zero, 0, sizeof(zero));

    if (!buf.data || buf.width == 0 || buf.height == 0 ||
        buf.channels < 3 || ncolors == 0)
        return zero;

    uint64_t npix = (uint64_t)buf.width * buf.height;
    uint8_t  ch   = buf.channels;

    /* Working copy of pixels that we will sort in-place. */
    uint8_t *pixels = (uint8_t *)malloc(npix * ch);
    if (!pixels) return zero;
    memcpy(pixels, buf.data, npix * ch);

    /* Bucket array; upper bound: we never need more than 2*ncolors entries. */
    uint64_t max_buckets = ncolors * 2 + 2;
    MedBucket *buckets = (MedBucket *)malloc(max_buckets * sizeof(MedBucket));
    if (!buckets) { free(pixels); return zero; }

    buckets[0].start = 0;
    buckets[0].count = npix;
    uint64_t nbuckets = 1;

    /* Iteratively split the largest bucket until we reach ncolors buckets. */
    while (nbuckets < ncolors) {
        /* Find the bucket with the most pixels. */
        uint64_t largest = 0;
        for (uint64_t i = 1; i < nbuckets; i++)
            if (buckets[i].count > buckets[largest].count)
                largest = i;

        if (buckets[largest].count <= 1)
            break;

        MedBucket *bk = &buckets[largest];

        /* Find the colour channel with the largest range in this bucket. */
        uint8_t lo[3] = {255, 255, 255};
        uint8_t hi[3] = {  0,   0,   0};
        for (uint64_t i = bk->start; i < bk->start + bk->count; i++) {
            const uint8_t *p = pixels + i * ch;
            for (int c = 0; c < 3; c++) {
                if (p[c] < lo[c]) lo[c] = p[c];
                if (p[c] > hi[c]) hi[c] = p[c];
            }
        }
        int best_ch = 0;
        int best_range = (int)hi[0] - (int)lo[0];
        for (int c = 1; c < 3; c++) {
            int range = (int)hi[c] - (int)lo[c];
            if (range > best_range) { best_range = range; best_ch = c; }
        }

        /* Sort this bucket's pixels by the chosen channel. */
        g_sort_ch = best_ch;
        qsort(pixels + bk->start * ch, (size_t)bk->count, ch, pixel_cmp);

        /* Split at the median. */
        uint64_t half        = bk->count / 2;
        uint64_t orig_start  = bk->start;
        uint64_t orig_count  = bk->count;

        buckets[largest].start = orig_start;
        buckets[largest].count = half;

        if (nbuckets < max_buckets) {
            buckets[nbuckets].start = orig_start + half;
            buckets[nbuckets].count = orig_count - half;
            nbuckets++;
        }
    }

    /* Compute representative (mean) colour for each bucket. */
    uint8_t *palette = (uint8_t *)malloc(nbuckets * ch);
    if (!palette) { free(pixels); free(buckets); return zero; }

    for (uint64_t bi = 0; bi < nbuckets; bi++) {
        MedBucket *bk = &buckets[bi];
        uint64_t sum[4] = {0, 0, 0, 0};
        for (uint64_t i = bk->start; i < bk->start + bk->count; i++) {
            const uint8_t *p = pixels + i * ch;
            for (uint8_t c = 0; c < ch; c++) sum[c] += p[c];
        }
        uint8_t *pal = palette + bi * ch;
        for (uint8_t c = 0; c < ch; c++)
            pal[c] = (uint8_t)(sum[c] / bk->count);
    }

    /* Map every original pixel to its nearest palette entry. */
    uint8_t *out_data = (uint8_t *)malloc(npix * ch);
    if (!out_data) { free(palette); free(pixels); free(buckets); return zero; }

    for (uint64_t i = 0; i < npix; i++) {
        const uint8_t *orig = buf.data + i * ch;
        uint64_t best_dist  = (uint64_t)-1;
        uint64_t best_pal   = 0;
        for (uint64_t bi = 0; bi < nbuckets; bi++) {
            const uint8_t *pal = palette + bi * ch;
            uint64_t dist = 0;
            for (int c = 0; c < 3 && c < (int)ch; c++) {
                int64_t d = (int64_t)orig[c] - (int64_t)pal[c];
                dist += (uint64_t)(d * d);
            }
            if (dist < best_dist) { best_dist = dist; best_pal = bi; }
        }
        memcpy(out_data + i * ch, palette + best_pal * ch, ch);
    }

    free(palette);
    free(pixels);
    free(buckets);

    TkImgBuf out;
    out.width    = buf.width;
    out.height   = buf.height;
    out.channels = buf.channels;
    out.data     = out_data;
    out.data_len = npix * ch;
    return out;
}

/* =========================================================================
 * image_text_draw — built-in 5×7 bitmap font  (Story 34.3.2)
 * =========================================================================
 *
 * Covers ASCII 32–126 (95 characters).  Each glyph is 7 rows × 5 columns.
 * Each byte encodes one row; bit 4 (0x10) is the leftmost (column 0) pixel.
 */

static const uint8_t FONT_5X7[95][7] = {
    /* 32 ' '  */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 33 '!'  */ {0x04,0x04,0x04,0x04,0x00,0x00,0x04},
    /* 34 '"'  */ {0x0A,0x0A,0x00,0x00,0x00,0x00,0x00},
    /* 35 '#'  */ {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A},
    /* 36 '$'  */ {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04},
    /* 37 '%'  */ {0x18,0x19,0x02,0x04,0x08,0x13,0x03},
    /* 38 '&'  */ {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D},
    /* 39 '\'' */ {0x04,0x04,0x00,0x00,0x00,0x00,0x00},
    /* 40 '('  */ {0x02,0x04,0x08,0x08,0x08,0x04,0x02},
    /* 41 ')'  */ {0x08,0x04,0x02,0x02,0x02,0x04,0x08},
    /* 42 '*'  */ {0x00,0x04,0x15,0x0E,0x15,0x04,0x00},
    /* 43 '+'  */ {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},
    /* 44 ','  */ {0x00,0x00,0x00,0x00,0x06,0x04,0x08},
    /* 45 '-'  */ {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    /* 46 '.'  */ {0x00,0x00,0x00,0x00,0x00,0x06,0x06},
    /* 47 '/'  */ {0x01,0x02,0x02,0x04,0x08,0x08,0x10},
    /* 48 '0'  */ {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},
    /* 49 '1'  */ {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    /* 50 '2'  */ {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},
    /* 51 '3'  */ {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    /* 52 '4'  */ {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
    /* 53 '5'  */ {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    /* 54 '6'  */ {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},
    /* 55 '7'  */ {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    /* 56 '8'  */ {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},
    /* 57 '9'  */ {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    /* 58 ':'  */ {0x00,0x06,0x06,0x00,0x06,0x06,0x00},
    /* 59 ';'  */ {0x00,0x06,0x06,0x00,0x06,0x04,0x08},
    /* 60 '<'  */ {0x02,0x04,0x08,0x10,0x08,0x04,0x02},
    /* 61 '='  */ {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00},
    /* 62 '>'  */ {0x08,0x04,0x02,0x01,0x02,0x04,0x08},
    /* 63 '?'  */ {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},
    /* 64 '@'  */ {0x0E,0x11,0x01,0x0D,0x15,0x15,0x0E},
    /* 65 'A'  */ {0x04,0x0A,0x11,0x11,0x1F,0x11,0x11},
    /* 66 'B'  */ {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
    /* 67 'C'  */ {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    /* 68 'D'  */ {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},
    /* 69 'E'  */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    /* 70 'F'  */ {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
    /* 71 'G'  */ {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},
    /* 72 'H'  */ {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
    /* 73 'I'  */ {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    /* 74 'J'  */ {0x07,0x02,0x02,0x02,0x12,0x12,0x0C},
    /* 75 'K'  */ {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    /* 76 'L'  */ {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
    /* 77 'M'  */ {0x11,0x1B,0x15,0x11,0x11,0x11,0x11},
    /* 78 'N'  */ {0x11,0x19,0x15,0x13,0x11,0x11,0x11},
    /* 79 'O'  */ {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    /* 80 'P'  */ {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
    /* 81 'Q'  */ {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    /* 82 'R'  */ {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
    /* 83 'S'  */ {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
    /* 84 'T'  */ {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
    /* 85 'U'  */ {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    /* 86 'V'  */ {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04},
    /* 87 'W'  */ {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},
    /* 88 'X'  */ {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
    /* 89 'Y'  */ {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
    /* 90 'Z'  */ {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},
    /* 91 '['  */ {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E},
    /* 92 '\\' */ {0x10,0x08,0x08,0x04,0x02,0x02,0x01},
    /* 93 ']'  */ {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E},
    /* 94 '^'  */ {0x04,0x0A,0x11,0x00,0x00,0x00,0x00},
    /* 95 '_'  */ {0x00,0x00,0x00,0x00,0x00,0x00,0x1F},
    /* 96 '`'  */ {0x08,0x04,0x00,0x00,0x00,0x00,0x00},
    /* 97 'a'  */ {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F},
    /* 98 'b'  */ {0x10,0x10,0x1E,0x11,0x11,0x11,0x1E},
    /* 99 'c'  */ {0x00,0x00,0x0E,0x10,0x10,0x11,0x0E},
    /*100 'd'  */ {0x01,0x01,0x0F,0x11,0x11,0x11,0x0F},
    /*101 'e'  */ {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E},
    /*102 'f'  */ {0x06,0x09,0x08,0x1C,0x08,0x08,0x08},
    /*103 'g'  */ {0x00,0x00,0x0F,0x11,0x0F,0x01,0x0E},
    /*104 'h'  */ {0x10,0x10,0x1E,0x11,0x11,0x11,0x11},
    /*105 'i'  */ {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E},
    /*106 'j'  */ {0x02,0x00,0x06,0x02,0x02,0x12,0x0C},
    /*107 'k'  */ {0x10,0x10,0x12,0x14,0x18,0x14,0x12},
    /*108 'l'  */ {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E},
    /*109 'm'  */ {0x00,0x00,0x1A,0x15,0x15,0x11,0x11},
    /*110 'n'  */ {0x00,0x00,0x1E,0x11,0x11,0x11,0x11},
    /*111 'o'  */ {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E},
    /*112 'p'  */ {0x00,0x00,0x1E,0x11,0x1E,0x10,0x10},
    /*113 'q'  */ {0x00,0x00,0x0F,0x11,0x0F,0x01,0x01},
    /*114 'r'  */ {0x00,0x00,0x16,0x19,0x10,0x10,0x10},
    /*115 's'  */ {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E},
    /*116 't'  */ {0x08,0x08,0x1E,0x08,0x08,0x09,0x06},
    /*117 'u'  */ {0x00,0x00,0x11,0x11,0x11,0x13,0x0D},
    /*118 'v'  */ {0x00,0x00,0x11,0x11,0x11,0x0A,0x04},
    /*119 'w'  */ {0x00,0x00,0x11,0x11,0x15,0x15,0x0A},
    /*120 'x'  */ {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11},
    /*121 'y'  */ {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E},
    /*122 'z'  */ {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F},
    /*123 '{'  */ {0x02,0x04,0x04,0x08,0x04,0x04,0x02},
    /*124 '|'  */ {0x04,0x04,0x04,0x00,0x04,0x04,0x04},
    /*125 '}'  */ {0x08,0x04,0x04,0x02,0x04,0x04,0x08},
    /*126 '~'  */ {0x00,0x08,0x15,0x02,0x00,0x00,0x00},
};

TkImgBuf image_text_draw(TkImgBuf buf, const char *text,
                          int x, int y, int size, uint32_t color)
{
    TkImgBuf zero;
    memset(&zero, 0, sizeof(zero));

    if (!buf.data || buf.width == 0 || buf.height == 0 || !text || size < 1)
        return zero;

    uint64_t npix = (uint64_t)buf.width * buf.height;
    uint8_t  ch   = buf.channels;
    uint8_t *data = (uint8_t *)malloc(npix * ch);
    if (!data) return zero;
    memcpy(data, buf.data, npix * ch);

    /* Decompose the packed color word into per-channel bytes. */
    uint8_t cr = (uint8_t)((color >> 24) & 0xFF);
    uint8_t cg = (uint8_t)((color >> 16) & 0xFF);
    uint8_t cb = (uint8_t)((color >>  8) & 0xFF);
    uint8_t ca = (uint8_t)( color        & 0xFF);

    int cursor_x = x;
    int advance  = (5 + 1) * size; /* 5 columns + 1 pixel gap, scaled */

    for (const char *cp = text; *cp != '\0'; cp++) {
        unsigned int c = (unsigned int)(unsigned char)*cp;
        if (c < 32 || c > 126) {
            cursor_x += advance;
            continue;
        }
        const uint8_t *glyph = FONT_5X7[c - 32];

        for (int row = 0; row < 7; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 5; col++) {
                /* Bit 4 (0x10) is the leftmost column. */
                if (!(bits & (uint8_t)(0x10u >> (unsigned)col)))
                    continue;

                /* Scale each set pixel to a size×size block. */
                for (int sy = 0; sy < size; sy++) {
                    int py = y + row * size + sy;
                    if (py < 0 || py >= (int)buf.height) continue;
                    for (int sx = 0; sx < size; sx++) {
                        int px = cursor_x + col * size + sx;
                        if (px < 0 || px >= (int)buf.width) continue;

                        uint8_t *dst = data +
                            ((uint64_t)py * buf.width + (uint64_t)px) * ch;
                        if (ch >= 1) dst[0] = cr;
                        if (ch >= 2) dst[1] = cg;
                        if (ch >= 3) dst[2] = cb;
                        if (ch >= 4) dst[3] = ca;
                    }
                }
            }
        }
        cursor_x += advance;
    }

    TkImgBuf out;
    out.width    = buf.width;
    out.height   = buf.height;
    out.channels = buf.channels;
    out.data     = data;
    out.data_len = npix * ch;
    return out;
}

/* =========================================================================
 * Story 135.5 — document preprocessing: adaptive threshold, convolution,
 * and TIFF decode.
 *
 * Note on what was already here.  The story listed arbitrary-angle rotation
 * and a blur primitive as gaps.  image_rotate() (inverse-mapped bilinear)
 * and image_blur() have been in this file since 34.3.1; what was missing was
 * any way to reach them from toke — neither had a wrapper in image_glue.c
 * nor an entry in stdlib/image.tki.  Those two are therefore an exposure
 * change, not new C, and only the two below are new algorithms.
 * ========================================================================= */

/* Reduce any image to a single luminance plane of width*height bytes
 * (BT.601, matching image_to_grayscale).  Caller frees. */
static uint8_t *img_gray_plane(TkImgBuf buf)
{
    if (!buf.data || buf.width == 0 || buf.height == 0) return NULL;
    uint8_t  ch = buf.channels ? buf.channels : 1;
    uint64_t n  = (uint64_t)buf.width * buf.height;
    if (buf.data_len < n * ch) return NULL;

    uint8_t *g = (uint8_t *)malloc(n);
    if (!g) return NULL;
    for (uint64_t i = 0; i < n; i++) {
        const uint8_t *p = buf.data + i * ch;
        g[i] = (ch >= 3)
            ? (uint8_t)((299 * (int)p[0] + 587 * (int)p[1] + 114 * (int)p[2]) / 1000)
            : p[0];
    }
    return g;
}

static int img_finite(double v)
{
    return v == v && v > -1e308 && v < 1e308;
}

/* -------------------------------------------------------------------------
 * image_adaptive_threshold — Sauvola
 *
 * WHY SAUVOLA AND NOT A GLOBAL THRESHOLD OR A LOCAL MEAN.
 * A global threshold (Otsu and friends) picks one value for the whole page,
 * so a scan whose left half is in shadow loses either the text on the dark
 * side or the background on the light side.  That is the failure this
 * function exists to remove.
 * The cheapest local answer is Bradley/Wellner — threshold at the local mean
 * less a fixed percentage — but it has no notion of contrast, so over a blank
 * margin, where the mean is essentially the paper colour, roughly half the
 * pixels fall on each side and the margin fills with speckle.
 * Sauvola adds the local standard deviation: T = m(1 + k(s/R - 1)).  Where
 * there is text, s is large, the bracket approaches 1 and T approaches m.
 * Where the region is blank, s -> 0, the bracket falls to (1 - k) and T drops
 * below the background, so the whole region stays white.  That is exactly the
 * behaviour document binarisation needs, and it is why Sauvola is the
 * standard choice for scanned text.
 *
 * Cost.  Integral images of the values and of their squares make every window
 * mean and variance four array reads, so the running time does not depend on
 * the window size and a 31x31 window is no more expensive than a 3x3 one.
 * ------------------------------------------------------------------------- */

ImgResult image_adaptive_threshold(TkImgBuf buf, uint32_t window, double k)
{
    ImgResult res;
    memset(&res, 0, sizeof(res));

    if (!buf.data || buf.width == 0 || buf.height == 0) {
        res.is_err  = 1;
        res.err_msg = "image_adaptive_threshold: empty image";
        return res;
    }
    if (!img_finite(k) || k < -10.0 || k > 10.0) {
        res.is_err  = 1;
        res.err_msg = "image_adaptive_threshold: k must be finite and within "
                      "[-10, 10] (0.2 is the usual value)";
        return res;
    }

    if (window == 0)            window = 31;   /* default */
    if ((window & 1u) == 0)     window += 1;   /* an even window has no centre */
    if (window < 3)             window = 3;

    uint32_t w = buf.width, h = buf.height;
    uint64_t n = (uint64_t)w * h;

    uint8_t *gray = img_gray_plane(buf);
    if (!gray) {
        res.is_err  = 1;
        res.err_msg = "image_adaptive_threshold: pixel buffer shorter than "
                      "width*height*channels, or out of memory";
        return res;
    }

    /* (w+1) x (h+1) with a zero first row and column, so a window sum needs
     * no special case at the image edge. */
    uint64_t iw = (uint64_t)w + 1;
    uint64_t *sum = (uint64_t *)calloc((size_t)(iw * ((uint64_t)h + 1)), sizeof(uint64_t));
    uint64_t *sq  = (uint64_t *)calloc((size_t)(iw * ((uint64_t)h + 1)), sizeof(uint64_t));
    uint8_t  *out = (uint8_t *)malloc((size_t)n);
    if (!sum || !sq || !out) {
        free(gray); free(sum); free(sq); free(out);
        res.is_err  = 1;
        res.err_msg = "image_adaptive_threshold: out of memory";
        return res;
    }

    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint64_t v = gray[(uint64_t)y * w + x];
            uint64_t i = ((uint64_t)y + 1) * iw + (x + 1);
            sum[i] = v     + sum[i - 1] + sum[i - iw] - sum[i - iw - 1];
            sq[i]  = v * v + sq[i - 1]  + sq[i - iw]  - sq[i - iw - 1];
        }
    }

    int r = (int)(window / 2);
    const double R = 128.0;   /* the dynamic range of the standard deviation */

    for (uint32_t y = 0; y < h; y++) {
        int y0 = (int)y - r; if (y0 < 0) y0 = 0;
        int y1 = (int)y + r; if (y1 > (int)h - 1) y1 = (int)h - 1;
        for (uint32_t x = 0; x < w; x++) {
            int x0 = (int)x - r; if (x0 < 0) x0 = 0;
            int x1 = (int)x + r; if (x1 > (int)w - 1) x1 = (int)w - 1;

            uint64_t a = (uint64_t)y0 * iw + (uint64_t)x0;
            uint64_t b = (uint64_t)y0 * iw + (uint64_t)x1 + 1;
            uint64_t c = ((uint64_t)y1 + 1) * iw + (uint64_t)x0;
            uint64_t d = ((uint64_t)y1 + 1) * iw + (uint64_t)x1 + 1;

            /* Unsigned wraparound in the intermediate terms cancels: the
             * true inclusion-exclusion result is non-negative and in range. */
            double area = (double)(x1 - x0 + 1) * (double)(y1 - y0 + 1);
            double s1   = (double)(sum[d] - sum[b] - sum[c] + sum[a]);
            double s2   = (double)(sq[d]  - sq[b]  - sq[c]  + sq[a]);

            double mean = s1 / area;
            double var  = s2 / area - mean * mean;
            if (var < 0.0) var = 0.0;          /* rounding only */
            double t = mean * (1.0 + k * (sqrt(var) / R - 1.0));

            out[(uint64_t)y * w + x] =
                ((double)gray[(uint64_t)y * w + x] > t) ? 255 : 0;
        }
    }

    free(gray); free(sum); free(sq);

    res.ok.width    = w;
    res.ok.height   = h;
    res.ok.channels = 1;
    res.ok.data     = out;
    res.ok.data_len = n;
    return res;
}

/* -------------------------------------------------------------------------
 * image_convolve — arbitrary odd-sized kernel
 *
 * A general primitive rather than another fixed filter: image_blur and
 * image_sharpen are each one hard-coded 3x3 kernel, and every further one a
 * caller wants (Gaussian at a chosen sigma, unsharp mask, Sobel, Laplacian,
 * a motion-deblur estimate) would otherwise be a new C function.  The cost of
 * the general version is the same loop with the kernel read from memory.
 * ------------------------------------------------------------------------- */

#define IMG_KERNEL_MAX 63

ImgResult image_convolve(TkImgBuf buf, const double *kernel, uint32_t ksize,
                         double divisor, double offset)
{
    ImgResult res;
    memset(&res, 0, sizeof(res));

    if (!buf.data || buf.width == 0 || buf.height == 0) {
        res.is_err = 1; res.err_msg = "image_convolve: empty image"; return res;
    }
    if (!kernel) {
        res.is_err = 1; res.err_msg = "image_convolve: null kernel"; return res;
    }
    if (ksize == 0 || (ksize & 1u) == 0) {
        res.is_err = 1;
        res.err_msg = "image_convolve: kernel size must be odd (3, 5, 7, ...)";
        return res;
    }
    if (ksize > IMG_KERNEL_MAX) {
        res.is_err = 1;
        res.err_msg = "image_convolve: kernel size must be at most 63";
        return res;
    }
    if (!img_finite(divisor) || !img_finite(offset)) {
        res.is_err = 1;
        res.err_msg = "image_convolve: divisor and offset must be finite";
        return res;
    }

    uint8_t  ch     = buf.channels ? buf.channels : 1;
    uint32_t w      = buf.width, h = buf.height;
    uint64_t need   = (uint64_t)w * h * ch;
    if (buf.data_len < need) {
        res.is_err = 1;
        res.err_msg = "image_convolve: pixel buffer shorter than "
                      "width*height*channels";
        return res;
    }

    uint32_t kn = ksize * ksize;
    double   ksum = 0.0;
    for (uint32_t i = 0; i < kn; i++) {
        if (!img_finite(kernel[i])) {
            res.is_err = 1;
            res.err_msg = "image_convolve: kernel contains a non-finite value";
            return res;
        }
        ksum += kernel[i];
    }
    /* divisor == 0 means "normalise by the kernel sum".  An edge-detection
     * kernel sums to zero, so that case is left unscaled instead of dividing
     * by zero. */
    if (divisor == 0.0) divisor = (ksum == 0.0) ? 1.0 : ksum;

    uint8_t *out = (uint8_t *)malloc((size_t)need);
    if (!out) {
        res.is_err = 1; res.err_msg = "image_convolve: out of memory"; return res;
    }

    int      r      = (int)(ksize / 2);
    uint64_t stride = (uint64_t)w * ch;

    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            for (uint8_t c = 0; c < ch; c++) {
                double acc = 0.0;
                for (int ky = -r; ky <= r; ky++) {
                    int sy = (int)y + ky;
                    if (sy < 0) sy = 0;
                    if (sy >= (int)h) sy = (int)h - 1;
                    for (int kx = -r; kx <= r; kx++) {
                        int sx = (int)x + kx;
                        if (sx < 0) sx = 0;
                        if (sx >= (int)w) sx = (int)w - 1;
                        acc += kernel[(uint32_t)(ky + r) * ksize + (uint32_t)(kx + r)]
                             * (double)buf.data[(uint64_t)sy * stride
                                                + (uint64_t)sx * ch + c];
                    }
                }
                out[(uint64_t)y * stride + (uint64_t)x * ch + c] =
                    clamp_u8(acc / divisor + offset);
            }
        }
    }

    res.ok.width    = w;
    res.ok.height   = h;
    res.ok.channels = ch;
    res.ok.data     = out;
    res.ok.data_len = need;
    return res;
}

/* =========================================================================
 * TIFF decode, including multi-page (Story 135.5)
 *
 * WHY A DECODER IN THIS FILE RATHER THAN A VENDORED LIBRARY.
 * ADR-0015 makes the toolchain C99-only, and libtiff is a large dependency
 * with its own build system for a format whose baseline is small.  The
 * subset a document pipeline actually meets — an IFD chain, four
 * compressions, four photometric interpretations — is the code below, and it
 * reuses the DEFLATE inflater this file already carries for PNG.  If a
 * consumer later needs the parts that are genuinely large (CCITT G4, JPEG in
 * TIFF), that is the point to reconsider, and ADR-0015 names the two routes.
 *
 * WHAT IS SUPPORTED
 *   byte order      II and MM
 *   pages           the full IFD chain, addressed by 0-based index
 *   compression     1 (none), 5 (LZW), 8 / 32946 (Deflate), 32773 (PackBits)
 *   samples         1, 4, 8 and 16 bits; 16-bit is reduced to its high byte
 *   photometric     0 WhiteIsZero, 1 BlackIsZero, 2 RGB, 3 Palette
 *   predictor       1 (none) and 2 (horizontal differencing, 8-bit)
 *
 * WHAT IS REFUSED, BY NAME
 *   CCITT G3/G4 (2, 3, 4), JPEG (6, 7), tiled layouts, PlanarConfiguration 2
 *   and BigTIFF.  Each returns a message saying which, because "decode
 *   failed" on a fax-derived scan is the least useful answer available.
 * ========================================================================= */

typedef struct {
    const uint8_t *p;
    uint64_t       len;
    int            be;     /* 1 = MM (big-endian), 0 = II (little-endian) */
} TiffFile;

typedef struct {
    uint16_t tag;
    uint16_t type;
    uint32_t count;
    uint64_t voff;         /* absolute offset of the first value */
} TiffEntry;

static uint16_t tf_u16(const TiffFile *t, uint64_t off)
{
    if (off + 2 > t->len) return 0;
    const uint8_t *q = t->p + off;
    return t->be ? (uint16_t)(((uint16_t)q[0] << 8) | q[1])
                 : (uint16_t)(((uint16_t)q[1] << 8) | q[0]);
}

static uint32_t tf_u32(const TiffFile *t, uint64_t off)
{
    if (off + 4 > t->len) return 0;
    const uint8_t *q = t->p + off;
    return t->be
        ? ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3]
        : ((uint32_t)q[3] << 24) | ((uint32_t)q[2] << 16) | ((uint32_t)q[1] << 8) | q[0];
}

/* 0 = not a baseline TIFF; 2 = BigTIFF (magic 43), which is a different
 * container and is reported separately; 1 = usable. */
static int tiff_open(const uint8_t *bytes, uint64_t len, TiffFile *t)
{
    if (!bytes || len < 8) return 0;
    if      (bytes[0] == 'I' && bytes[1] == 'I') t->be = 0;
    else if (bytes[0] == 'M' && bytes[1] == 'M') t->be = 1;
    else return 0;
    t->p = bytes; t->len = len;
    uint16_t magic = tf_u16(t, 2);
    if (magic == 43) return 2;
    return (magic == 42) ? 1 : 0;
}

#define TIFF_MAX_PAGES 65536u

/* Walk the IFD chain.  Returns the page count; if `want` >= 0 and `found` is
 * non-NULL, *found receives the offset of that 0-based page (0 if absent). */
static uint32_t tiff_walk(const TiffFile *t, int64_t want, uint64_t *found)
{
    if (found) *found = 0;
    uint64_t off = tf_u32(t, 4);
    uint32_t n   = 0;

    while (off != 0 && off + 2 <= t->len && n < TIFF_MAX_PAGES) {
        uint16_t nent         = tf_u16(t, off);
        uint64_t next_off_pos = off + 2 + (uint64_t)nent * 12;
        if (next_off_pos + 4 > t->len) break;     /* truncated IFD */

        if (want >= 0 && (int64_t)n == want && found) *found = off;
        n++;

        uint64_t next = tf_u32(t, next_off_pos);
        if (next == off) break;                   /* self-link: malformed */
        off = next;
    }
    return n;
}

static uint32_t tiff_type_size(uint16_t ty)
{
    switch (ty) {
        case 1: case 2: case 6: case 7:   return 1;  /* BYTE ASCII SBYTE UNDEF */
        case 3: case 8:                   return 2;  /* SHORT SSHORT */
        case 4: case 9: case 11:          return 4;  /* LONG SLONG FLOAT */
        case 5: case 10: case 12:         return 8;  /* RATIONAL SRAT DOUBLE */
        default:                          return 0;
    }
}

static int tiff_find(const TiffFile *t, uint64_t ifd, uint16_t tag, TiffEntry *e)
{
    uint16_t nent = tf_u16(t, ifd);
    for (uint16_t i = 0; i < nent; i++) {
        uint64_t off = ifd + 2 + (uint64_t)i * 12;
        if (off + 12 > t->len) return 0;
        if (tf_u16(t, off) != tag) continue;

        e->tag   = tag;
        e->type  = tf_u16(t, off + 2);
        e->count = tf_u32(t, off + 4);
        uint32_t esz   = tiff_type_size(e->type);
        uint64_t total = (uint64_t)esz * e->count;
        /* Values of four bytes or fewer live in the entry itself. */
        e->voff = (total <= 4) ? (off + 8) : (uint64_t)tf_u32(t, off + 8);
        return 1;
    }
    return 0;
}

static uint32_t tiff_val(const TiffFile *t, const TiffEntry *e, uint32_t i)
{
    if (i >= e->count) return 0;
    uint32_t esz = tiff_type_size(e->type);
    uint64_t off = e->voff + (uint64_t)i * esz;
    switch (esz) {
        case 1:  return (off < t->len) ? t->p[off] : 0;
        case 2:  return tf_u16(t, off);
        case 4:  return tf_u32(t, off);
        default: return 0;
    }
}

static uint32_t tiff_tag1(const TiffFile *t, uint64_t ifd, uint16_t tag,
                          uint32_t dflt)
{
    TiffEntry e;
    if (!tiff_find(t, ifd, tag, &e) || e.count == 0) return dflt;
    return tiff_val(t, &e, 0);
}

/* ---- decompressors ------------------------------------------------------ */

/* PackBits (TIFF spec section 9): a run header n in [0,127] introduces n+1
 * literal bytes; n in [-1,-127] repeats the next byte 1-n times; -128 is a
 * no-op.  Short output is zero-filled rather than rejected — a trailing
 * partial strip is common and recoverable. */
static int tiff_packbits(const uint8_t *src, uint64_t srclen,
                         uint8_t *dst, uint64_t dstlen)
{
    uint64_t si = 0, di = 0;
    while (si < srclen && di < dstlen) {
        int8_t n = (int8_t)src[si++];
        if (n >= 0) {
            uint64_t cnt = (uint64_t)n + 1;
            if (si + cnt > srclen)  cnt = srclen - si;
            if (di + cnt > dstlen)  cnt = dstlen - di;
            memcpy(dst + di, src + si, (size_t)cnt);
            si += cnt; di += cnt;
        } else if (n != -128) {
            if (si >= srclen) break;
            uint64_t cnt = (uint64_t)(1 - (int)n);
            uint8_t  v   = src[si++];
            if (di + cnt > dstlen) cnt = dstlen - di;
            memset(dst + di, v, (size_t)cnt);
            di += cnt;
        }
    }
    if (di == 0) return -1;
    if (di < dstlen) memset(dst + di, 0, (size_t)(dstlen - di));
    return 0;
}

/* TIFF LZW (spec section 13).  MSB-first codes of 9..12 bits, 256 = Clear,
 * 257 = EndOfInformation, dictionary entries from 258.  TIFF's "early change"
 * quirk is the one difference from GIF LZW: the code width grows one code
 * sooner, when the next free code is 2^width - 1. */
#define TIFF_LZW_CLEAR 256u
#define TIFF_LZW_EOI   257u
#define TIFF_LZW_MAX   4096u

static int tiff_lzw(const uint8_t *src, uint64_t srclen,
                    uint8_t *dst, uint64_t dstlen)
{
    uint16_t *prefix = (uint16_t *)malloc(TIFF_LZW_MAX * sizeof(uint16_t));
    uint8_t  *suffix = (uint8_t  *)malloc(TIFF_LZW_MAX);
    uint8_t  *stack  = (uint8_t  *)malloc(TIFF_LZW_MAX + 1);
    if (!prefix || !suffix || !stack) {
        free(prefix); free(suffix); free(stack);
        return -1;
    }

    uint64_t di = 0, bitpos = 0, nbits = srclen * 8;
    uint32_t next = 258, width = 9;
    int64_t  prev = -1;

    while (bitpos + width <= nbits) {
        uint32_t code = 0;
        for (uint32_t b = 0; b < width; b++) {
            uint64_t bp = bitpos + b;
            code = (code << 1) | ((src[bp >> 3] >> (7 - (bp & 7))) & 1u);
        }
        bitpos += width;

        if (code == TIFF_LZW_EOI) break;
        if (code == TIFF_LZW_CLEAR) {
            next = 258; width = 9; prev = -1;
            continue;
        }

        /* Build the code's string on `stack`, last character first. */
        uint32_t sp = 0;
        if (code < next) {
            uint32_t c = code;
            while (c >= 258) {
                if (sp >= TIFF_LZW_MAX) goto lzw_fail;
                stack[sp++] = suffix[c];
                c = prefix[c];
            }
            stack[sp++] = (uint8_t)c;
        } else if (code == next && prev >= 0) {
            /* The KwKwK case: the code being read is the one this step is
             * about to define, so its string is string(prev) + first(prev).
             * Reversed, that is first(prev) followed by reverse(string(prev)),
             * so slot 0 is reserved and filled once the walk finds it. */
            sp = 1;
            uint32_t c = (uint32_t)prev;
            while (c >= 258) {
                if (sp >= TIFF_LZW_MAX) goto lzw_fail;
                stack[sp++] = suffix[c];
                c = prefix[c];
            }
            stack[sp++] = (uint8_t)c;
            stack[0] = stack[sp - 1];
        } else {
            goto lzw_fail;                 /* a code beyond the dictionary */
        }

        if (prev >= 0 && next < TIFF_LZW_MAX) {
            prefix[next] = (uint16_t)prev;
            suffix[next] = stack[sp - 1];  /* first character of this string */
            next++;
            if (next + 1 >= (1u << width) && width < 12) width++;  /* early change */
        }

        while (sp > 0) {
            uint8_t v = stack[--sp];
            if (di < dstlen) dst[di++] = v;
        }
        prev = (int64_t)code;
    }

    free(prefix); free(suffix); free(stack);
    if (di == 0) return -1;
    if (di < dstlen) memset(dst + di, 0, (size_t)(dstlen - di));
    return 0;

lzw_fail:
    free(prefix); free(suffix); free(stack);
    return -1;
}

/* Compression 8 and 32946 are both zlib streams (RFC 1950): a two-byte
 * header in front of the RFC 1951 payload this file already inflates. */
static int tiff_inflate(const uint8_t *src, uint64_t srclen,
                        uint8_t *dst, uint64_t dstlen)
{
    if (srclen < 3) return -1;
    OutBuf ob;
    if (inflate_all(src + 2, srclen - 2, &ob) != 0 || !ob.buf) {
        free(ob.buf);
        return -1;
    }
    uint64_t n = ob.len < dstlen ? ob.len : dstlen;
    memcpy(dst, ob.buf, (size_t)n);
    if (n < dstlen) memset(dst + n, 0, (size_t)(dstlen - n));
    free(ob.buf);
    return 0;
}

/* ---- sample extraction -------------------------------------------------- */

static uint32_t tiff_sample(const uint8_t *row, uint64_t row_len,
                            uint64_t idx, uint32_t bps, int be)
{
    switch (bps) {
        case 1:
            return (idx >> 3) < row_len
                 ? ((row[idx >> 3] >> (7 - (idx & 7))) & 1u) : 0;
        case 4:
            if ((idx >> 1) >= row_len) return 0;
            return (idx & 1) ? (row[idx >> 1] & 0x0Fu) : (uint32_t)(row[idx >> 1] >> 4);
        case 8:
            return idx < row_len ? row[idx] : 0;
        case 16:
            /* Reduce to 8 bits by taking the high byte, which is the first
             * byte in MM order and the second in II order. */
            if (idx * 2 + 1 >= row_len) return 0;
            return be ? row[idx * 2] : row[idx * 2 + 1];
        default:
            return 0;
    }
}

static uint8_t tiff_scale8(uint32_t v, uint32_t bps)
{
    switch (bps) {
        case 1:  return v ? 255 : 0;
        case 4:  return (uint8_t)(v * 17);   /* 0..15 spread over 0..255 */
        default: return (uint8_t)v;          /* 8, or the high byte of 16 */
    }
}

/* ---- public entry points ------------------------------------------------ */

int64_t image_tiff_pages(const uint8_t *bytes, uint64_t len)
{
    TiffFile t;
    if (tiff_open(bytes, len, &t) != 1) return -1;
    return (int64_t)tiff_walk(&t, -1, NULL);
}

ImgResult image_tiff_decode(const uint8_t *bytes, uint64_t len, uint32_t page)
{
    ImgResult res;
    memset(&res, 0, sizeof(res));
    res.is_err = 1;

    TiffFile t;
    int opened = tiff_open(bytes, len, &t);
    if (opened == 2) {
        res.err_msg = "image_tiff_decode: BigTIFF (magic 43) is not supported";
        return res;
    }
    if (opened != 1) {
        res.err_msg = "image_tiff_decode: not a TIFF file (expected II*\\0 or MM\\0*)";
        return res;
    }

    uint64_t ifd = 0;
    uint32_t npages = tiff_walk(&t, (int64_t)page, &ifd);
    if (ifd == 0) {
        res.err_msg = npages == 0
            ? "image_tiff_decode: no readable IFD in this TIFF"
            : "image_tiff_decode: page index is past the last page";
        return res;
    }

    if (tiff_tag1(&t, ifd, 322, 0) != 0 || tiff_tag1(&t, ifd, 324, 0) != 0) {
        res.err_msg = "image_tiff_decode: tiled TIFF is not supported "
                      "(only strip layouts)";
        return res;
    }

    uint32_t w      = tiff_tag1(&t, ifd, 256, 0);
    uint32_t h      = tiff_tag1(&t, ifd, 257, 0);
    uint32_t bps    = tiff_tag1(&t, ifd, 258, 1);
    uint32_t comp   = tiff_tag1(&t, ifd, 259, 1);
    uint32_t photo  = tiff_tag1(&t, ifd, 262, 1);
    uint32_t spp    = tiff_tag1(&t, ifd, 277, 1);
    uint32_t rps    = tiff_tag1(&t, ifd, 278, 0);
    uint32_t planar = tiff_tag1(&t, ifd, 284, 1);
    uint32_t pred   = tiff_tag1(&t, ifd, 317, 1);

    if (w == 0 || h == 0) {
        res.err_msg = "image_tiff_decode: page has zero width or height";
        return res;
    }
    if ((uint64_t)w * h > (1ull << 28)) {
        res.err_msg = "image_tiff_decode: page exceeds the 256-megapixel cap";
        return res;
    }
    if (comp == 2 || comp == 3 || comp == 4) {
        res.err_msg = "image_tiff_decode: CCITT G3/G4 fax compression is not "
                      "supported; re-encode as LZW, Deflate or PackBits";
        return res;
    }
    if (comp == 6 || comp == 7) {
        res.err_msg = "image_tiff_decode: JPEG-in-TIFF is not supported";
        return res;
    }
    if (comp != 1 && comp != 5 && comp != 8 && comp != 32946 && comp != 32773) {
        res.err_msg = "image_tiff_decode: unrecognised compression "
                      "(supported: 1 none, 5 LZW, 8/32946 Deflate, "
                      "32773 PackBits)";
        return res;
    }
    if (planar != 1) {
        res.err_msg = "image_tiff_decode: PlanarConfiguration 2 (separate "
                      "sample planes) is not supported";
        return res;
    }
    if (bps != 1 && bps != 4 && bps != 8 && bps != 16) {
        res.err_msg = "image_tiff_decode: only 1, 4, 8 and 16 bits per sample "
                      "are supported";
        return res;
    }
    if (spp == 0 || spp > 4) {
        res.err_msg = "image_tiff_decode: SamplesPerPixel must be 1..4";
        return res;
    }
    if (photo > 3) {
        res.err_msg = "image_tiff_decode: only WhiteIsZero, BlackIsZero, RGB "
                      "and Palette photometrics are supported";
        return res;
    }
    if (pred == 2 && bps != 8) {
        res.err_msg = "image_tiff_decode: Predictor 2 is supported only for "
                      "8-bit samples";
        return res;
    }
    if (pred != 1 && pred != 2) {
        res.err_msg = "image_tiff_decode: unsupported Predictor "
                      "(1 and 2 only)";
        return res;
    }

    /* Every sample must be the same width; a mixed-depth file would silently
     * decode as garbage otherwise. */
    TiffEntry bpse;
    if (tiff_find(&t, ifd, 258, &bpse)) {
        for (uint32_t i = 1; i < bpse.count && i < spp; i++) {
            if (tiff_val(&t, &bpse, i) != bps) {
                res.err_msg = "image_tiff_decode: mixed bits-per-sample is "
                              "not supported";
                return res;
            }
        }
    }

    TiffEntry cmap;
    int have_cmap = tiff_find(&t, ifd, 320, &cmap);
    if (photo == 3) {
        if (bps > 8) {
            res.err_msg = "image_tiff_decode: palette images must be 8 bits "
                          "per sample or fewer";
            return res;
        }
        if (!have_cmap || cmap.count < 3u * (1u << bps)) {
            res.err_msg = "image_tiff_decode: palette image with no usable "
                          "ColorMap";
            return res;
        }
    }

    uint8_t outch = (photo == 3) ? 3
                  : (photo == 2) ? (uint8_t)(spp >= 4 ? 4 : 3)
                  : 1;
    if (photo == 2 && spp < 3) {
        res.err_msg = "image_tiff_decode: RGB photometric with fewer than 3 "
                      "samples per pixel";
        return res;
    }

    TiffEntry so, sbc;
    if (!tiff_find(&t, ifd, 273, &so) || !tiff_find(&t, ifd, 279, &sbc)) {
        res.err_msg = "image_tiff_decode: page has no StripOffsets / "
                      "StripByteCounts";
        return res;
    }

    if (rps == 0 || rps > h) rps = h;
    uint32_t nstrips = (h + rps - 1) / rps;
    if (so.count < nstrips || sbc.count < nstrips) {
        res.err_msg = "image_tiff_decode: strip table is shorter than the "
                      "image";
        return res;
    }

    uint64_t src_row = ((uint64_t)w * spp * bps + 7) / 8;
    uint64_t out_len = (uint64_t)w * h * outch;

    uint8_t *out    = (uint8_t *)malloc((size_t)out_len);
    uint8_t *rowbuf = (uint8_t *)malloc((size_t)(src_row * rps));
    if (!out || !rowbuf) {
        free(out); free(rowbuf);
        res.err_msg = "image_tiff_decode: out of memory";
        return res;
    }
    memset(out, 0, (size_t)out_len);

    uint32_t ncol = (photo == 3) ? (1u << bps) : 0;

    for (uint32_t s = 0; s < nstrips; s++) {
        uint32_t rows = rps;
        if ((uint64_t)s * rps + rows > h) rows = (uint32_t)(h - (uint64_t)s * rps);
        uint64_t want = src_row * rows;

        uint64_t soff = tiff_val(&t, &so,  s);
        uint64_t scnt = tiff_val(&t, &sbc, s);
        if (soff >= len || soff + scnt > len) {
            free(out); free(rowbuf);
            res.err_msg = "image_tiff_decode: strip runs past the end of the "
                          "buffer";
            return res;
        }

        const uint8_t *sp = t.p + soff;
        int rc = 0;
        memset(rowbuf, 0, (size_t)want);
        if (comp == 1) {
            uint64_t n = scnt < want ? scnt : want;
            memcpy(rowbuf, sp, (size_t)n);
        } else if (comp == 32773) {
            rc = tiff_packbits(sp, scnt, rowbuf, want);
        } else if (comp == 5) {
            rc = tiff_lzw(sp, scnt, rowbuf, want);
        } else {
            rc = tiff_inflate(sp, scnt, rowbuf, want);
        }
        if (rc != 0) {
            free(out); free(rowbuf);
            res.err_msg = "image_tiff_decode: strip failed to decompress";
            return res;
        }

        /* Predictor 2: each sample is stored as a difference from the sample
         * one pixel to its left, so undo it before anything reads pixels. */
        if (pred == 2) {
            for (uint32_t ry = 0; ry < rows; ry++) {
                uint8_t *rw = rowbuf + (uint64_t)ry * src_row;
                for (uint64_t i = spp; i < src_row; i++)
                    rw[i] = (uint8_t)(rw[i] + rw[i - spp]);
            }
        }

        for (uint32_t ry = 0; ry < rows; ry++) {
            uint32_t gy = (uint32_t)((uint64_t)s * rps + ry);
            const uint8_t *rw = rowbuf + (uint64_t)ry * src_row;
            uint8_t *dr = out + (uint64_t)gy * w * outch;

            for (uint32_t x = 0; x < w; x++) {
                uint64_t base = (uint64_t)x * spp;
                if (photo == 3) {
                    uint32_t idx = tiff_sample(rw, src_row, base, bps, t.be);
                    if (idx >= ncol) idx = ncol - 1;
                    /* ColorMap is 3 * 2^bps SHORTs: all reds, then all
                     * greens, then all blues, each 0..65535. */
                    dr[(uint64_t)x * 3 + 0] =
                        (uint8_t)(tiff_val(&t, &cmap, idx) >> 8);
                    dr[(uint64_t)x * 3 + 1] =
                        (uint8_t)(tiff_val(&t, &cmap, ncol + idx) >> 8);
                    dr[(uint64_t)x * 3 + 2] =
                        (uint8_t)(tiff_val(&t, &cmap, 2 * ncol + idx) >> 8);
                } else if (photo == 2) {
                    for (uint8_t c = 0; c < outch; c++)
                        dr[(uint64_t)x * outch + c] =
                            tiff_scale8(tiff_sample(rw, src_row, base + c,
                                                    bps, t.be), bps);
                } else {
                    uint8_t v = tiff_scale8(
                        tiff_sample(rw, src_row, base, bps, t.be), bps);
                    /* WhiteIsZero stores 0 for white, so invert to the
                     * 0 = black convention the rest of this module uses. */
                    dr[x] = (photo == 0) ? (uint8_t)(255 - v) : v;
                }
            }
        }
    }

    free(rowbuf);

    res.is_err      = 0;
    res.err_msg     = NULL;
    res.ok.width    = w;
    res.ok.height   = h;
    res.ok.channels = outch;
    res.ok.data     = out;
    res.ok.data_len = out_len;
    return res;
}
