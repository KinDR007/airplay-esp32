// DIDL-Lite metadata parser — best-effort extraction of title/artist/album +
// track duration from CurrentURIMetaData. The XML is wildly inconsistent
// across control points; we just string-scan for the tags we care about.

#include "dlna_internal.h"

#include <stdlib.h>
#include <string.h>

// Walk past optional namespace prefix to the close-tag bracket.
static const char *find_tag_text(const char *xml, const char *tag) {
  // Search for "<tag" or "<ns:tag" preceded by '<' or '<x:'
  size_t tlen = strlen(tag);
  for (const char *p = xml; *p; p++) {
    if (*p != '<') continue;
    const char *q = p + 1;
    // skip optional namespace "xx:"
    const char *colon = strchr(q, ':');
    const char *gt = strchr(q, '>');
    if (!gt) return NULL;
    const char *name = q;
    if (colon && colon < gt && (colon - q) < 16) {
      name = colon + 1;
    }
    if (!strncmp(name, tag, tlen) && (name[tlen] == '>' || name[tlen] == ' ')) {
      // Open tag found — return pointer to text after '>'.
      return gt + 1;
    }
  }
  return NULL;
}

static void extract(const char *xml, const char *tag, char *out, size_t cap) {
  const char *p = find_tag_text(xml, tag);
  if (!p) {
    out[0] = 0;
    return;
  }
  // Read until next '<'.
  size_t n = 0;
  while (*p && *p != '<' && n + 1 < cap) {
    out[n++] = *p++;
  }
  out[n] = 0;
  // Trim trailing whitespace.
  while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t' ||
                   out[n - 1] == '\n' || out[n - 1] == '\r'))
    out[--n] = 0;
}

// Parse "H:MM:SS" or "HH:MM:SS.fff" → seconds.
static uint32_t parse_duration(const char *s) {
  if (!s || !*s) return 0;
  unsigned h = 0, m = 0, sec = 0;
  if (sscanf(s, "%u:%u:%u", &h, &m, &sec) >= 2) {
    return h * 3600 + m * 60 + sec;
  }
  return 0;
}

void dlna_didl_extract(const char *xml, char *title, size_t title_cap,
                       char *artist, size_t artist_cap, char *album,
                       size_t album_cap, uint32_t *duration_secs) {
  if (!xml || !*xml) {
    if (title) title[0] = 0;
    if (artist) artist[0] = 0;
    if (album) album[0] = 0;
    if (duration_secs) *duration_secs = 0;
    return;
  }
  if (title) extract(xml, "title", title, title_cap);
  if (artist) {
    extract(xml, "artist", artist, artist_cap);
    if (!artist[0]) extract(xml, "creator", artist, artist_cap);
  }
  if (album) extract(xml, "album", album, album_cap);
  if (duration_secs) {
    // <res duration="0:04:01.000">...</res> — find duration="..."
    const char *p = strstr(xml, "duration=\"");
    if (p) {
      p += 10;
      char buf[24] = {0};
      size_t i = 0;
      while (*p && *p != '"' && i + 1 < sizeof(buf)) buf[i++] = *p++;
      *duration_secs = parse_duration(buf);
    } else {
      *duration_secs = 0;
    }
  }
}
