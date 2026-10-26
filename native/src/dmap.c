/*
 * DMAP track metadata parser. xbox-cast castcore. GPL-3.0-or-later (see LICENSE).
 * Tag meanings as in UxPlay's uxplay.cpp process_metadata() (GPL-3.0): minm = title, asar = artist, asal = album.
 */
#include "dmap.h"

#include <string.h>

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3];
}

/* copies at most cap-1 bytes, never splitting a UTF-8 sequence; drops embedded NULs */
static void copy_field(char *dst, size_t cap, const uint8_t *src, size_t n)
{
    size_t out = 0;
    if (n > cap - 1) {
        n = cap - 1;
        while (n > 0 && (src[n] & 0xC0) == 0x80) n--;   /* src[n] is the first byte cut off: back up to a lead byte */
    }
    for (size_t i = 0; i < n; i++) {
        if (src[i]) dst[out++] = (char) src[i];
    }
    dst[out] = '\0';
}

static int parse_items(const uint8_t *p, size_t len, xc_dmap_track *out, int depth)
{
    int found = 0;
    while (len > 0) {
        if (len < 8) return -1;
        const uint8_t *tag = p;
        uint32_t n = be32(p + 4);
        if (n > len - 8) return -1;
        const uint8_t *v = p + 8;
        if (!memcmp(tag, "mlit", 4)) {
            if (depth > 2) return -1;
            int r = parse_items(v, n, out, depth + 1);
            if (r < 0) return -1;
            found += r;
        } else if (!memcmp(tag, "minm", 4)) {
            copy_field(out->title, sizeof(out->title), v, n);
            found++;
        } else if (!memcmp(tag, "asar", 4)) {
            copy_field(out->artist, sizeof(out->artist), v, n);
            found++;
        } else if (!memcmp(tag, "asal", 4)) {
            copy_field(out->album, sizeof(out->album), v, n);
            found++;
        }
        p += 8 + n;
        len -= 8 + n;
    }
    return found;
}

int xc_dmap_parse_track(const uint8_t *buf, size_t len, xc_dmap_track *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!buf) return -1;
    return parse_items(buf, len, out, 0);
}
