/*
 * Torrents — folder torrents over Reticulum (aurora/docs/torrents.md)
 *
 * The unit of sharing is a FOLDER, not a file, and its address is a key
 * (`nfolder1…`) rather than a hash of its contents — so a publisher can add or
 * remove files and every seeder converges on the new state under the SAME link.
 * Files inside stay content-addressed (sha256), exactly like BitTorrent: the
 * directory is mutable, the bytes are not.
 *
 * The tracker is the Indexer mesh: it answers "who has this folder" with a list
 * of devices and their physical profile (mains or battery, WiFi or cellular,
 * how recently we heard them), never with bytes. Pinning is how a device joins
 * the swarm: keep a full copy, follow the op-log, and advertise yourself as a
 * holder — which is what stops the publisher's phone from being the only source.
 *
 * All storage and networking is host-side behind hal_folder_* (create, browse,
 * download, pin, swarm, link). This module renders and applies policy.
 *
 * Performance (aurora/docs/performance.md): this wapp also runs in the
 * BACKGROUND with no page attached, so the tick must stay near-free. Every
 * render is diffed before it is sent (changed_send), the host HALs are polled on
 * long periods rather than every tick, and the swarm — whose refresh is a DHT
 * walk — is only asked about the torrent the user actually has open.
 */

#include <stdint.h>
#include "geogram_wasm_hal.h"

/* ── tiny libc (same helpers as the other wapps) ──────────────────────────── */
static unsigned s_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int s_eq(const char *a, const char *b) {
  while (*a && *b && *a == *b) { a++; b++; } return *a == *b;
}
static int s_pre(const char *s, const char *pre) {
  while (*pre) { if (*s != *pre) return 0; s++; pre++; } return 1;
}
static void s_cpy(char *d, const char *s, unsigned m) {
  unsigned i = 0; while (i < m - 1 && s[i]) { d[i] = s[i]; i++; } d[i] = 0;
}
static void s_cat(char *d, const char *s, unsigned m) {
  unsigned l = s_len(d), i = 0;
  while (l + i < m - 1 && s[i]) { d[l + i] = s[i]; i++; } d[l + i] = 0;
}
static void u_itoa(unsigned v, char *out) {
  char t[12]; int j = 0;
  if (v == 0) t[j++] = '0';
  while (v > 0) { t[j++] = (char)('0' + v % 10); v /= 10; }
  int k = 0; while (j > 0) out[k++] = t[--j];
  out[k] = 0;
}
static int to_int(const char *s) {
  int v = 0;
  while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
  return v;
}
/* JSON string escaping. The TAB matters: row ids carry "sha\tname", and a raw
 * control character inside a JSON string makes the WHOLE message unparseable —
 * the host drops it, the list never updates, and the wapp looks dead while
 * being perfectly alive. (That is exactly what happened: tapping a torrent did
 * nothing, and the log said "dropped unparseable message: Control character in
 * string".) jstr() already decodes \t; this is the missing other half. */
static void jesc(char *dst, unsigned m, const char *src) {
  unsigned l = s_len(dst);
  for (const char *p = src; *p && l < m - 3; p++) {
    char c = *p;
    if (c == '"' || c == '\\') { dst[l++] = '\\'; dst[l++] = c; }
    else if (c == '\n') { dst[l++] = '\\'; dst[l++] = 'n'; }
    else if (c == '\t') { dst[l++] = '\\'; dst[l++] = 't'; }
    else if (c == '\r') { dst[l++] = '\\'; dst[l++] = 'r'; }
    else if ((unsigned char)c < 0x20) { dst[l++] = ' '; }  /* never raw */
    else dst[l++] = c;
  }
  dst[l] = 0;
}
/* "key":"value" → out. Decodes \t and \n, which we use as field separators. */
static int jstr(const char *buf, const char *key, char *out, unsigned m) {
  char pat[80]; pat[0] = '"'; pat[1] = 0;
  s_cat(pat, key, sizeof(pat)); s_cat(pat, "\":\"", sizeof(pat));
  unsigned pl = s_len(pat);
  for (const char *p = buf; *p; p++) {
    int ok = 1;
    for (unsigned i = 0; i < pl; i++) { if (p[i] != pat[i]) { ok = 0; break; } }
    if (!ok) continue;
    p += pl; unsigned i = 0;
    while (*p && *p != '"' && i < m - 1) {
      if (*p == '\\' && *(p + 1)) {
        p++;
        char c = *p++;
        if (c == 'n') out[i++] = '\n';
        else if (c == 't') out[i++] = '\t';
        else if (c == 'r') out[i++] = '\r';
        else out[i++] = c;
      } else out[i++] = *p++;
    }
    out[i] = 0; return 1;
  }
  out[0] = 0; return 0;
}
static int jnum(const char *buf, const char *key) {
  char pat[80]; pat[0] = '"'; pat[1] = 0;
  s_cat(pat, key, sizeof(pat)); s_cat(pat, "\":", sizeof(pat));
  unsigned pl = s_len(pat);
  for (const char *p = buf; *p; p++) {
    int ok = 1;
    for (unsigned i = 0; i < pl; i++) { if (p[i] != pat[i]) { ok = 0; break; } }
    if (!ok) continue;
    return to_int(p + pl);
  }
  return 0;
}
static int jbool_def(const char *buf, const char *key, int def) {
  char pat[80]; pat[0] = '"'; pat[1] = 0;
  s_cat(pat, key, sizeof(pat)); s_cat(pat, "\":", sizeof(pat));
  unsigned pl = s_len(pat);
  for (const char *p = buf; *p; p++) {
    int ok = 1;
    for (unsigned i = 0; i < pl; i++) { if (p[i] != pat[i]) { ok = 0; break; } }
    if (!ok) continue;
    p += pl; while (*p == ' ') p++;
    return *p == 't' || *p == '1';
  }
  return def;
}
/* djb2 — change-detection so an unchanged list is never re-sent (a re-sent list
 * resets the user's scroll position, and costs a rebuild for nothing). */
static uint32_t djb2(const char *s) {
  uint32_t h = 5381;
  for (; *s; s++) h = ((h << 5) + h) ^ (unsigned char)*s;
  return h;
}
static int changed_send(const char *m, uint32_t *last) {
  uint32_t h = djb2(m);
  if (h == *last) return 0;
  *last = h;
  hal_msg_send(m, s_len(m));
  return 1;
}

static void notify(const char *level, const char *body) {
  char m[512] = "{\"type\":\"notify\",\"level\":\"";
  s_cat(m, level, sizeof(m));
  s_cat(m, "\",\"title\":\"Torrents\",\"body\":\"", sizeof(m));
  jesc(m, sizeof(m), body);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void log_clear(const char *field) {
  char m[80] = "{\"type\":\"ui.log.clear\",\"field\":\"";
  s_cat(m, field, sizeof(m)); s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void log_line(const char *field, const char *text) {
  char m[600] = "{\"type\":\"ui.log.append\",\"field\":\"";
  s_cat(m, field, sizeof(m));
  s_cat(m, "\",\"line\":\"", sizeof(m));
  jesc(m, sizeof(m), text);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void prompt_copy(const char *title, const char *body, const char *copyval) {
  char m[900] = "{\"type\":\"ui.prompt\",\"id\":\"noop\",\"title\":\"";
  jesc(m, sizeof(m), title);
  s_cat(m, "\",\"body\":\"", sizeof(m)); jesc(m, sizeof(m), body);
  s_cat(m, "\",\"copy\":\"", sizeof(m)); jesc(m, sizeof(m), copyval);
  s_cat(m, "\",\"confirm\":\"Close\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void prompt_input(const char *id, const char *title, const char *hint,
                         unsigned mx) {
  char m[400] = "{\"type\":\"ui.prompt\",\"id\":\"";
  s_cat(m, id, sizeof(m));
  s_cat(m, "\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), title);
  s_cat(m, "\",\"input\":{\"hint\":\"", sizeof(m)); jesc(m, sizeof(m), hint);
  s_cat(m, "\",\"max\":", sizeof(m));
  { char nb[12]; u_itoa(mx, nb); s_cat(m, nb, sizeof(m)); }
  s_cat(m, "},\"confirm\":\"OK\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Sizes the way a torrent client shows them. */
static void fmt_size(unsigned b, char *out, unsigned m) {
  char nb[16];
  out[0] = 0;
  if (b < 1024u) { u_itoa(b, nb); s_cat(out, nb, m); s_cat(out, " B", m); return; }
  if (b < 1024u * 1024u) {
    u_itoa(b / 1024u, nb); s_cat(out, nb, m); s_cat(out, " KB", m); return;
  }
  if (b < 1024u * 1024u * 1024u) {
    u_itoa(b / (1024u * 1024u), nb); s_cat(out, nb, m);
    s_cat(out, " MB", m); return;
  }
  u_itoa(b / (1024u * 1024u * 1024u), nb); s_cat(out, nb, m);
  s_cat(out, " GB", m);
}
/* "40s" / "12m" / "3h" — how long ago we heard a holder. */
static void fmt_age(unsigned ms, char *out, unsigned m) {
  char nb[16];
  unsigned secs = ms / 1000u;
  out[0] = 0;
  if (secs < 90u) { u_itoa(secs, nb); s_cat(out, nb, m); s_cat(out, "s", m); return; }
  if (secs < 5400u) { u_itoa(secs / 60u, nb); s_cat(out, nb, m); s_cat(out, "m", m); return; }
  u_itoa(secs / 3600u, nb); s_cat(out, nb, m); s_cat(out, "h", m);
}

/* ── state ───────────────────────────────────────────────────────────────── */
static char g_json[65536];   /* HAL replies */
static char g_out[65536];    /* ui.people.set payload */

/* 0 = the torrent list, 1 = inside one torrent. */
static int  g_view = 0;
static char g_cur[80] = "";        /* open folderId (hex) */
static char g_cur_name[120] = "";
static char g_cur_path[512] = "";  /* "" = root, else ends with '/' */
static char g_sel[80] = "";        /* the torrent a "..." menu was opened on */

static uint32_t g_list_hash = 0;
static unsigned g_tick = 0;

/* Settings (KV) */
static int g_pin_on_open = 1;
static int g_rescan_min = 15;

static void settings_save(void) {
  char b[24]; b[0] = 0;
  s_cat(b, g_pin_on_open ? "1" : "0", sizeof(b));
  { char nb[12]; u_itoa((unsigned)g_rescan_min, nb); s_cat(b, nb, sizeof(b)); }
  hal_kv_set("cfg", 3, b, s_len(b));
}
static void settings_load(void) {
  char b[24];
  uint32_t n = hal_kv_get("cfg", 3, b, sizeof(b) - 1);
  if (n < 2) return;
  b[n] = 0;
  g_pin_on_open = b[0] == '1';
  int r = to_int(b + 1);
  if (r >= 0 && r < 10000) g_rescan_min = r;
}

/* ── the torrent list ────────────────────────────────────────────────────── */

/* Copy the next JSON object at or after [p] into [slice] and return the cursor
 * just past it (NULL when there is none left). The host's folder HALs answer
 * with arrays of objects, so a brace counter is all the parsing we need — and
 * one iterator keeps every list walk in this file identical. */
static const char *next_obj(const char *p, char *slice, unsigned m) {
  if (!p) return 0;
  while (*p && *p != '{') p++;
  if (!*p) return 0;
  int depth = 0;
  unsigned i = 0;
  while (*p) {
    if (*p == '{') depth++;
    else if (*p == '}') depth--;
    if (i < m - 1) slice[i++] = *p;
    p++;
    if (depth == 0) break;
  }
  slice[i] = 0;
  return p;
}

/* One row of the torrent list, appended to g_out. */
static int g_first_row = 1;
static void row_open(void) {
  g_first_row = 1;
  g_out[0] = 0;
  s_cat(g_out, "{\"type\":\"ui.people.set\",\"field\":\"torrents\",\"sections\":[",
        sizeof(g_out));
}
static void section_open(const char *title) {
  if (!g_first_row) s_cat(g_out, "]},", sizeof(g_out));
  s_cat(g_out, "{\"title\":\"", sizeof(g_out));
  jesc(g_out, sizeof(g_out), title);
  s_cat(g_out, "\",\"items\":[", sizeof(g_out));
  g_first_row = 1;
}
/* A row, optionally with an icon. Pass icon=0 for a torrent (it keeps the
 * generated avatar, which makes one key distinguishable from another at a
 * glance); pass a name for a folder or a file, where a random coloured sigil
 * says nothing and the TYPE is the whole point. */
static void row_icon(const char *id, const char *title, const char *subtitle,
                     const char *icon) {
  if (!g_first_row) s_cat(g_out, ",", sizeof(g_out));
  g_first_row = 0;
  s_cat(g_out, "{\"id\":\"", sizeof(g_out)); jesc(g_out, sizeof(g_out), id);
  s_cat(g_out, "\",\"title\":\"", sizeof(g_out)); jesc(g_out, sizeof(g_out), title);
  s_cat(g_out, "\",\"subtitle\":\"", sizeof(g_out)); jesc(g_out, sizeof(g_out), subtitle);
  if (icon && icon[0]) {
    s_cat(g_out, "\",\"icon\":\"", sizeof(g_out));
    jesc(g_out, sizeof(g_out), icon);
  }
  s_cat(g_out, "\"}", sizeof(g_out));
}
static void row(const char *id, const char *title, const char *subtitle) {
  row_icon(id, title, subtitle, 0);
}

/* The icon a file's NAME earns it. The extension is all we have (the bytes may
 * not even be here yet), and it is what the OS routes on anyway. */
static const char *icon_for(const char *name) {
  const char *dot = 0;
  for (const char *p = name; *p; p++) {
    if (*p == '.') dot = p;
  }
  if (!dot || !dot[1]) return "insert_drive_file";
  char e[8];
  unsigned i = 0;
  for (const char *p = dot + 1; *p && i < sizeof(e) - 1; p++) {
    char c = *p;
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    e[i++] = c;
  }
  e[i] = 0;
  if (s_eq(e, "jpg") || s_eq(e, "jpeg") || s_eq(e, "png") || s_eq(e, "gif") ||
      s_eq(e, "webp") || s_eq(e, "bmp") || s_eq(e, "svg"))
    return "image";
  if (s_eq(e, "mp4") || s_eq(e, "mkv") || s_eq(e, "webm") || s_eq(e, "avi") ||
      s_eq(e, "mov"))
    return "movie";
  if (s_eq(e, "mp3") || s_eq(e, "ogg") || s_eq(e, "wav") || s_eq(e, "flac") ||
      s_eq(e, "opus") || s_eq(e, "m4a"))
    return "audiotrack";
  if (s_eq(e, "pdf")) return "picture_as_pdf";
  if (s_eq(e, "apk")) return "android";
  if (s_eq(e, "zip") || s_eq(e, "gz") || s_eq(e, "xz") || s_eq(e, "tar") ||
      s_eq(e, "7z") || s_eq(e, "rar"))
    return "archive";
  if (s_eq(e, "txt") || s_eq(e, "md") || s_eq(e, "log") || s_eq(e, "json"))
    return "description";
  return "insert_drive_file";
}

/* Tell the host we are INSIDE something, so the back arrow and the system-back
 * gesture come to us as `nav_back` (up one level) instead of leaving the wapp.
 * Clearing it at the root is what makes the next back exit, as it should. */
static void nav_set(int inside, const char *title) {
  char m[300] = "{\"type\":\"ui.nav\",\"back\":";
  s_cat(m, inside ? "true" : "false", sizeof(m));
  s_cat(m, ",\"title\":\"", sizeof(m));
  jesc(m, sizeof(m), title ? title : "");
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void row_close(void) {
  s_cat(g_out, "]}]}", sizeof(g_out));
}

/* One row's title + subtitle, from ONE hal_folder_stats call.
 *
 * hal_folder_stats is not free on the host (it reduces the signed op-log and
 * totals the serve counters), so this is a place where a lazy render turns into
 * a hot loop: two calls per row, every 6s, per torrent. It is called once per
 * row and the result is cached — a torrent's name and size do not change between
 * two adjacent frames (docs/performance.md §4.2, "a cosmetic value never
 * deserves a hot loop").
 */
#define STATS_CACHE 24
#define STATS_TTL_TICKS 30      /* ~30s; a rescan/download refreshes it anyway */
static char g_sc_fid[STATS_CACHE][80];
static char g_sc_title[STATS_CACHE][160];
static char g_sc_sub[STATS_CACHE][200];
static unsigned g_sc_at[STATS_CACHE];
static unsigned g_sc_used = 0;

/* Drop every cached row (after an edit/rescan/pin, where the numbers really did
 * change and the user is owed the truth immediately). */
static void stats_cache_clear(void) { g_sc_used = 0; }

static void torrent_row(const char *fid, int owned, int pinned,
                        char *title, unsigned tm, char *sub, unsigned sm) {
  for (unsigned i = 0; i < g_sc_used; i++) {
    if (!s_eq(g_sc_fid[i], fid)) continue;
    if (g_tick - g_sc_at[i] > STATS_TTL_TICKS) break;   /* stale: re-stat below */
    s_cpy(title, g_sc_title[i], tm);
    s_cpy(sub, g_sc_sub[i], sm);
    return;
  }

  char st[4096];
  uint32_t n = hal_folder_stats(fid, s_len(fid), st, sizeof(st) - 1);
  st[n] = 0;

  /* Title: the folder's PUBLISHED (signed) name, else the head of its key.
   * Never a name taken from a link — that one is unsigned, and an unsigned name
   * is a lie waiting to happen (docs/torrents.md §11). */
  char name[160];
  jstr(st, "name", name, sizeof(name));
  if (name[0]) {
    s_cpy(title, name, tm);
  } else {
    char npub[90];
    jstr(st, "npub", npub, sizeof(npub));
    s_cpy(title, npub[0] ? npub : fid, tm);
    if (s_len(title) > 20) title[20] = 0;
  }

  unsigned files = (unsigned)jnum(st, "fileCount");
  unsigned bytes = (unsigned)jnum(st, "totalBytes");
  unsigned serves = (unsigned)jnum(st, "serves");
  sub[0] = 0;
  s_cat(sub, owned ? "mine" : (pinned ? "pinned" : "following"), sm);
  { char nb[12]; u_itoa(files, nb);
    s_cat(sub, " - ", sm); s_cat(sub, nb, sm);
    s_cat(sub, files == 1 ? " file" : " files", sm); }
  { char fs[24]; fmt_size(bytes, fs, sizeof(fs));
    s_cat(sub, " - ", sm); s_cat(sub, fs, sm); }
  if (serves) {
    char nb[12]; u_itoa(serves, nb);
    s_cat(sub, " - served ", sm); s_cat(sub, nb, sm); s_cat(sub, "x", sm);
  }

  /* Cache it. Reuse this folder's slot if it has one, else append; when the
   * cache is full the oldest row goes — with a couple of dozen torrents open
   * that never happens, and if it does the cost is one extra stats call. */
  unsigned slot = g_sc_used;
  for (unsigned i = 0; i < g_sc_used; i++) {
    if (s_eq(g_sc_fid[i], fid)) { slot = i; break; }
  }
  if (slot == g_sc_used) {
    if (g_sc_used < STATS_CACHE) g_sc_used++;
    else {
      slot = 0;
      for (unsigned i = 1; i < g_sc_used; i++) {
        if (g_sc_at[i] < g_sc_at[slot]) slot = i;
      }
    }
  }
  s_cpy(g_sc_fid[slot], fid, sizeof(g_sc_fid[slot]));
  s_cpy(g_sc_title[slot], title, sizeof(g_sc_title[slot]));
  s_cpy(g_sc_sub[slot], sub, sizeof(g_sc_sub[slot]));
  g_sc_at[slot] = g_tick;
}

/* The list. Downloaded first — that is what a torrent client is FOR, and it is
 * the list the user came to look at; what this device publishes is the smaller,
 * rarer case and sits underneath.
 *
 * The section title carries NO count: the host's tab renders "<title> (n)" from
 * the number of rows, so a count written in here would be said twice. And no
 * placeholder rows — an "add your first torrent" row is an item, so it would
 * make an empty section report (1). An empty tab reports (0), honestly. */
static void render_list(void) {
  char slice[1024];

  /* Downloaded (someone else's key; we hold a copy of the contents). */
  uint32_t n = hal_folder_subs(g_json, sizeof(g_json) - 1);
  g_json[n] = 0;

  row_open();
  section_open("Downloaded");

  const char *p = next_obj(g_json, slice, sizeof(slice));
  while (p) {
    char fid[80];
    jstr(slice, "folderId", fid, sizeof(fid));
    int pinned = jbool_def(slice, "autoSync", 0);
    if (fid[0]) {
      char title[160], sub[200], rid[90] = "t:";
      torrent_row(fid, 0, pinned, title, sizeof(title), sub, sizeof(sub));
      s_cat(rid, fid, sizeof(rid));
      row(rid, title, sub);
    }
    p = next_obj(p, slice, sizeof(slice));
  }

  /* Mine (we hold the master key: only we can change what is in it). */
  n = hal_folder_list(g_json, sizeof(g_json) - 1);
  g_json[n] = 0;
  section_open("Mine");

  p = next_obj(g_json, slice, sizeof(slice));
  while (p) {
    char fid[80];
    jstr(slice, "folderId", fid, sizeof(fid));
    if (fid[0]) {
      char title[160], sub[200], rid[90] = "t:";
      torrent_row(fid, 1, 0, title, sizeof(title), sub, sizeof(sub));
      s_cat(rid, fid, sizeof(rid));
      row(rid, title, sub);
    }
    p = next_obj(p, slice, sizeof(slice));
  }

  row_close();
  changed_send(g_out, &g_list_hash);
}

/* Inside one torrent: this directory level only (the host keeps the payload —
 * and our work — proportional to one level, not to the whole folder). */
static void render_open(void) {
  char arg[600];
  s_cpy(arg, g_cur, sizeof(arg));
  s_cat(arg, "\t", sizeof(arg));
  s_cat(arg, g_cur_path, sizeof(arg));
  uint32_t n = hal_folder_browse(arg, s_len(arg), g_json, sizeof(g_json) - 1);
  g_json[n] = 0;

  char name[160];
  jstr(g_json, "name", name, sizeof(name));
  if (name[0]) s_cpy(g_cur_name, name, sizeof(g_cur_name));

  char title[300];
  s_cpy(title, g_cur_name[0] ? g_cur_name : "Torrent", sizeof(title));
  if (g_cur_path[0]) {
    s_cat(title, " / ", sizeof(title));
    s_cat(title, g_cur_path, sizeof(title));
  }

  /* Inside a torrent: the ONE back control (the AppBar arrow / the system-back
   * gesture) comes to us as `nav_back` and goes up a level. No ".." row — a
   * second back affordance on the same panel is clutter, and the user already
   * has one that works everywhere else in the app. */
  nav_set(1, g_cur_name[0] ? g_cur_name : "Torrent");

  row_open();
  section_open(title);

  char slice[1200];

  /* Subfolders: {"dirs":[{"name":..}]} — stop at the array's ']' so the files
   * that follow are not read as directories. */
  const char *d = g_json;
  while (*d && !s_pre(d, "\"dirs\":[")) d++;
  if (*d) {
    const char *end = d;
    while (*end && *end != ']') end++;
    const char *p = next_obj(d, slice, sizeof(slice));
    while (p && p <= end) {
      char dn[200];
      jstr(slice, "name", dn, sizeof(dn));
      if (dn[0]) {
        char rid[240] = "cd:";
        s_cat(rid, dn, sizeof(rid));
        row_icon(rid, dn, "folder", "folder");
      }
      p = next_obj(p, slice, sizeof(slice));
    }
  }

  /* Files: {"files":[{"x":sha,"base":leaf,"name":path,"size":n}]} */
  const char *f = g_json;
  while (*f && !s_pre(f, "\"files\":[")) f++;
  if (*f) {
    const char *p = next_obj(f, slice, sizeof(slice));
    while (p) {
      char sha[80], base[200], full[400];
      jstr(slice, "x", sha, sizeof(sha));
      jstr(slice, "base", base, sizeof(base));
      jstr(slice, "name", full, sizeof(full));
      unsigned size = (unsigned)jnum(slice, "size");
      if (sha[0]) {
        char sub[80];
        fmt_size(size, sub, sizeof(sub));
        char rid[520] = "f:";
        s_cat(rid, sha, sizeof(rid));
        s_cat(rid, "\t", sizeof(rid));
        s_cat(rid, full[0] ? full : base, sizeof(rid));
        row_icon(rid, base[0] ? base : sha, sub, icon_for(base));
      }
      p = next_obj(p, slice, sizeof(slice));
    }
  }

  row_close();
  changed_send(g_out, &g_list_hash);
}

static void render_current(void) {
  if (g_view == 1 && g_cur[0]) render_open();
  else render_list();
}

/* ── the swarm: who has this, and what are they made of ──────────────────── */
static void render_swarm(void) {
  log_clear("swarm_log");
  if (!g_cur[0]) {
    log_line("swarm_log", "Open a torrent first.");
    return;
  }
  uint32_t n = hal_folder_swarm(g_cur, s_len(g_cur), g_json, sizeof(g_json) - 1);
  g_json[n] = 0;

  int count = 0;
  char slice[1400];
  const char *p = next_obj(g_json, slice, sizeof(slice));
  while (p) {
    char dest[60], power[24], uplink[24], prov[16], region[24];
    jstr(slice, "dest", dest, sizeof(dest));
    jstr(slice, "power", power, sizeof(power));
    jstr(slice, "uplink", uplink, sizeof(uplink));
    jstr(slice, "provenance", prov, sizeof(prov));
    jstr(slice, "region", region, sizeof(region));
    unsigned heard = (unsigned)jnum(slice, "lastHeardMs");
    unsigned hops = (unsigned)jnum(slice, "hops");

    if (dest[0]) {
      char line[400] = "";
      char shortd[20];
      s_cpy(shortd, dest, sizeof(shortd));
      if (s_len(shortd) > 8) shortd[8] = 0;
      s_cat(line, shortd, sizeof(line));
      s_cat(line, "  ", sizeof(line));
      s_cat(line, power[0] ? power : "power?", sizeof(line));
      s_cat(line, "/", sizeof(line));
      s_cat(line, uplink[0] ? uplink : "uplink?", sizeof(line));
      if (hops) {
        char nb[12]; u_itoa(hops, nb);
        s_cat(line, "  ", sizeof(line)); s_cat(line, nb, sizeof(line));
        s_cat(line, hops == 1 ? " hop" : " hops", sizeof(line));
      }
      if (heard) {
        char ab[16]; fmt_age(heard, ab, sizeof(ab));
        s_cat(line, "  heard ", sizeof(line)); s_cat(line, ab, sizeof(line));
        s_cat(line, " ago", sizeof(line));
      }
      /* Provenance is not decoration: after Indexer-to-Indexer sync the
       * freshness we are quoting is second-hand, and the age of the information
       * is not the age of the device. */
      s_cat(line, s_eq(prov, "direct") ? " (heard directly)"
                                       : " (an Indexer told us)", sizeof(line));
      if (region[0]) {
        s_cat(line, "  region ", sizeof(line)); s_cat(line, region, sizeof(line));
      }
      log_line("swarm_log", line);
      count++;
    }
    p = next_obj(p, slice, sizeof(slice));
  }

  if (count == 0) {
    log_line("swarm_log", "No holders known yet.");
    log_line("swarm_log",
             "The DHT resolve runs in the background - reopen this in a moment.");
    log_line("swarm_log",
             "If it stays empty, nobody reachable is holding this folder: pin it "
             "and you become its first other copy.");
  } else {
    char l[120] = "";
    char nb[12]; u_itoa((unsigned)count, nb);
    s_cat(l, nb, sizeof(l));
    s_cat(l, count == 1 ? " holder. " : " holders. ", sizeof(l));
    s_cat(l, "Best first: mains + a fat uplink beats a phone on cellular.",
          sizeof(l));
    log_line("swarm_log", l);
  }
}

/* ── info + settings panels ──────────────────────────────────────────────── */
static void render_info(void) {
  log_clear("info_log");
  if (!g_cur[0]) {
    log_line("info_log", "Open a torrent first.");
    return;
  }
  char link[400];
  uint32_t n = hal_folder_link(g_cur, s_len(g_cur), link, sizeof(link) - 1);
  link[n] = 0;

  char st[4096];
  n = hal_folder_stats(g_cur, s_len(g_cur), st, sizeof(st) - 1);
  st[n] = 0;

  char name[160];
  jstr(st, "name", name, sizeof(name));
  if (name[0]) log_line("info_log", name);
  if (link[0]) log_line("info_log", link);

  char l[200] = "";
  { char nb[12]; u_itoa((unsigned)jnum(st, "fileCount"), nb);
    s_cat(l, nb, sizeof(l)); s_cat(l, " files, ", sizeof(l)); }
  { char fs[24]; fmt_size((unsigned)jnum(st, "totalBytes"), fs, sizeof(fs));
    s_cat(l, fs, sizeof(l)); }
  log_line("info_log", l);

  l[0] = 0;
  { char nb[12]; u_itoa((unsigned)jnum(st, "serves"), nb);
    s_cat(l, "served ", sizeof(l)); s_cat(l, nb, sizeof(l));
    s_cat(l, " times from this device", sizeof(l)); }
  log_line("info_log", l);
  log_line("info_log",
           jbool_def(st, "owned", 0)
               ? "You hold this folder's key: only you can change what's in it."
               : "Someone else holds this folder's key. You can read, host and "
                 "re-share it - you cannot change it.");
}

static void render_settings(void) {
  log_clear("settings_log");
  uint32_t n = hal_folder_subs(g_json, sizeof(g_json) - 1);
  g_json[n] = 0;
  int pinned = 0, subs = 0;
  char slice[1024];
  const char *p = next_obj(g_json, slice, sizeof(slice));
  while (p) {
    subs++;
    if (jbool_def(slice, "autoSync", 0)) pinned++;
    p = next_obj(p, slice, sizeof(slice));
  }
  char l[200] = "";
  char nb[12];
  u_itoa((unsigned)pinned, nb);
  s_cat(l, "Pinned: ", sizeof(l)); s_cat(l, nb, sizeof(l));
  u_itoa((unsigned)subs, nb);
  s_cat(l, " of ", sizeof(l)); s_cat(l, nb, sizeof(l));
  s_cat(l, " followed torrents", sizeof(l));
  log_line("settings_log", l);
  log_line("settings_log",
           "A pinned torrent is held in full and announced to the Indexers, so "
           "the swarm stops waking the publisher's phone.");
  log_line("settings_log",
           "Seeding continues while the app is in the background.");
}

/* ── manage menu ─────────────────────────────────────────────────────────── */
static void prompt_manage(void) {
  int pinned = 0;
  {
    /* Ask the host, not our own memory: the pin may have been set elsewhere. */
    uint32_t n = hal_folder_subs(g_json, sizeof(g_json) - 1);
    g_json[n] = 0;
    char slice[1024];
    const char *p = next_obj(g_json, slice, sizeof(slice));
    while (p) {
      char fid[80];
      jstr(slice, "folderId", fid, sizeof(fid));
      if (s_eq(fid, g_cur) && jbool_def(slice, "autoSync", 0)) pinned = 1;
      p = next_obj(p, slice, sizeof(slice));
    }
  }
  char m[900] = "{\"type\":\"ui.prompt\",\"id\":\"mng:";
  jesc(m, sizeof(m), g_cur);
  s_cat(m, "\",\"title\":\"Manage torrent\",\"body\":\"", sizeof(m));
  s_cat(m, pinned ? "Pinned: this device keeps a full copy and seeds it."
                  : "Not pinned: this device is not holding a copy for others.",
        sizeof(m));
  s_cat(m, "\",\"chips\":["
           "{\"label\":\"Download all\",\"value\":\"dlall\"},", sizeof(m));
  s_cat(m, pinned ? "{\"label\":\"Unpin\",\"value\":\"unpin\"},"
                  : "{\"label\":\"Pin (keep + seed)\",\"value\":\"pin\"},",
        sizeof(m));
  s_cat(m, "{\"label\":\"Copy link\",\"value\":\"link\"},"
           "{\"label\":\"Rescan\",\"value\":\"rescan\"},"
           "{\"label\":\"Remove\",\"value\":\"remove\"}],"
           "\"confirm\":\"Cancel\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

static void do_copy_link(void) {
  if (!g_cur[0]) { notify("info", "Open a torrent first"); return; }
  char link[400];
  uint32_t n = hal_folder_link(g_cur, s_len(g_cur), link, sizeof(link) - 1);
  link[n] = 0;
  if (!link[0]) { notify("warning", "No link yet"); return; }
  char body[600] = "Anyone with this link can read, download and re-host this "
                   "torrent - not change it:\n";
  s_cat(body, link, sizeof(body));
  prompt_copy(g_cur_name[0] ? g_cur_name : "Torrent link", body, link);
}

/* Open a torrent by any address a user might paste. */
static void open_by_id(const char *idOrLink) {
  char st[4096];
  uint32_t n = hal_folder_stats(idOrLink, s_len(idOrLink), st, sizeof(st) - 1);
  st[n] = 0;
  char fid[80];
  jstr(st, "folderId", fid, sizeof(fid));
  if (!fid[0]) {
    notify("warning", "That is not a folder link");
    return;
  }
  s_cpy(g_cur, fid, sizeof(g_cur));
  g_cur_name[0] = 0;
  g_cur_path[0] = 0;
  g_view = 1;
  /* Pull the signed op-log so the listing appears; the browse triggers it. */
  render_open();
  if (g_pin_on_open) {
    hal_folder_pin(fid, s_len(fid), 1);
    notify("info", "Pinned: downloading and seeding this torrent");
  } else {
    notify("info", "Opening torrent...");
  }
}

/* ── lifecycle ───────────────────────────────────────────────────────────── */
__attribute__((export_name("module_init")))
void module_init(void) {
  settings_load();
  /* Only render when somebody is looking. As a background service this engine's
   * ui.* messages are read by nobody, and building them still costs main-isolate
   * time — which is exactly the 300ms stall this check removed. Seeding, which
   * is the reason this wapp runs in the background at all, is host-side and
   * needs no render. */
  if (hal_ui_attached()) render_list();
  hal_log(1, "torrents: ready", 15);
}

__attribute__((export_name("module_tick")))
void module_tick(void) {
  g_tick++;

  /* Diffed and cached, but a render still costs a HAL round trip per torrent
   * (stats → the host reduces the signed op-log). Every 6s is plenty for a page
   * somebody has open, and it is skipped entirely when nobody does.
   * (docs/performance.md: "what does this cost per hour with the screen off,
   * and who is awake to see the result?") */
  if (g_tick % 6 == 0 && hal_ui_attached()) render_current();

  /* Republish what changed on disk, so a torrent shared from a directory tracks
   * the directory. The host diffs it against the signed op-log and only writes
   * an op when something actually moved. */
  if (g_rescan_min > 0 &&
      g_tick % (unsigned)(g_rescan_min * 60) == 0) {
    hal_folder_rescan("", 0);   /* all owned disk folders */
    stats_cache_clear();        /* the sizes may genuinely have moved */
  }
}

__attribute__((export_name("module_handle_event")))
void module_handle_event(void) {
  if (hal_msg_available() == 0) return;
  char buf[4096];
  uint32_t n = hal_msg_recv(buf, sizeof(buf) - 1);
  if (n == 0) return;
  buf[n] = 0;

  char cmd[40] = "", typ[24] = "";
  jstr(buf, "command", cmd, sizeof(cmd));
  jstr(buf, "type", typ, sizeof(typ));

  /* The picker came back: a directory becomes a torrent. */
  if (s_eq(typ, "fs.picked")) {
    char path[400] = "";
    jstr(buf, "path", path, sizeof(path));
    if (!path[0]) return;
    if (!jbool_def(buf, "dir", 0)) {
      notify("info", "Pick a folder: a torrent is a folder, not a single file");
      return;
    }
    if (hal_folder_add_disk(path, s_len(path))) {
      g_view = 0;
      g_cur[0] = 0;
      stats_cache_clear();
      notify("info", "Creating the torrent - hashing the folder...");
    } else {
      notify("warning", "Reticulum is still starting - try again in a moment");
    }
    return;
  }

  if (s_eq(typ, "prompt")) {
    char id[120] = "", val[400] = "", input[400] = "";
    jstr(buf, "id", id, sizeof(id));
    jstr(buf, "value", val, sizeof(val));
    jstr(buf, "input", input, sizeof(input));

    if (s_eq(id, "open")) {
      if (input[0]) open_by_id(input);
      return;
    }
    if (s_pre(id, "mng:")) {
      const char *fid = id + 4;
      if (s_eq(val, "dlall")) {
        const char *j = "{\"all\":true}";
        hal_folder_download(fid, s_len(fid), j, s_len(j));
        notify("info", "Downloading every file in this torrent");
      } else if (s_eq(val, "pin")) {
        hal_folder_pin(fid, s_len(fid), 1);
        stats_cache_clear();
        notify("info", "Pinned: keeping a full copy and telling the Indexers");
      } else if (s_eq(val, "unpin")) {
        hal_folder_pin(fid, s_len(fid), 0);
        stats_cache_clear();
        notify("info", "Unpinned: no longer keeping this in sync");
      } else if (s_eq(val, "link")) {
        do_copy_link();
      } else if (s_eq(val, "rescan")) {
        hal_folder_rescan(fid, s_len(fid));
        stats_cache_clear();
        notify("info", "Rescanning the folder on disk");
      } else if (s_eq(val, "remove")) {
        hal_folder_remove(fid, s_len(fid));
        stats_cache_clear();
        g_view = 0; g_cur[0] = 0; g_cur_path[0] = 0;
        notify("info", "Removed. The files on disk were not touched.");
        render_list();
      }
      return;
    }
    if (s_pre(id, "file:")) {
      /* "file:<sha>\t<name>" — the one file the user tapped. */
      const char *r = id + 5;
      char sha[80] = "", name[300] = "";
      unsigned i = 0;
      while (*r && *r != '\t' && i < sizeof(sha) - 1) sha[i++] = *r++;
      sha[i] = 0;
      if (*r == '\t') r++;
      i = 0;
      while (*r && i < sizeof(name) - 1) name[i++] = *r++;
      name[i] = 0;
      if (!sha[0] || !g_cur[0]) return;
      if (s_eq(val, "open")) {
        /* "sha\tname": the name carries the extension, which is what the OS
         * routes on. The host opens a disk-backed file in place and exports a
         * downloaded one off the UI isolate first. */
        char a[500] = "";
        s_cat(a, sha, sizeof(a));
        s_cat(a, "\t", sizeof(a));
        s_cat(a, name[0] ? name : sha, sizeof(a));
        hal_folder_open_file(g_cur, s_len(g_cur), a, s_len(a));
        notify("info", "Opening...");
      } else if (s_eq(val, "dl")) {
        char j[420] = "{\"sha\":\"";
        jesc(j, sizeof(j), sha);
        s_cat(j, "\",\"name\":\"", sizeof(j));
        jesc(j, sizeof(j), name[0] ? name : sha);
        s_cat(j, "\"}", sizeof(j));
        hal_folder_download(g_cur, s_len(g_cur), j, s_len(j));
        notify("info", "Downloading...");
      }
      return;
    }
    return;
  }

  /* ── actions ── */
  if (s_eq(cmd, "t_add_disk")) {
    const char *m = "{\"type\":\"fs.pick\",\"mode\":\"dir\","
                    "\"title\":\"Pick a folder to share as a torrent\"}";
    hal_msg_send(m, s_len(m));
  } else if (s_eq(cmd, "t_open_link")) {
    prompt_input("open", "Open a torrent", "nfolder1... / npub / hex id", 400);
  } else if (s_eq(cmd, "t_back") || s_eq(cmd, "nav_back")) {
    /* One back control, one sensible chain: subfolder -> its parent -> the
     * torrent list -> (nav cleared) out of the wapp. */
    if (g_view == 1 && g_cur_path[0]) {
      unsigned L = s_len(g_cur_path);
      g_cur_path[L - 1] = 0;
      int k = (int)s_len(g_cur_path) - 1;
      while (k >= 0 && g_cur_path[k] != '/') k--;
      g_cur_path[k + 1] = 0;
      render_open();
    } else {
      g_view = 0; g_cur[0] = 0; g_cur_path[0] = 0; g_cur_name[0] = 0;
      nav_set(0, "");     /* at the root: the next back leaves the wapp */
      render_list();
    }
  } else if (s_eq(cmd, "t_manage")) {
    if (g_cur[0]) prompt_manage();
    else notify("info", "Open a torrent first");
  } else if (s_eq(cmd, "swarm_refresh")) {
    render_swarm();
  } else if (s_eq(cmd, "copy_link")) {
    do_copy_link();
  } else if (s_eq(cmd, "copy_id")) {
    if (!g_cur[0]) { notify("info", "Open a torrent first"); return; }
    prompt_copy("Folder id (hex)", g_cur, g_cur);
  } else if (s_eq(cmd, "settings_apply")) {
    g_pin_on_open = jbool_def(buf, "pin_on_open", 1);
    char rb[16] = "";
    jstr(buf, "rescan_min", rb, sizeof(rb));
    if (rb[0]) {
      int v = to_int(rb);
      if (v >= 0 && v < 10000) g_rescan_min = v;
    }
    settings_save();
    render_settings();
    notify("info", "Saved");
  } else if (s_eq(cmd, "torrents_tap")) {
    char id[600] = "";
    jstr(buf, "torrents_id", id, sizeof(id));
    if (!id[0] || s_eq(id, "none")) return;

    if (s_pre(id, "t:")) {
      s_cpy(g_cur, id + 2, sizeof(g_cur));
      s_cpy(g_sel, g_cur, sizeof(g_sel));
      g_cur_name[0] = 0;
      g_cur_path[0] = 0;
      g_view = 1;
      render_open();
      render_swarm();   /* the swarm panel is about the torrent that is open */
      render_info();
    } else if (s_pre(id, "cd:")) {
      s_cat(g_cur_path, id + 3, sizeof(g_cur_path));
      s_cat(g_cur_path, "/", sizeof(g_cur_path));
      render_open();
    } else if (s_eq(id, "up:")) {
      unsigned L = s_len(g_cur_path);
      if (L) {
        g_cur_path[L - 1] = 0;
        int k = (int)s_len(g_cur_path) - 1;
        while (k >= 0 && g_cur_path[k] != '/') k--;
        g_cur_path[k + 1] = 0;
      }
      render_open();
    } else if (s_pre(id, "f:")) {
      /* one file: offer the download, and show the hash it will be checked against */
      const char *r = id + 2;
      char sha[80] = "", name[400] = "";
      unsigned i = 0;
      while (*r && *r != '\t' && i < sizeof(sha) - 1) sha[i++] = *r++;
      sha[i] = 0;
      if (*r == '\t') r++;
      i = 0;
      while (*r && i < sizeof(name) - 1) name[i++] = *r++;
      name[i] = 0;
      char m[900] = "{\"type\":\"ui.prompt\",\"id\":\"file:";
      jesc(m, sizeof(m), sha);
      s_cat(m, "\\t", sizeof(m));
      jesc(m, sizeof(m), name);
      s_cat(m, "\",\"title\":\"", sizeof(m));
      jesc(m, sizeof(m), name[0] ? name : "File");
      s_cat(m, "\",\"body\":\"Opens with whatever this device uses for that "
               "type. Every byte is checked against this hash before it is "
               "kept:\\n", sizeof(m));
      jesc(m, sizeof(m), sha);
      s_cat(m, "\",\"copy\":\"", sizeof(m));
      jesc(m, sizeof(m), sha);
      s_cat(m, "\",\"chips\":[{\"label\":\"Open\",\"value\":\"open\"},"
               "{\"label\":\"Download\",\"value\":\"dl\"}],"
               "\"confirm\":\"Close\"}", sizeof(m));
      hal_msg_send(m, s_len(m));
    }
  }
}

__attribute__((export_name("module_destroy")))
void module_destroy(void) {}

__attribute__((export_name("module_tick_interval_ms")))
uint32_t module_tick_interval_ms(void) { return 1000; }
