/*
 * DMAP track metadata parser (AirPlay audio SET_PARAMETER application/x-dmap-tagged). xbox-cast castcore.
 * Copyright (C) 2026 xbox-cast contributors. GPL-3.0-or-later (see LICENSE).
 */
#ifndef XC_DMAP_H
#define XC_DMAP_H

#include <stddef.h>
#include <stdint.h>

#define XC_DMAP_FIELD_MAX 256

typedef struct {
    char title[XC_DMAP_FIELD_MAX];    /* "minm", UTF-8, NUL-terminated, "" if absent */
    char artist[XC_DMAP_FIELD_MAX];   /* "asar" */
    char album[XC_DMAP_FIELD_MAX];    /* "asal" */
} xc_dmap_track;

/* Parses one DMAP buffer: an "mlit" container (8-byte header: 4-char tag + big-endian u32 length) holding tagged
   items, or a bare item list. Unknown tags are skipped; strings longer than the field are cut at a UTF-8 boundary.
   Returns the number of fields found (0..3), or -1 if the buffer is malformed. Never reads past len. */
int xc_dmap_parse_track(const uint8_t *buf, size_t len, xc_dmap_track *out);

#endif /* XC_DMAP_H */
