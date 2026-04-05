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
 * JPEG, WebP, and BMP are intentionally stubbed.  Functions for those formats
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
#include <math.h>
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
    bs_align(bs);
    if (bs->byte_pos + 4 > bs->src_len) return -1;

    uint16_t len  = (uint16_t)(bs->src[bs->byte_pos] | ((uint16_t)bs->src[bs->byte_pos+1] << 8));
    uint16_t nlen = (uint16_t)(bs->src[bs->byte_pos+2] | ((uint16_t)bs->src[bs->byte_pos+3] << 8));
    bs->byte_pos += 4;
    bs->bits_avail = 0; bs->bits = 0; /* stream is now byte-aligned */

    if ((uint16_t)(len ^ nlen) != 0xFFFF) return -1;
    if (bs->byte_pos + len > bs->src_len) return -1;

    for (uint16_t i = 0; i < len; i++)
        outbuf_push(ob, bs->src[bs->byte_pos++]);

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
        res.err_msg = "image_decode: buffer too short to be a valid PNG";
        return res;
    }
    if (memcmp(bytes, PNG_SIG, 8) != 0) {
        res.is_err  = 1;
        res.err_msg = "image_decode: not a PNG file (bad signature)";
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
        res.is_err  = 1;
        res.err_msg = "BMP encode is not implemented in the minimal stdlib";
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
