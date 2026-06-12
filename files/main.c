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
static char g_list[16384];   /* hal_media_list JSON */
static char g_out[16384];    /* ui.people.set message */

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

static void render_library(void) {
  uint32_t n = hal_media_list(0, 100, g_list, sizeof(g_list) - 1);
  g_list[n] = 0;
  char *m = g_out; const unsigned sz = sizeof(g_out);
  m[0] = 0;
  s_cat(m, "{\"type\":\"ui.people.set\",\"field\":\"library\",\"sections\":["
           "{\"title\":\"Files\",\"items\":[", sz);
  int first = 1;
  /* Walk the JSON array entry by entry: each object starts at '{"sha256"'. */
  const char *p = g_list;
  while ((p = (const char *)p) && *p) {
    if (!(p[0]=='{'&&p[1]=='"'&&p[2]=='s'&&p[3]=='h'&&p[4]=='a'&&p[5]=='2')) { p++; continue; }
    /* Slice runs to the next entry start (or end of buffer). */
    const char *end = p + 1;
    while (*end && !(end[0]=='}'&&end[1]==','&&end[2]=='{'&&end[3]=='"'&&end[4]=='s')) end++;
    if (*end) end++;            /* include the closing '}' */
    char slice[1200]; unsigned si = 0;
    for (const char *q = p; q < end && si < sizeof(slice) - 1; q++) slice[si++] = *q;
    slice[si] = 0;

    char token[80], name[96], ext[20], tags[160];
    jstr(slice, "token", token, sizeof(token));
    jstr(slice, "name", name, sizeof(name));
    jstr(slice, "ext", ext, sizeof(ext));
    copy_tags(slice, slice + si, tags, sizeof(tags));
    unsigned size = (unsigned)jnum(slice, "size");

    char sub[64]; sub[0] = 0;
    s_cat(sub, ".", sizeof(sub)); s_cat(sub, ext, sizeof(sub));
    s_cat(sub, " - ", sizeof(sub));
    { char fs[24]; fmt_size(size, fs, sizeof(fs)); s_cat(sub, fs, sizeof(sub)); }

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

static void prompt_details(const char *token) {
  char meta[1200];
  uint32_t n = hal_media_meta(token, s_len(token), meta, sizeof(meta) - 1);
  if (n == 0) return;
  meta[n] = 0;
  char name[96], ext[20];
  jstr(meta, "name", name, sizeof(name));
  jstr(meta, "ext", ext, sizeof(ext));
  char body[900]; body[0] = 0;
  if (name[0]) { s_cat(body, name, sizeof(body)); s_cat(body, "\n", sizeof(body)); }
  s_cat(body, "Token (paste into any chat):\n", sizeof(body));
  s_cat(body, token, sizeof(body));
  { char fs[24]; fmt_size((unsigned)jnum(meta, "size"), fs, sizeof(fs));
    s_cat(body, "\nSize: ", sizeof(body)); s_cat(body, fs, sizeof(body)); }
  /* Magnet for cross-network (BitTorrent) sharing. Built in the background —
   * empty on the first open, present once seeding has computed it. */
  { char magnet[400];
    uint32_t mg = hal_media_magnet(token, s_len(token), magnet, sizeof(magnet) - 1);
    if (mg > 0) { magnet[mg] = 0;
      s_cat(body, "\n\nMagnet (share to fetch over BitTorrent):\n", sizeof(body));
      s_cat(body, magnet, sizeof(body)); }
    else s_cat(body, "\n\nMagnet: computing… (reopen in a moment)", sizeof(body)); }

  char m[1400] = "{\"type\":\"ui.prompt\",\"id\":\"fdet:";
  jesc(m, sizeof(m), token);
  s_cat(m, "\",\"title\":\"", sizeof(m));
  jesc(m, sizeof(m), name[0] ? name : "File");
  s_cat(m, "\",\"body\":\"", sizeof(m));
  jesc(m, sizeof(m), body);
  s_cat(m, "\",\"chips\":["
          "{\"label\":\"Edit tags\",\"value\":\"tags\"},"
          "{\"label\":\"Delete\",\"value\":\"del\"}]}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

static void prompt_tags(const char *token) {
  char m[400] = "{\"type\":\"ui.prompt\",\"id\":\"ftg:";
  jesc(m, sizeof(m), token);
  s_cat(m, "\",\"title\":\"Tags\",\"body\":\"Space-separated tags. "
          "Empty clears.\",\"input\":{\"hint\":\"tags\",\"max\":60},"
          "\"confirm\":\"Save\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

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
  render_library();
}

/* ── prompt results / commands ──────────────────────────────────────────── */
static void do_prompt_result(const char *buf) {
  char pid[96] = "", val[24] = "", inp[80] = "";
  jstr(buf, "prompt_id", pid, sizeof(pid));
  jstr(buf, "prompt_value", val, sizeof(val));
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
  } else if (pid[0]=='f'&&pid[1]=='d'&&pid[2]=='e'&&pid[3]=='t'&&pid[4]==':') {
    const char *token = pid + 5;
    if (s_eq(val, "del")) {
      hal_media_delete(token, s_len(token));
      render_library();
      notify("info", "Deleted from the archive");
    } else if (s_eq(val, "tags")) {
      prompt_tags(token);
    }
  } else if (pid[0]=='f'&&pid[1]=='t'&&pid[2]=='g'&&pid[3]==':') {
    apply_tags(pid + 4, inp);
  }
}

/* ── module entry points ────────────────────────────────────────────────── */
static uint64_t g_tick = 0;

__attribute__((export_name("module_init")))
void module_init(void) {
  share_load();
  share_apply();      /* resume the providers with the saved settings */
  render_library();
  render_status();
  hal_log(1, "files: ready", 12);
}

__attribute__((export_name("module_tick")))
void module_tick(void) {
  g_tick++;
  if (g_tick % 5 == 0) render_status();
  if (g_tick % 15 == 0) render_library();   /* pick up async fetches */
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
    render_library();
    prompt_details(token);    /* shows the shareable token right away */
    return;
  }

  if (s_eq(cmd, "add_file")) {
    const char *m = "{\"type\":\"file.pick\",\"title\":\"Add a file to the archive\","
                    "\"mode\":\"view\"}";
    hal_msg_send(m, s_len(m));
  } else if (s_eq(cmd, "find_hash")) {
    prompt_search();
  } else if (s_eq(cmd, "library_tap")) {
    char token[80] = "";
    jstr(buf, "library_id", token, sizeof(token));
    if (token[0]) prompt_details(token);
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
