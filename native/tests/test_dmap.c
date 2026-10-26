/*
 * Unit test for src/dmap.c (xbox-cast castcore). GPL-3.0-or-later.
 */
#include <stdio.h>
#include <string.h>

#include "dmap.h"

static int failures;
#define CHECK(cond, what) do { int ok_ = (cond); printf("  %s %s\n", ok_ ? "ok  " : "FAIL", what); if (!ok_) failures++; } while (0)

static size_t item(uint8_t *p, const char *tag, const void *v, size_t n)
{
    memcpy(p, tag, 4);
    p[4] = (uint8_t) (n >> 24); p[5] = (uint8_t) (n >> 16); p[6] = (uint8_t) (n >> 8); p[7] = (uint8_t) n;
    if (n) memcpy(p + 8, v, n);
    return 8 + n;
}

int main(void)
{
    uint8_t inner[1024], buf[1100];
    xc_dmap_track t;
    size_t n = 0;
    uint32_t persistent_id = 0x01020304;

    printf("dmap: mlit with title/artist/album + unknown tags\n");
    n += item(inner + n, "mper", &persistent_id, 4);
    n += item(inner + n, "minm", "Song \xc3\xa9", 7);
    n += item(inner + n, "asar", "Artist", 6);
    n += item(inner + n, "asal", "Album", 5);
    n += item(inner + n, "asgn", "Genre", 5);
    size_t total = item(buf, "mlit", inner, n);
    CHECK(xc_dmap_parse_track(buf, total, &t) == 3, "3 fields");
    CHECK(!strcmp(t.title, "Song \xc3\xa9"), "title UTF-8");
    CHECK(!strcmp(t.artist, "Artist"), "artist");
    CHECK(!strcmp(t.album, "Album"), "album");

    printf("dmap: truncated buffer is rejected without reading past the end\n");
    CHECK(xc_dmap_parse_track(buf, total - 3, &t) == -1, "truncated -> -1");
    CHECK(xc_dmap_parse_track(buf, 5, &t) == -1, "short header -> -1");

    printf("dmap: bare item list, missing fields stay empty\n");
    n = item(buf, "minm", "Only title", 10);
    CHECK(xc_dmap_parse_track(buf, n, &t) == 1, "1 field");
    CHECK(!strcmp(t.title, "Only title") && !t.artist[0] && !t.album[0], "others empty");

    printf("dmap: long field is cut at a UTF-8 boundary\n");
    {
        uint8_t big[600];
        for (size_t i = 0; i + 1 < sizeof(big); i += 2) { big[i] = 0xc3; big[i + 1] = 0xa9; }   /* "é" x 300 */
        n = item(buf, "minm", big, sizeof(big));
        CHECK(xc_dmap_parse_track(buf, n, &t) == 1, "parsed");
        size_t l = strlen(t.title);
        CHECK(l < XC_DMAP_FIELD_MAX && l % 2 == 0, "even length (no split sequence)");
    }

    printf("dmap: empty / NULL\n");
    CHECK(xc_dmap_parse_track(buf, 0, &t) == 0, "empty -> 0");
    CHECK(xc_dmap_parse_track(NULL, 10, &t) == -1, "NULL -> -1");

    printf(failures ? "FAILED (%d)\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}
