/*
 * Files — decentralized media archive (DESIGN.md)
 *
 * The control surface over the host's content-addressed media archive
 * (APRX.md §16): browse/search the files behind `file:<sha256>.<ext>`
 * tokens, add new files (picker → archive → shareable token), fetch hashes
 * announced by others, and switch on the two provider transports —
 * the Blossom-compatible HTTP endpoint and the BitTorrent seeder.
 *
 * All storage/networking machinery is host-side behind hal_media_* /
 * hal_share_*; this module renders UI and applies policy.
 */

#include <stdint.h>
#include "geogram_wasm_hal.h"

/* ── tiny libc (same helpers as the other wapps) ──────────────────────── */
static unsigned s_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int s_eq(const char *a, const char *b) {
  while (*a && *b && *a == *b) { a++; b++; } return *a == *b;
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
  int neg = 0, v = 0;
  if (*s == '-') { neg = 1; s++; }
  while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
  return neg ? -v : v;
}
static void jesc(char *dst, unsigned m, const char *src) {
  unsigned l = s_len(dst);
  for (const char *p = src; *p && l < m - 3; p++) {
    char c = *p;
    if (c == '"' || c == '\\') { dst[l++] = '\\'; dst[l++] = c; }
    else if (c == '\n') { dst[l++] = '\\'; dst[l++] = 'n'; }
    else dst[l++] = c;
  }
  dst[l] = 0;
}
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
      if (*p == '\\' && *(p + 1)) { p++; out[i++] = *p++; }
      else out[i++] = *p++;
    }
    out[i] = 0; return 1;
  }
  out[0] = 0; return 0;
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
/* "key":<number> → int (0 when absent). */
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

static void notify(const char *level, const char *body) {
  char m[512] = "{\"type\":\"notify\",\"level\":\"";
  s_cat(m, level, sizeof(m));
  s_cat(m, "\",\"title\":\"Files\",\"body\":\"", sizeof(m));
  jesc(m, sizeof(m), body);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void log_line(const char *field, const char *text) {
  char m[400] = "{\"type\":\"ui.log.append\",\"field\":\"";
  s_cat(m, field, sizeof(m));
  s_cat(m, "\",\"line\":\"", sizeof(m));
  jesc(m, sizeof(m), text);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void log_clear(const char *field) {
  char m[80] = "{\"type\":\"ui.log.clear\",\"field\":\"";
  s_cat(m, field, sizeof(m)); s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* "1.2 MB" style size for subtitles. */
static void fmt_size(unsigned b, char *out, unsigned m) {
  char nb[16];
  out[0] = 0;
  if (b < 1024) { u_itoa(b, nb); s_cat(out, nb, m); s_cat(out, " B", m); return; }
  if (b < 1024u * 1024u) {
    u_itoa(b / 1024u, nb); s_cat(out, nb, m); s_cat(out, " KB", m); return;
  }
  u_itoa(b / (1024u * 1024u), nb); s_cat(out, nb, m);
  s_cat(out, " MB", m);
}

/* ── sharing settings (persisted in KV) ─────────────────────────────────── */
static int g_blossom_on = 1;
static int g_uploads_on = 0;
static int g_seed_on = 1;
static int g_port = 3457;

static void share_save(void) {
  char b[40]; b[0] = 0;
  s_cat(b, g_blossom_on ? "1" : "0", sizeof(b));
  s_cat(b, g_uploads_on ? "1" : "0", sizeof(b));
  s_cat(b, g_seed_on ? "1" : "0", sizeof(b));
  char pb[12]; u_itoa((unsigned)g_port, pb);
  s_cat(b, pb, sizeof(b));
  hal_kv_set("share", 5, b, s_len(b));
}
static void share_load(void) {
  char b[40];
  uint32_t n = hal_kv_get("share", 5, b, sizeof(b) - 1);
  if (n < 4) return;
  b[n] = 0;
  g_blossom_on = b[0] == '1';
  g_uploads_on = b[1] == '1';
  g_seed_on = b[2] == '1';
  int p = to_int(b + 3);
  if (p > 0 && p < 65536) g_port = p;
}
/* Push the current settings to the host services. */
static void share_apply(void) {
  char m[160] = "{\"server\":";
  s_cat(m, g_blossom_on ? "true" : "false", sizeof(m));
  s_cat(m, ",\"port\":", sizeof(m));
  { char pb[12]; u_itoa((unsigned)g_port, pb); s_cat(m, pb, sizeof(m)); }
  s_cat(m, ",\"uploads\":", sizeof(m));
  s_cat(m, g_uploads_on ? "true" : "false", sizeof(m));
  s_cat(m, ",\"seed\":", sizeof(m));
  s_cat(m, g_seed_on ? "true" : "false", sizeof(m));
  s_cat(m, "}", sizeof(m));
  hal_share_ctl(m, s_len(m));
}

/* ── Library rendering (people list) ────────────────────────────────────── */
static char g_list[16384];   /* hal_media_* JSON */
static char g_out[16384];    /* ui.people.set message */

/* Current library view, so async refreshes (module_tick) keep what the user is
 * looking at: 0 = all files, 1 = search results, 2 = one folder, 3 = folder list. */
static int  g_view = 0;
static char g_query[160] = "";
static char g_dir_parent[96] = "";
static char g_dir_folder[96] = "";

static int s_pre(const char *s, const char *pre) {
  while (*pre) { if (*s != *pre) return 0; s++; pre++; } return 1;
}

/* Copy the raw `"tags":[...]` array out of one entry's JSON slice. */
static void copy_tags(const char *obj, const char *end, char *out, unsigned m) {
  s_cpy(out, "[]", m);
  for (const char *p = obj; p < end - 8; p++) {
    if (p[0]=='"'&&p[1]=='t'&&p[2]=='a'&&p[3]=='g'&&p[4]=='s'&&p[5]=='"'&&p[6]==':'&&p[7]=='[') {
      const char *q = p + 7;
      unsigned i = 0;
      while (q < end && *q != ']' && i < m - 2) out[i++] = *q++;
      if (i < m - 2) out[i++] = ']';
      out[i] = 0;
      return;
    }
  }
}

/* Render a hal_media_* JSON array (list / search / folder) into the people list. */
static void render_items(const char *json, const char *title) {
  char *m = g_out; const unsigned sz = sizeof(g_out);
  m[0] = 0;
  s_cat(m, "{\"type\":\"ui.people.set\",\"field\":\"library\",\"sections\":["
           "{\"title\":\"", sz);
  jesc(m, sz, title);
  s_cat(m, "\",\"items\":[", sz);
  int first = 1;
  /* Walk the JSON array entry by entry: each object starts at '{"sha256"'. */
  const char *p = json;
  while (p && *p) {
    if (!(p[0]=='{'&&p[1]=='"'&&p[2]=='s'&&p[3]=='h'&&p[4]=='a'&&p[5]=='2')) { p++; continue; }
    const char *end = p + 1;
    while (*end && !(end[0]=='}'&&end[1]==','&&end[2]=='{'&&end[3]=='"'&&end[4]=='s')) end++;
    if (*end) end++;            /* include the closing '}' */
    char slice[1600]; unsigned si = 0;
    for (const char *q = p; q < end && si < sizeof(slice) - 1; q++) slice[si++] = *q;
    slice[si] = 0;

    char token[80], name[96], ext[20], tags[200], folder[96];
    jstr(slice, "token", token, sizeof(token));
    jstr(slice, "name", name, sizeof(name));
    jstr(slice, "ext", ext, sizeof(ext));
    jstr(slice, "folder", folder, sizeof(folder));
    copy_tags(slice, slice + si, tags, sizeof(tags));
    unsigned size = (unsigned)jnum(slice, "size");
    unsigned dls = (unsigned)jnum(slice, "downloads");

    char sub[140]; sub[0] = 0;
    s_cat(sub, ".", sizeof(sub)); s_cat(sub, ext, sizeof(sub));
    s_cat(sub, " - ", sizeof(sub));
    { char fs[24]; fmt_size(size, fs, sizeof(fs)); s_cat(sub, fs, sizeof(sub)); }
    { char nb[12]; u_itoa(dls, nb);
      s_cat(sub, " - ", sizeof(sub)); s_cat(sub, nb, sizeof(sub));
      s_cat(sub, " dl", sizeof(sub)); }
    if (folder[0]) { s_cat(sub, " - ", sizeof(sub)); s_cat(sub, folder, sizeof(sub)); }

    if (!first) s_cat(m, ",", sz);
    first = 0;
    s_cat(m, "{\"id\":\"", sz); jesc(m, sz, token);
    s_cat(m, "\",\"title\":\"", sz); jesc(m, sz, name[0] ? name : token);
    s_cat(m, "\",\"subtitle\":\"", sz); jesc(m, sz, sub);
    s_cat(m, "\",\"tags\":", sz); s_cat(m, tags, sz);
    s_cat(m, "}", sz);

    p = end;
  }
  s_cat(m, "]}]}", sz);
  hal_msg_send(m, s_len(m));
}

static void render_library(void) {
  uint32_t n = hal_media_list(0, 100, g_list, sizeof(g_list) - 1);
  g_list[n] = 0;
  render_items(g_list, "Files");
}

static void render_search(void) {
  uint32_t n = hal_media_search(g_query, s_len(g_query), g_list, sizeof(g_list) - 1);
  g_list[n] = 0;
  char title[200]; title[0] = 0;
  s_cat(title, "Search: ", sizeof(title)); s_cat(title, g_query, sizeof(title));
  render_items(g_list, title);
}

static void render_folder(void) {
  char j[240] = "{\"parent\":\"";
  jesc(j, sizeof(j), g_dir_parent);
  s_cat(j, "\",\"folder\":\"", sizeof(j));
  jesc(j, sizeof(j), g_dir_folder);
  s_cat(j, "\"}", sizeof(j));
  uint32_t n = hal_media_list_folder(j, s_len(j), g_list, sizeof(g_list) - 1);
  g_list[n] = 0;
  char title[200]; title[0] = 0;
  if (g_dir_parent[0]) { s_cat(title, g_dir_parent, sizeof(title)); s_cat(title, " / ", sizeof(title)); }
  s_cat(title, g_dir_folder[0] ? g_dir_folder : "(uncategorized)", sizeof(title));
  render_items(g_list, title);
}

/* The virtual-folder tree as tappable rows (id "dir:<parent>\t<folder>"). */
static void render_folders(void) {
  uint32_t n = hal_media_folders(g_list, sizeof(g_list) - 1);
  g_list[n] = 0;
  char *m = g_out; const unsigned sz = sizeof(g_out);
  m[0] = 0;
  s_cat(m, "{\"type\":\"ui.people.set\",\"field\":\"library\",\"sections\":["
           "{\"title\":\"Folders\",\"items\":[", sz);
  int first = 1;
  const char *p = g_list;
  while (p && *p) {
    if (*p != '{') { p++; continue; }
    const char *end = p + 1;
    while (*end && *end != '}') end++;
    char slice[280]; unsigned si = 0;
    for (const char *q = p; q <= end && si < sizeof(slice) - 1; q++) slice[si++] = *q;
    slice[si] = 0;
    char parent[96], folder[96];
    jstr(slice, "parent", parent, sizeof(parent));
    jstr(slice, "folder", folder, sizeof(folder));
    unsigned cnt = (unsigned)jnum(slice, "count");
    char label[210]; label[0] = 0;
    if (parent[0]) { s_cat(label, parent, sizeof(label)); s_cat(label, " / ", sizeof(label)); }
    s_cat(label, folder[0] ? folder : "(uncategorized)", sizeof(label));
    char id[210]; id[0] = 0;
    s_cat(id, "dir:", sizeof(id)); s_cat(id, parent, sizeof(id));
    s_cat(id, "\t", sizeof(id)); s_cat(id, folder, sizeof(id));
    char cb[12]; u_itoa(cnt, cb);
    char subc[24]; subc[0] = 0; s_cat(subc, cb, sizeof(subc)); s_cat(subc, " files", sizeof(subc));
    if (!first) s_cat(m, ",", sz);
    first = 0;
    s_cat(m, "{\"id\":\"", sz); jesc(m, sz, id);
    s_cat(m, "\",\"title\":\"", sz); jesc(m, sz, label);
    s_cat(m, "\",\"subtitle\":\"", sz); jesc(m, sz, subc);
    s_cat(m, "\"}", sz);
    p = end + 1;
  }
  s_cat(m, "]}]}", sz);
  hal_msg_send(m, s_len(m));
}

static void render_current(void) {
  if (g_view == 1) render_search();
  else if (g_view == 2) render_folder();
  else if (g_view == 3) render_folders();
  else render_library();
}

/* ── status (Sharing panel log) ─────────────────────────────────────────── */
static void render_status(void) {
  char st[2048];
  uint32_t n = hal_share_status(st, sizeof(st) - 1);
  if (n == 0) return;
  st[n] = 0;
  log_clear("share_log");
  char line[120]; line[0] = 0;
  s_cat(line, "Blossom HTTP: ", sizeof(line));
  s_cat(line, jbool_def(st, "running", 0) ? "serving on port " : "stopped (port ",
        sizeof(line));
  { char pb[12]; u_itoa((unsigned)jnum(st, "port"), pb); s_cat(line, pb, sizeof(line)); }
  if (!jbool_def(st, "running", 0)) s_cat(line, ")", sizeof(line));
  log_line("share_log", line);

  line[0] = 0;
  s_cat(line, "Requests served: ", sizeof(line));
  { char nb[12]; u_itoa((unsigned)jnum(st, "requests"), nb); s_cat(line, nb, sizeof(line)); }
  s_cat(line, "  -  bytes: ", sizeof(line));
  { char fs[24]; fmt_size((unsigned)jnum(st, "bytes"), fs, sizeof(fs)); s_cat(line, fs, sizeof(line)); }
  log_line("share_log", line);

  /* one line per active torrent: "seed <token> peers:N" */
  const char *p = st;
  while (*p) {
    if (p[0]=='"'&&p[1]=='i'&&p[2]=='n'&&p[3]=='f'&&p[4]=='o'&&p[5]=='h') {
      char slice[400]; unsigned si = 0;
      const char *q = p;
      while (*q && *q != '}' && si < sizeof(slice) - 2) slice[si++] = *q++;
      slice[si] = 0;
      char tok[80], ih[48], prog[8];
      jstr(slice, "token", tok, sizeof(tok));
      jstr(slice, "infohash", ih, sizeof(ih));
      jstr(slice, "progress", prog, sizeof(prog));
      line[0] = 0;
      s_cat(line, jbool_def(slice, "seeding", 0) ? "seed " : "fetch ", sizeof(line));
      if (tok[0]) s_cat(line, tok, sizeof(line));
      else { s_cat(line, "ih:", sizeof(line)); s_cat(line, ih, sizeof(line)); }
      s_cat(line, " ", sizeof(line)); s_cat(line, prog, sizeof(line));
      s_cat(line, "% peers:", sizeof(line));
      { char nb[12]; u_itoa((unsigned)jnum(slice, "peers"), nb); s_cat(line, nb, sizeof(line)); }
      log_line("share_log", line);
      p = q;
    }
    p++;
  }
}

/* ── Folders (mutable, IPNS-like) ───────────────────────────────────────── */
static char g_cur_folder[80] = "";       /* hex/npub folderId open ("" = list) */
static char g_cur_folder_name[96] = "";
static int  g_mode = 0;                   /* 0 = folders, 1 = disk browser */
static char g_browse[300] = "/";          /* current browse path (disk add) */

/* find `"key":[` → pointer just after '[', or 0. */
static const char *find_arr(const char *buf, const char *key) {
  char pat[40]; pat[0] = '"'; pat[1] = 0;
  s_cat(pat, key, sizeof(pat)); s_cat(pat, "\":[", sizeof(pat));
  unsigned pl = s_len(pat);
  for (const char *p = buf; *p; p++) {
    int ok = 1;
    for (unsigned i = 0; i < pl; i++) { if (p[i] != pat[i]) { ok = 0; break; } }
    if (ok) return p + pl;
  }
  return 0;
}

static void render_owned(void) {
  uint32_t n = hal_folder_list(g_list, sizeof(g_list) - 1);
  g_list[n] = 0;
  char *m = g_out; const unsigned sz = sizeof(g_out);
  m[0] = 0;
  s_cat(m, "{\"type\":\"ui.people.set\",\"field\":\"folders\",\"sections\":["
           "{\"title\":\"My folders\",\"items\":[", sz);
  int first = 1;
  const char *p = g_list;
  while (p && *p) {
    if (*p != '{') { p++; continue; }
    const char *end = p + 1;
    while (*end && *end != '}') end++;
    char slice[300]; unsigned si = 0;
    for (const char *q = p; q <= end && si < sizeof(slice) - 1; q++) slice[si++] = *q;
    slice[si] = 0;
    char fid[80], nm[96], npub[80];
    jstr(slice, "folderId", fid, sizeof(fid));
    jstr(slice, "name", nm, sizeof(nm));
    jstr(slice, "npub", npub, sizeof(npub));
    if (fid[0]) {
      if (!first) s_cat(m, ",", sz);
      first = 0;
      s_cat(m, "{\"id\":\"own:", sz); jesc(m, sz, fid);
      s_cat(m, "\",\"title\":\"", sz); jesc(m, sz, nm[0] ? nm : "(folder)");
      s_cat(m, "\",\"subtitle\":\"", sz); jesc(m, sz, npub[0] ? npub : fid);
      s_cat(m, "\"}", sz);
    }
    p = end + 1;
  }
  s_cat(m, "]}]}", sz);
  hal_msg_send(m, s_len(m));
}

/* Append items from a JSON object-array (files/links). idkey = the id field
 * ('x' or 'f'); idprefix = the tap-id prefix; sub = a fixed subtitle ("" = id).
 * When [nameInId], the tap id becomes "<prefix><id>\t<name>" so the handler has
 * both the content hash and the file name. */
static void append_obj_items(char *m, unsigned sz, const char *arr,
                             const char *idkey, const char *idprefix,
                             const char *sub, int nameInId, int *first) {
  if (!arr) return;
  const char *p = arr;
  while (*p && *p != ']') {
    if (*p != '{') { p++; continue; }
    const char *end = p + 1;
    while (*end && *end != '}') end++;
    char slice[400]; unsigned si = 0;
    for (const char *q = p; q <= end && si < sizeof(slice) - 1; q++) slice[si++] = *q;
    slice[si] = 0;
    char idv[80], nm[120];
    jstr(slice, idkey, idv, sizeof(idv));
    jstr(slice, "name", nm, sizeof(nm));
    if (idv[0]) {
      if (!*first) s_cat(m, ",", sz);
      *first = 0;
      s_cat(m, "{\"id\":\"", sz); s_cat(m, idprefix, sz); jesc(m, sz, idv);
      if (nameInId) { s_cat(m, "\\t", sz); jesc(m, sz, nm); }
      s_cat(m, "\",\"title\":\"", sz); jesc(m, sz, nm[0] ? nm : idv);
      s_cat(m, "\",\"subtitle\":\"", sz);
      if (sub[0]) s_cat(m, sub, sz);
      else { char sh[18]; s_cpy(sh, idv, sizeof(sh)); jesc(m, sz, sh); }
      s_cat(m, "\"}", sz);
    }
    p = (*end) ? end + 1 : end;
  }
}

static void render_open(void) {
  uint32_t n = hal_folder_browse(g_cur_folder, s_len(g_cur_folder),
                                 g_list, sizeof(g_list) - 1);
  g_list[n] = 0;
  jstr(g_list, "name", g_cur_folder_name, sizeof(g_cur_folder_name));
  char *m = g_out; const unsigned sz = sizeof(g_out);
  m[0] = 0;
  s_cat(m, "{\"type\":\"ui.people.set\",\"field\":\"folders\",\"sections\":[", sz);
  s_cat(m, "{\"title\":\"", sz);
  jesc(m, sz, g_cur_folder_name[0] ? g_cur_folder_name : "Folder");
  s_cat(m, " - files\",\"items\":[", sz);
  int first = 1;
  append_obj_items(m, sz, find_arr(g_list, "files"), "x", "ffile:", "", 1, &first);
  s_cat(m, "]},{\"title\":\"Linked folders\",\"items\":[", sz);
  first = 1;
  append_obj_items(m, sz, find_arr(g_list, "links"), "f", "dir:", "folder", 0, &first);
  s_cat(m, "]}]}", sz);
  hal_msg_send(m, s_len(m));
}

/* Parent directory of [p] into [out]. */
static void parent_path(const char *p, char *out, unsigned m) {
  s_cpy(out, p, m);
  unsigned l = s_len(out);
  if (l <= 1) { s_cpy(out, "/", m); return; }
  if (out[l - 1] == '/') out[--l] = 0;
  while (l > 0 && out[l - 1] != '/') out[--l] = 0;
  if (l > 1 && out[l - 1] == '/') out[l - 1] = 0;
  if (out[0] == 0) s_cpy(out, "/", m);
}

/* In-app directory browser for "add folder from disk": directories as tappable
 * rows (id "cd:<path>"), with ".. (up)". The screen's "Use this folder" action
 * registers g_browse. */
static void render_browse(void) {
  uint32_t n = hal_fs_listdir(g_browse, s_len(g_browse), g_list, sizeof(g_list) - 1);
  g_list[n] = 0;
  char *m = g_out; const unsigned sz = sizeof(g_out);
  m[0] = 0;
  s_cat(m, "{\"type\":\"ui.people.set\",\"field\":\"folders\",\"sections\":["
           "{\"title\":\"Add folder: ", sz);
  jesc(m, sz, g_browse);
  s_cat(m, "\",\"items\":[", sz);
  int first = 1;
  if (!(g_browse[0] == '/' && g_browse[1] == 0)) {
    char par[300]; parent_path(g_browse, par, sizeof(par));
    s_cat(m, "{\"id\":\"cd:", sz); jesc(m, sz, par);
    s_cat(m, "\",\"title\":\".. (up)\",\"subtitle\":\"\"}", sz);
    first = 0;
  }
  const char *p = g_list;
  while (p && *p) {
    if (*p != '{') { p++; continue; }
    const char *end = p + 1;
    while (*end && *end != '}') end++;
    char slice[600]; unsigned si = 0;
    for (const char *q = p; q <= end && si < sizeof(slice) - 1; q++) slice[si++] = *q;
    slice[si] = 0;
    if (jbool_def(slice, "dir", 0)) {
      char name[200], pth[300];
      jstr(slice, "name", name, sizeof(name));
      jstr(slice, "path", pth, sizeof(pth));
      if (pth[0]) {
        if (!first) s_cat(m, ",", sz);
        first = 0;
        s_cat(m, "{\"id\":\"cd:", sz); jesc(m, sz, pth);
        s_cat(m, "\",\"title\":\"", sz); jesc(m, sz, name);
        s_cat(m, "\",\"subtitle\":\"folder\"}", sz);
      }
    }
    p = end + 1;
  }
  s_cat(m, "]}]}", sz);
  hal_msg_send(m, s_len(m));
}

/* Is auto-sync currently on for [fid]? (reads hal_folder_subs) */
static int folder_autosync_on(const char *fid) {
  char buf[2048];
  uint32_t n = hal_folder_subs(buf, sizeof(buf) - 1);
  buf[n] = 0;
  const char *p = buf;
  while (*p) {
    if (*p == '{') {
      const char *end = p + 1;
      while (*end && *end != '}') end++;
      char slice[400]; unsigned si = 0;
      for (const char *q = p; q <= end && si < sizeof(slice) - 1; q++) slice[si++] = *q;
      slice[si] = 0;
      char id[80]; jstr(slice, "folderId", id, sizeof(id));
      if (s_eq(id, fid)) return jbool_def(slice, "autoSync", 0);
      p = end + 1;
      continue;
    }
    p++;
  }
  return 0;
}

static void render_mfolders(void) {
  if (g_mode == 1) render_browse();
  else if (g_cur_folder[0]) render_open();
  else render_owned();
}

/* A single-input folder prompt; [id] is the result-id prefix, [folderId] is
 * appended so the result carries it. */
static void prompt_input1(const char *id, const char *folderId,
                          const char *title, const char *hint, unsigned mx) {
  char m[500] = "{\"type\":\"ui.prompt\",\"id\":\"";
  s_cat(m, id, sizeof(m)); jesc(m, sizeof(m), folderId);
  s_cat(m, "\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), title);
  s_cat(m, "\",\"input\":{\"hint\":\"", sizeof(m)); jesc(m, sizeof(m), hint);
  s_cat(m, "\",\"max\":", sizeof(m));
  { char nb[12]; u_itoa(mx, nb); s_cat(m, nb, sizeof(m)); }
  s_cat(m, "},\"confirm\":\"OK\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

static void prompt_folder_manage(void) {
  char m[900] = "{\"type\":\"ui.prompt\",\"id\":\"fmg:";
  jesc(m, sizeof(m), g_cur_folder);
  s_cat(m, "\",\"title\":\"", sizeof(m));
  jesc(m, sizeof(m), g_cur_folder_name[0] ? g_cur_folder_name : "Folder");
  s_cat(m, "\",\"body\":\"Folder id (share so others can browse):\\n", sizeof(m));
  jesc(m, sizeof(m), g_cur_folder);
  s_cat(m, "\",\"chips\":["
          "{\"label\":\"Download all\",\"value\":\"dlall\"},"
          "{\"label\":\"", sizeof(m));
  s_cat(m, folder_autosync_on(g_cur_folder) ? "Auto-sync: ON" : "Auto-sync: OFF",
        sizeof(m));
  s_cat(m, "\",\"value\":\"sync\"},"
          "{\"label\":\"Add file (owner)\",\"value\":\"addf\"},"
          "{\"label\":\"Rename (owner)\",\"value\":\"name\"},"
          "{\"label\":\"Description (owner)\",\"value\":\"desc\"},"
          "{\"label\":\"Link folder (owner)\",\"value\":\"link\"},"
          "{\"label\":\"Grant admin (owner)\",\"value\":\"grant\"},"
          "{\"label\":\"Revoke admin (owner)\",\"value\":\"revoke\"},"
          "{\"label\":\"Rescan disk (owner)\",\"value\":\"rescan\"}]}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

static void folder_edit_kv(const char *fid, const char *op, const char *key,
                           const char *val) {
  char j[600] = "{\"op\":\"";
  s_cat(j, op, sizeof(j)); s_cat(j, "\",\"", sizeof(j)); s_cat(j, key, sizeof(j));
  s_cat(j, "\":\"", sizeof(j)); jesc(j, sizeof(j), val); s_cat(j, "\"}", sizeof(j));
  hal_folder_edit(fid, s_len(fid), j, s_len(j));
}

/* ── prompts ────────────────────────────────────────────────────────────── */
static void prompt_search(void) {
  const char *m = "{\"type\":\"ui.prompt\",\"id\":\"fsearch\","
    "\"title\":\"Find a file\","
    "\"body\":\"Paste a file: token (found on the local network over Blossom) "
    "or a magnet: link (fetched from the BitTorrent swarm).\","
    "\"input\":{\"hint\":\"file:<sha256>.<ext>  or  magnet:?xt=...\",\"max\":600},"
    "\"confirm\":\"Fetch\"}";
  hal_msg_send(m, s_len(m));
}

/* Flatten the "tags":[...] array of one meta JSON into "a b c" for editing. */
static void tags_flat(const char *meta, char *out, unsigned m) {
  char arr[200]; copy_tags(meta, meta + s_len(meta), arr, sizeof(arr));
  unsigned o = 0;
  for (const char *p = arr; *p && o < m - 1; p++) {
    if (*p == '"' || *p == '[' || *p == ']') continue;
    out[o++] = (*p == ',') ? ' ' : *p;
  }
  out[o] = 0;
}

/* Set a single string metadata field via hal_media_set_meta. */
static void set_meta_kv(const char *token, const char *key, const char *val) {
  char j[400] = "{\"";
  s_cat(j, key, sizeof(j)); s_cat(j, "\":\"", sizeof(j));
  jesc(j, sizeof(j), val); s_cat(j, "\"}", sizeof(j));
  hal_media_set_meta(token, s_len(token), j, s_len(j));
}

/* A focused single-field editor, prefilled with the current value. [kind] is a
 * 2-char tag: nm name, ds description, tg tags, fd folder, pa parent. */
static void prompt_edit(const char *kind, const char *token,
                        const char *title, const char *cur, unsigned maxlen) {
  char m[1100] = "{\"type\":\"ui.prompt\",\"id\":\"f";
  s_cat(m, kind, sizeof(m)); s_cat(m, ":", sizeof(m));
  jesc(m, sizeof(m), token);
  s_cat(m, "\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), title);
  s_cat(m, "\",\"input\":{\"hint\":\"", sizeof(m)); jesc(m, sizeof(m), title);
  s_cat(m, "\",\"value\":\"", sizeof(m)); jesc(m, sizeof(m), cur);
  s_cat(m, "\",\"max\":", sizeof(m));
  { char nb[12]; u_itoa(maxlen, nb); s_cat(m, nb, sizeof(m)); }
  s_cat(m, "},\"confirm\":\"Save\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

static void prompt_details(const char *token) {
  char meta[1600];
  uint32_t n = hal_media_meta(token, s_len(token), meta, sizeof(meta) - 1);
  if (n == 0) return;
  meta[n] = 0;
  char name[96], desc[300], folder[96], parent[96];
  jstr(meta, "name", name, sizeof(name));
  jstr(meta, "description", desc, sizeof(desc));
  jstr(meta, "folder", folder, sizeof(folder));
  jstr(meta, "parent", parent, sizeof(parent));
  unsigned dls = (unsigned)jnum(meta, "downloads");

  char body[1100]; body[0] = 0;
  { char fs[24]; fmt_size((unsigned)jnum(meta, "size"), fs, sizeof(fs));
    s_cat(body, "Size: ", sizeof(body)); s_cat(body, fs, sizeof(body)); }
  { char nb[12]; u_itoa(dls, nb);
    s_cat(body, "   Downloads: ", sizeof(body)); s_cat(body, nb, sizeof(body)); }
  if (parent[0] || folder[0]) {
    s_cat(body, "\nFolder: ", sizeof(body));
    if (parent[0]) { s_cat(body, parent, sizeof(body)); s_cat(body, " / ", sizeof(body)); }
    s_cat(body, folder[0] ? folder : "(none)", sizeof(body));
  }
  if (desc[0]) { s_cat(body, "\n\n", sizeof(body)); s_cat(body, desc, sizeof(body)); }
  s_cat(body, "\n\nToken (paste into any chat):\n", sizeof(body));
  s_cat(body, token, sizeof(body));
  { char magnet[400];
    uint32_t mg = hal_media_magnet(token, s_len(token), magnet, sizeof(magnet) - 1);
    if (mg > 0) { magnet[mg] = 0;
      s_cat(body, "\n\nMagnet (BitTorrent):\n", sizeof(body));
      s_cat(body, magnet, sizeof(body)); } }

  char m[2000] = "{\"type\":\"ui.prompt\",\"id\":\"fdet:";
  jesc(m, sizeof(m), token);
  s_cat(m, "\",\"title\":\"", sizeof(m));
  jesc(m, sizeof(m), name[0] ? name : "File");
  s_cat(m, "\",\"body\":\"", sizeof(m));
  jesc(m, sizeof(m), body);
  s_cat(m, "\",\"chips\":["
          "{\"label\":\"Rename\",\"value\":\"nm\"},"
          "{\"label\":\"Description\",\"value\":\"ds\"},"
          "{\"label\":\"Tags\",\"value\":\"tg\"},"
          "{\"label\":\"Folder\",\"value\":\"fd\"},"
          "{\"label\":\"Parent\",\"value\":\"pa\"},"
          "{\"label\":\"Delete\",\"value\":\"del\"}]}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Parse a space-separated tag string into {"tags":[...]} and apply. */
static void apply_tags(const char *token, const char *tags) {
  char j[260] = "{\"tags\":[";
  const char *t = tags; int first = 1; char one[40]; unsigned oi = 0;
  for (;; t++) {
    char c = *t;
    if (c == ' ' || c == 0) {
      if (oi) {
        one[oi] = 0;
        if (!first) s_cat(j, ",", sizeof(j));
        s_cat(j, "\"", sizeof(j)); jesc(j, sizeof(j), one); s_cat(j, "\"", sizeof(j));
        first = 0; oi = 0;
      }
      if (!c) break;
    } else if (oi < sizeof(one) - 1) one[oi++] = c;
  }
  s_cat(j, "]}", sizeof(j));
  hal_media_set_meta(token, s_len(token), j, s_len(j));
  render_current();
}

/* Open the right single-field editor for a detail chip, prefilled. */
static void open_edit(const char *token, const char *which) {
  char meta[1600];
  uint32_t n = hal_media_meta(token, s_len(token), meta, sizeof(meta) - 1);
  meta[n] = 0;
  char cur[300] = "";
  if (s_eq(which, "nm")) { jstr(meta, "name", cur, sizeof(cur)); prompt_edit("nm", token, "Name", cur, 96); }
  else if (s_eq(which, "ds")) { jstr(meta, "description", cur, sizeof(cur)); prompt_edit("ds", token, "Description (max 250)", cur, 250); }
  else if (s_eq(which, "tg")) { tags_flat(meta, cur, sizeof(cur)); prompt_edit("tg", token, "Tags (space-separated)", cur, 120); }
  else if (s_eq(which, "fd")) { jstr(meta, "folder", cur, sizeof(cur)); prompt_edit("fd", token, "Folder name", cur, 96); }
  else if (s_eq(which, "pa")) { jstr(meta, "parent", cur, sizeof(cur)); prompt_edit("pa", token, "Parent folder", cur, 96); }
}

/* ── prompt results / commands ──────────────────────────────────────────── */
static void do_prompt_result(const char *buf) {
  char pid[280] = "", val[24] = "";
  jstr(buf, "prompt_id", pid, sizeof(pid));
  jstr(buf, "prompt_value", val, sizeof(val));
  char inp[320] = "";
  jstr(buf, "prompt_input", inp, sizeof(inp));

  if (s_eq(pid, "fsearch")) {
    /* A magnet link can be long, so re-read prompt_input into a big buffer. */
    char big[600] = "";
    jstr(buf, "prompt_input", big, sizeof(big));
    if (!big[0]) return;
    if (big[0]=='m'&&big[1]=='a'&&big[2]=='g'&&big[3]=='n'&&big[4]=='e'&&big[5]=='t') {
      hal_media_fetch_magnet(big, s_len(big), "", 0);
      notify("info", "Joining the BitTorrent swarm…");
    } else if (hal_media_fetch(big, s_len(big))) {
      notify("info", "Looking on the local network for the file…");
    } else {
      notify("warning", "Paste a file: token or a magnet: link");
    }
    return;
  }
  if (s_pre(pid, "fdet:")) {
    const char *token = pid + 5;
    if (s_eq(val, "del")) {
      hal_media_delete(token, s_len(token));
      render_current();
      notify("info", "Deleted from the archive");
    } else {
      open_edit(token, val);
    }
    return;
  }
  if (s_pre(pid, "fnm:")) { set_meta_kv(pid + 4, "name", inp); render_current(); notify("info", "Saved"); return; }
  if (s_pre(pid, "fds:")) { if (s_len(inp) > 250) inp[250] = 0; set_meta_kv(pid + 4, "description", inp); render_current(); notify("info", "Saved"); return; }
  if (s_pre(pid, "ftg:")) { apply_tags(pid + 4, inp); notify("info", "Saved"); return; }
  if (s_pre(pid, "ffd:")) { set_meta_kv(pid + 4, "folder", inp); render_current(); notify("info", "Saved"); return; }
  if (s_pre(pid, "fpa:")) { set_meta_kv(pid + 4, "parent", inp); render_current(); notify("info", "Saved"); return; }

  // ── Mutable folders ──
  if (s_eq(pid, "fnew")) {
    if (!inp[0]) return;
    char j[200] = "{\"name\":\""; jesc(j, sizeof(j), inp); s_cat(j, "\"}", sizeof(j));
    char id[80]; uint32_t n = hal_folder_create(j, s_len(j), id, sizeof(id) - 1);
    id[n] = 0;
    if (id[0]) {
      s_cpy(g_cur_folder, id, sizeof(g_cur_folder));
      s_cpy(g_cur_folder_name, inp, sizeof(g_cur_folder_name));
      render_mfolders();
      notify("info", "Folder created");
    } else {
      notify("warning", "Could not create folder (is the network on?)");
    }
    return;
  }
  if (s_eq(pid, "fopen")) {
    if (!inp[0]) return;
    s_cpy(g_cur_folder, inp, sizeof(g_cur_folder));
    g_cur_folder_name[0] = 0;
    render_mfolders();
    return;
  }
  if (s_pre(pid, "fmg:")) {
    const char *fid = pid + 4;
    if (s_eq(val, "addf")) prompt_input1("fadf:", fid, "Add file (token or sha256)", "file:... or sha256", 120);
    else if (s_eq(val, "name")) prompt_input1("fnm2:", fid, "Folder name", "name", 96);
    else if (s_eq(val, "desc")) prompt_input1("fds2:", fid, "Description", "description", 250);
    else if (s_eq(val, "link")) prompt_input1("flnk:", fid, "Link a folder (id or npub)", "folder id / npub", 120);
    else if (s_eq(val, "grant")) prompt_input1("fgr:", fid, "Grant admin (npub or hex)", "npub / hex pubkey", 120);
    else if (s_eq(val, "revoke")) prompt_input1("frv:", fid, "Revoke admin (npub or hex)", "npub / hex pubkey", 120);
    else if (s_eq(val, "rescan")) { hal_folder_rescan(fid, s_len(fid)); notify("info", "Rescanning disk..."); }
    else if (s_eq(val, "dlall")) {
      char j[] = "{\"all\":true}";
      hal_folder_download(fid, s_len(fid), j, s_len(j));
      notify("info", "Downloading all files...");
    } else if (s_eq(val, "sync")) {
      int on = folder_autosync_on(fid) ? 0 : 1;
      hal_folder_autosync(fid, s_len(fid), on);
      notify("info", on ? "Auto-sync on" : "Auto-sync off");
    }
    return;
  }
  if (s_pre(pid, "fadf:")) { folder_edit_kv(pid + 5, "addFile", "x", inp); render_mfolders(); notify("info", "Adding file..."); return; }
  if (s_pre(pid, "fnm2:")) { folder_edit_kv(pid + 5, "setMeta", "name", inp); render_mfolders(); notify("info", "Saved"); return; }
  if (s_pre(pid, "fds2:")) { if (s_len(inp) > 250) inp[250] = 0; folder_edit_kv(pid + 5, "setMeta", "desc", inp); render_mfolders(); notify("info", "Saved"); return; }
  if (s_pre(pid, "flnk:")) { folder_edit_kv(pid + 5, "link", "f", inp); render_mfolders(); notify("info", "Linked"); return; }
  if (s_pre(pid, "fgr:")) { folder_edit_kv(pid + 4, "grant", "p", inp); notify("info", "Admin granted"); return; }
  if (s_pre(pid, "frv:")) { folder_edit_kv(pid + 4, "revoke", "p", inp); notify("info", "Admin revoked"); return; }
  /* file inside a folder: id "ffl:<sha>\t<name>" */
  if (s_pre(pid, "ffl:")) {
    char sha[80] = "", name[160] = "";
    const char *r = pid + 4; unsigned i = 0;
    while (*r && *r != '\t' && i < sizeof(sha) - 1) sha[i++] = *r++;
    sha[i] = 0;
    if (*r == '\t') r++;
    i = 0; while (*r && i < sizeof(name) - 1) name[i++] = *r++;
    name[i] = 0;
    if (s_eq(val, "dl")) {
      char j[300] = "{\"sha\":\"";
      jesc(j, sizeof(j), sha); s_cat(j, "\",\"name\":\"", sizeof(j));
      jesc(j, sizeof(j), name[0] ? name : sha); s_cat(j, "\"}", sizeof(j));
      hal_folder_download(g_cur_folder, s_len(g_cur_folder), j, s_len(j));
      notify("info", "Downloading...");
    } else if (s_eq(val, "fetch")) {
      hal_media_fetch(sha, s_len(sha));
      notify("info", "Fetching...");
    }
    return;
  }
}

/* ── module entry points ────────────────────────────────────────────────── */
static uint64_t g_tick = 0;

__attribute__((export_name("module_init")))
void module_init(void) {
  share_load();
  share_apply();      /* resume the providers with the saved settings */
  render_current();
  render_mfolders();
  render_status();
  hal_log(1, "files: ready", 12);
}

/* Routine LAN scan: refresh the host's directory of reachable Blossom servers
 * so media resolution can query nearby devices by hash without scanning per
 * message. Run shortly after start and then every ~60s. */
static void lan_scan(void) {
  char servers[512];
  hal_lan_scan(servers, sizeof(servers) - 1);
}

__attribute__((export_name("module_tick")))
void module_tick(void) {
  g_tick++;
  if (g_tick == 3 || g_tick % 60 == 0) lan_scan();
  if (g_tick % 5 == 0) render_status();
  /* Pick up async fetches, but only when showing the full library — don't
   * clobber a search or folder view the user is reading. */
  if (g_tick % 15 == 0 && g_view == 0) render_library();
  /* Refresh the Folders view (folder browse is async/cached on the host). */
  if (g_tick % 6 == 0) render_mfolders();
}

__attribute__((export_name("module_handle_event")))
void module_handle_event(void) {
  if (hal_msg_available() == 0) return;
  char buf[2048];
  uint32_t n = hal_msg_recv(buf, sizeof(buf) - 1);
  if (n == 0) return;
  buf[n] = 0;

  char cmd[40] = "", typ[24] = "";
  jstr(buf, "command", cmd, sizeof(cmd));
  jstr(buf, "type", typ, sizeof(typ));

  if (s_eq(typ, "file.open")) {
    /* Picker result: archive the file and surface its token. */
    char path[400] = "";
    jstr(buf, "path", path, sizeof(path));
    if (!path[0]) return;
    char token[80];
    uint32_t n = hal_media_put_file(path, s_len(path), token, sizeof(token) - 1);
    if (n == 0) { notify("warning", "Could not read that file"); return; }
    token[n] = 0;
    render_current();
    prompt_details(token);    /* shows the shareable token right away */
    return;
  }

  if (s_eq(cmd, "add_file")) {
    const char *m = "{\"type\":\"file.pick\",\"title\":\"Add a file to the archive\","
                    "\"mode\":\"view\"}";
    hal_msg_send(m, s_len(m));
  } else if (s_eq(cmd, "find_hash")) {
    prompt_search();
  } else if (s_eq(cmd, "search")) {
    jstr(buf, "q", g_query, sizeof(g_query));
    if (g_query[0]) { g_view = 1; render_search(); }
    else { g_view = 0; render_library(); }
  } else if (s_eq(cmd, "show_all")) {
    g_view = 0; g_query[0] = 0; render_library();
  } else if (s_eq(cmd, "folders")) {
    g_view = 3; render_folders();
  } else if (s_eq(cmd, "folder_new")) {
    prompt_input1("fnew", "", "New folder", "folder name", 96);
  } else if (s_eq(cmd, "folder_open_id")) {
    prompt_input1("fopen", "", "Open folder", "folder id or npub", 120);
  } else if (s_eq(cmd, "folder_add_disk")) {
    hal_storage_request(0);               /* Android: all-files access */
    g_mode = 1; g_cur_folder[0] = 0;
    s_cpy(g_browse, "/", sizeof(g_browse));
    render_browse();
  } else if (s_eq(cmd, "folder_use")) {
    if (g_mode == 1) {
      hal_folder_add_disk(g_browse, s_len(g_browse));
      notify("info", "Adding folder from disk...");
      g_mode = 0; g_cur_folder[0] = 0;
      render_owned();
    } else {
      notify("info", "Open 'Add from disk' first");
    }
  } else if (s_eq(cmd, "folder_back")) {
    g_mode = 0; g_cur_folder[0] = 0; render_mfolders();
  } else if (s_eq(cmd, "folder_manage")) {
    if (g_cur_folder[0]) prompt_folder_manage();
    else notify("info", "Open a folder first");
  } else if (s_eq(cmd, "folders_tap")) {
    char id[280] = "";
    jstr(buf, "folders_id", id, sizeof(id));
    if (!id[0]) return;
    if (g_mode == 1) {                     /* disk browser: navigate */
      if (s_pre(id, "cd:")) { s_cpy(g_browse, id + 3, sizeof(g_browse)); render_browse(); }
      return;
    }
    if (s_pre(id, "own:") || s_pre(id, "dir:")) {
      s_cpy(g_cur_folder, id + 4, sizeof(g_cur_folder));
      g_cur_folder_name[0] = 0;
      render_open();
    } else if (s_pre(id, "ffile:")) {
      /* id = "ffile:<sha>\t<name>" */
      char sha[80] = "", name[160] = "";
      const char *r = id + 6; unsigned i = 0;
      while (*r && *r != '\t' && i < sizeof(sha) - 1) sha[i++] = *r++;
      sha[i] = 0;
      if (*r == '\t') r++;
      i = 0; while (*r && i < sizeof(name) - 1) name[i++] = *r++;
      name[i] = 0;
      char m[500] = "{\"type\":\"ui.prompt\",\"id\":\"ffl:";
      jesc(m, sizeof(m), sha); s_cat(m, "\\t", sizeof(m)); jesc(m, sizeof(m), name);
      s_cat(m, "\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), name[0] ? name : "File");
      s_cat(m, "\",\"body\":\"sha256:\\n", sizeof(m)); jesc(m, sizeof(m), sha);
      s_cat(m, "\",\"chips\":[{\"label\":\"Download\",\"value\":\"dl\"},"
              "{\"label\":\"Fetch\",\"value\":\"fetch\"}]}", sizeof(m));
      hal_msg_send(m, s_len(m));
    }
  } else if (s_eq(cmd, "library_tap")) {
    char id[200] = "";
    jstr(buf, "library_id", id, sizeof(id));
    if (!id[0]) return;
    if (s_pre(id, "dir:")) {
      /* "dir:<parent>\t<folder>" — open that virtual folder. */
      const char *r = id + 4;
      unsigned i = 0;
      while (*r && *r != '\t' && i < sizeof(g_dir_parent) - 1) g_dir_parent[i++] = *r++;
      g_dir_parent[i] = 0;
      if (*r == '\t') r++;
      i = 0;
      while (*r && i < sizeof(g_dir_folder) - 1) g_dir_folder[i++] = *r++;
      g_dir_folder[i] = 0;
      g_view = 2; render_folder();
    } else {
      prompt_details(id);
    }
  } else if (s_eq(cmd, "share_apply")) {
    g_blossom_on = jbool_def(buf, "blossom_on", 1);
    g_uploads_on = jbool_def(buf, "uploads_on", 0);
    g_seed_on = jbool_def(buf, "seed_on", 1);
    char pb[12] = "";
    jstr(buf, "blossom_port", pb, sizeof(pb));
    if (pb[0]) { int p = to_int(pb); if (p > 0 && p < 65536) g_port = p; }
    share_save();
    share_apply();
    render_status();
    notify("info", "Sharing settings applied");
  } else if (s_eq(cmd, "prompt")) {
    do_prompt_result(buf);
  }
}

__attribute__((export_name("module_destroy")))
void module_destroy(void) {}

__attribute__((export_name("module_tick_interval_ms")))
uint32_t module_tick_interval_ms(void) { return 1000; }
