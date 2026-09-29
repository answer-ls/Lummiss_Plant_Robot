#include "jpeg_integrity.h"
#include <string.h>

typedef struct {
    const uint8_t *data;
    size_t pos, end;
    unsigned bits, value;
    bool failed;
} reader_t;

/* 用移位寄存器批量取位；预取不足不是错误，区间尾仍可解析短码。 */
static void fill(reader_t *r, unsigned n)
{
    while (r->bits < n && r->pos < r->end) {
        unsigned byte = r->data[r->pos++];
        if (byte == 255 && (r->pos >= r->end || r->data[r->pos++] != 0)) {
            r->failed = true; return;
        }
        r->value = (r->value << 8) | byte;
        r->bits += 8;
    }
}

static unsigned take(reader_t *r, unsigned n)
{
    if (r->bits < n) fill(r, n);
    if (r->failed || r->bits < n) { r->failed = true; return 0; }
    r->bits -= n;
    return (r->value >> r->bits) & ((1U << n)-1);
}

static int symbol(reader_t *r, const jpeg_integrity_table_t *t)
{
    if (!t->valid) { r->failed = true; return -1; }
    if (r->bits < 8) fill(r, 8);
    if (r->failed) return -1;
    if (r->bits >= 8) {
        unsigned entry = t->lookup[(r->value >> (r->bits-8)) & 255];
        if (entry) {
            r->bits -= entry >> 8;
            return entry & 255;
        }
    }
    /* 长码及区间末尾不足八位时使用规范 Huffman 表回退。 */
    int code = 0;
    for (unsigned n = 1; n <= 16; ++n) {
        code = (code << 1) | take(r, 1);
        if (r->failed) return -1;
        if (t->last[n] >= 0 && code >= t->first[n] && code <= t->last[n])
            return t->values[t->base[n] + code - t->first[n]];
    }
    r->failed = true;
    return -1;
}

static bool block(reader_t *r, const jpeg_integrity_table_t *dc,
                  const jpeg_integrity_table_t *ac)
{
    int s = symbol(r, dc);
    if (s < 0 || s > 11) return false;
    take(r, s);
    for (unsigned k = 1; k < 64;) {
        int rs = symbol(r, ac);
        if (rs < 0 || r->failed) return false;
        if (!rs) break;
        if (rs == 0xf0) {
            k += 16;
            if (k > 64) return false;
        } else {
            unsigned run = rs >> 4, size = rs & 15;
            if (!size || size > 10 || k + run >= 64) return false;
            take(r, size);
            k += run + 1;
        }
    }
    return !r->failed;
}

static unsigned be16(const uint8_t *p) { return (p[0] << 8) | p[1]; }

bool jpeg_integrity_check(jpeg_integrity_workspace_t *w, const uint8_t *d,
                          size_t size, bool allow_padding, jpeg_integrity_result_t *out)
{
    *out = (jpeg_integrity_result_t){.reason = "header", .kind=JPEG_CHECK_HEADER};
    memset(w, 0, sizeof(*w));
    if (size < 4 || d[0] != 255 || d[1] != 216) return false;
    unsigned ids[3] = {0}, blocks[3] = {0}, dc[3] = {0}, ac[3] = {0};
    unsigned width = 0, height = 0, maxh = 0, maxv = 0, dri = 0;
    bool sof = false, sos = false;
    size_t p = 2;
    while (p + 4 <= size) {
        if (d[p++] != 255) return false;
        while (p < size && d[p] == 255) ++p;
        if (p + 3 > size) return false;
        unsigned marker = d[p++], len = be16(d+p);
        if (len < 2 || len > size-p) return false;
        const uint8_t *h = d+p+2;
        unsigned n = len-2;
        if (marker == 0xc0) {
            if (sof || n != 15 || h[0] != 8 || h[5] != 3) return false;
            height = be16(h+1); width = be16(h+3);
            for (unsigned c = 0; c < 3; ++c) {
                ids[c] = h[6+3*c];
                unsigned x = h[7+3*c] >> 4, y = h[7+3*c] & 15;
                if (!x || !y || x > 4 || y > 4) return false;
                for (unsigned j = 0; j < c; ++j) if (ids[j] == ids[c]) return false;
                blocks[c] = x*y;
                if (x > maxh) maxh = x;
                if (y > maxv) maxv = y;
            }
            if (blocks[0]+blocks[1]+blocks[2] > 10) return false;
            sof = true;
        } else if (marker == 0xc4) {
            unsigned j = 0;
            while (j < n) {
                if (n-j < 17) return false;
                unsigned info = h[j++], index = (info >> 4)*4+(info & 15);
                if ((info >> 4) > 1 || (info & 15) > 3) return false;
                jpeg_integrity_table_t *t = &w->tables[index];
                int code = 0, total = 0;
                for (unsigned b = 1; b <= 16; ++b) {
                    int count = h[j++];
                    t->first[b] = code; t->base[b] = total;
                    t->last[b] = count ? code+count-1 : -1;
                    if (code+count > (1 << b)) return false;
                    total += count; code = (code+count) << 1;
                }
                if (!total || total > 256 || (unsigned)total > n-j) return false;
                memcpy(t->values, h+j, total); j += total; t->valid = true;
                memset(t->lookup, 0, sizeof(t->lookup));
                for (unsigned b = 1; b <= 8; ++b) {
                    for (int c = t->first[b]; t->last[b] >= 0 && c <= t->last[b]; ++c) {
                        unsigned entry = (b << 8) | t->values[t->base[b]+c-t->first[b]];
                        unsigned start = (unsigned)c << (8-b);
                        for (unsigned k = 0; k < (1U << (8-b)); ++k) t->lookup[start+k] = entry;
                    }
                }
            }
        } else if (marker == 0xdd) {
            if (n != 2) return false;
            dri = be16(h);
        } else if (marker == 0xda) {
            if (!sof || n != 10 || h[0] != 3 || h[7] || h[8] != 63 || h[9]) return false;
            unsigned ordered[3], seen = 0;
            for (unsigned c = 0; c < 3; ++c) {
                unsigned k = 0;
                while (k < 3 && ids[k] != h[1+2*c]) ++k;
                if (k == 3 || (seen & (1U << k))) return false;
                seen |= 1U << k; ordered[c] = blocks[k];
                dc[c] = h[2+2*c] >> 4; ac[c] = (h[2+2*c]&15)+4;
                if (dc[c] > 3 || ac[c] > 7 || !w->tables[dc[c]].valid || !w->tables[ac[c]].valid) return false;
            }
            memcpy(blocks, ordered, sizeof(blocks));
            p += len; sos = true; break;
        } else if (marker != 0xdb && marker != 0xfe && !(marker >= 0xe0 && marker <= 0xef)) {
            /* 不支持的扫描格式拒绝送硬解，不能猜测其 MCU 布局。 */
            return false;
        }
        p += len;
    }
    if (!sos || !width || !height) return false;
    uint32_t total = ((width+8*maxh-1)/(8*maxh))*((height+8*maxv-1)/(8*maxv));
    uint32_t done = 0, segment = 0, padding_segments = 0;
    while (p < size) {
        size_t start = p, end = p;
        unsigned marker = 0;
        while (p < size) {
            if (d[p++] != 255) continue;
            end = p-1;
            while (p < size && d[p] == 255) ++p;
            if (p == size) { out->kind=JPEG_CHECK_RST; out->reason="restart"; return false; }
            marker = d[p++];
            if (marker) break;
        }
        *out = (jpeg_integrity_result_t){.reason="restart", .kind=JPEG_CHECK_RST,
            .padding_segments=padding_segments, .segment=segment,
            .expected=dri && total-done > dri ? dri : total-done, .offset=end};
        bool final = out->expected == total-done;
        if (marker != (final ? 0xd9 : 0xd0+(segment & 7))) return false;
        reader_t r = {.data=d, .pos=start, .end=end};
        out->reason = "mcu_entropy";
        out->kind = JPEG_CHECK_MCU;
        for (; out->mcus < out->expected; ++out->mcus) {
            for (unsigned c = 0; c < 3; ++c)
                for (unsigned b = 0; b < blocks[c]; ++b)
                    if (!block(&r, &w->tables[dc[c]], &w->tables[ac[c]])) {
                        out->offset = r.pos; return false;
                    }
        }
        /* 预取可能留有整字节；只有至多七位才属于填充，不能把额外数据放行。 */
        out->reason = "extra_data"; out->kind = JPEG_CHECK_EXTRA;
        if (r.pos != end || r.bits > 7) return false;
        if ((r.value & ((1U << r.bits)-1)) != ((1U << r.bits)-1)) {
            out->reason="padding_only"; out->kind=JPEG_CHECK_PADDING;
            out->padding_segments = ++padding_segments;
            if (!allow_padding) return false;
        }
        done += out->mcus;
        if (final) {
            if (p != size) { out->reason="extra_data"; out->kind=JPEG_CHECK_EXTRA; return false; }
            out->kind = padding_segments ? JPEG_CHECK_PADDING : JPEG_CHECK_OK;
            out->reason = padding_segments ? "padding_only" : "ok";
            return true;
        }
        ++segment;
    }
    return false;
}
