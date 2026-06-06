/*
 * APRS station wapp — Map / Messenger / Beacon / Settings.
 *
 * Mirrors the mature Geogram APRS UI on top of Aurora primitives:
 *  - Map      : pins for stations/messages received in the filter area
 *               (host renders ui.map.marker pushed from parsed packets)
 *  - Messenger: chat view of APRS text messages addressed to us
 *  - Beacon   : craft a position / status / emergency / timed beacon
 *  - Settings : callsign, position, network, filter, path, tags
 *
 * Networking is the reusable aprs.{h,c} library over the hal_socket_*
 * HAL. The APRS-IS passcode is computed (aprs_passcode) so we can TX.
 */
#include <stdint.h>
#include "geogram_wasm_hal.h"
#include "aprs.h"
#include "ble.h"

/* ── tiny libc ──────────────────────────────────────────────────────── */
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
static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
/* APRS message acks/rejects: body is "ack<n>" or "rej<n>" — not chat text. */
static int is_ack_text(const char *t) {
  if ((t[0] == 'a' && t[1] == 'c' && t[2] == 'k') ||
      (t[0] == 'r' && t[1] == 'e' && t[2] == 'j')) {
    return t[3] >= '0' && t[3] <= '9';
  }
  return 0;
}

/* extract "key":"value" from buf; returns 1 if found */
/* hex digit -> value, or -1 */
static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
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
      if (*p == '\\' && *(p + 1)) {
        p++;
        /* Decode \uXXXX -> one byte. The host JSON-encodes received BLE bytes,
         * so the 0x1f field separator arrives as ""; without this it was
         * copied as the literal text "u001f" and frames couldn't be split. */
        if (*p == 'u' && hexv(p[1]) >= 0 && hexv(p[2]) >= 0 &&
            hexv(p[3]) >= 0 && hexv(p[4]) >= 0) {
          int v = (hexv(p[1]) << 12) | (hexv(p[2]) << 8) |
                  (hexv(p[3]) << 4) | hexv(p[4]);
          p += 5;
          out[i++] = (char)(v & 0xff);
        } else {
          out[i++] = *p++;
        }
      } else out[i++] = *p++;
    }
    out[i] = 0; return 1;
  }
  out[0] = 0; return 0;
}
/* read a JSON bool: matches "key":true / "key":1 (host sends bools unquoted) */
/* Parse a boolean field; return `def` when the key is absent (so callers can
 * have a true default that an explicit "false" still overrides). */
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
static int jbool(const char *buf, const char *key) { return jbool_def(buf, key, 0); }
static double to_dbl(const char *s) {
  int neg = 0; if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
  double v = 0; while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
  if (*s == '.') { s++; double f = 0.1; while (*s >= '0' && *s <= '9') { v += (*s - '0') * f; f *= 0.1; s++; } }
  return neg ? -v : v;
}
static int to_int(const char *s) { return (int)to_dbl(s); }

/* append a number with 4 decimals to dst */
static void append_dbl(char *dst, unsigned m, double v) {
  unsigned l = s_len(dst);
  if (v < 0 && l < m - 1) { dst[l++] = '-'; v = -v; }
  int w = (int)v;
  int f = (int)((v - w) * 10000.0 + 0.5);
  if (f >= 10000) { w += 1; f -= 10000; }
  char wb[16]; int wi = 0;
  if (w == 0) wb[wi++] = '0';
  while (w > 0 && wi < 15) { wb[wi++] = (char)('0' + w % 10); w /= 10; }
  while (wi > 0 && l < m - 1) dst[l++] = wb[--wi];
  if (l < m - 6) {
    dst[l++] = '.';
    dst[l++] = (char)('0' + (f / 1000) % 10);
    dst[l++] = (char)('0' + (f / 100) % 10);
    dst[l++] = (char)('0' + (f / 10) % 10);
    dst[l++] = (char)('0' + f % 10);
  }
  dst[l] = 0;
}

/* JSON-escape src into dst (for embedding text in our outbox messages) */
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

/* ── state ──────────────────────────────────────────────────────────── */
static int   g_sock = -1;
static int   g_logged = 0;
static int   g_seq = 1;
static char  g_call[16] = "N0CALL";  /* replaced at init by hal_identity() */
static double g_lat = 0, g_lon = 0;
static int   g_radius = 100;
static char  g_symbol[8] = "/>";
static char  g_path[64] = "WIDE1-1,WIDE2-1";
static int   g_auto = 0;
static int   g_interval = 600;          /* seconds */
static uint64_t g_last_beacon = 0;
/* Auto-connect / auto-reconnect state. */
static int   g_want_connect = 0;        /* keep a connection alive */
static char  g_host[64] = APRS_DEFAULT_HOST;
static int   g_port = APRS_DEFAULT_PORT;
static uint64_t g_last_reconnect = 0;

/* BLE transport (shared adapter via hal_ble_*). g_ble_on = exchange enabled
 * (on by default — matches the "Exchange over Bluetooth" default in
 * screens/home.ui.json); g_ble_relay = bridge frames between BLE and APRS-IS;
 * g_ble_started tracks whether we've told the HAL to scan. */
static int g_ble_on = 1, g_ble_relay = 0, g_ble_started = 0;
static uint64_t g_ble_last_beacon = 0;
/* compact BLE senders, defined with the module entry points */
static void ble_tx_msg(const char *to, const char *text);
static void ble_tx_pos(double lat, double lon, const char *comment);

/* Recurring group bulletins: re-broadcast the same text every 5 minutes
 * for a chosen period (APRS is transient and most clients keep no history,
 * so periodic re-sends let late joiners catch important news). In-memory:
 * a restart clears the schedule (the pinned copy on receivers persists). */
#define RECUR_MAX 8
#define RECUR_INTERVAL 300            /* 5 minutes between re-sends */
typedef struct {
  int active;
  char group[8];
  char text[80];
  uint64_t end;                       /* stop re-sending at this epoch */
  uint64_t last;                      /* epoch of the last send */
} recur_t;
static recur_t g_recur[RECUR_MAX];

static void notify(const char *level, const char *body) {
  char m[256] = "{\"type\":\"notify\",\"level\":\"";
  s_cat(m, level, sizeof(m));
  s_cat(m, "\",\"title\":\"APRS\",\"body\":\"", sizeof(m));
  jesc(m, sizeof(m), body);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

static void status(const char *text) {
  char m[400] = "{\"type\":\"ui.log.append\",\"field\":\"status\",\"line\":\"";
  jesc(m, sizeof(m), text);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Persistent transport indicators on the map (replaces flickering toasts):
 * APRS-IS connected? and BLE active? Pushed only when a value changes, so a
 * flapping link never spams. -1 = nothing pushed yet. */
static int g_ind_net = -1, g_ind_ble = -1;
static void push_status(void) {
  int net = (g_sock >= 0 && g_logged) ? 1 : 0;
  int ble = g_ble_on ? 1 : 0;
  if (net == g_ind_net && ble == g_ind_ble) return;
  g_ind_net = net; g_ind_ble = ble;
  char m[256];
  s_cpy(m, "{\"type\":\"ui.map.status\",\"items\":["
           "{\"id\":\"aprsis\",\"label\":\"APRS-IS\",\"on\":", sizeof(m));
  s_cat(m, net ? "true" : "false", sizeof(m));
  s_cat(m, "},{\"id\":\"ble\",\"label\":\"BLE\",\"on\":", sizeof(m));
  s_cat(m, ble ? "true" : "false", sizeof(m));
  s_cat(m, "}]}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

static void fmt_time(char *b) {
  uint64_t e = hal_time_epoch();
  int hh = (int)((e / 3600) % 24), mm = (int)((e / 60) % 60);
  b[0] = (char)('0' + hh / 10); b[1] = (char)('0' + hh % 10); b[2] = ':';
  b[3] = (char)('0' + mm / 10); b[4] = (char)('0' + mm % 10); b[5] = 0;
}

/* kind: "pos" (position beacon) or "msg" (text message). The host uses
 * it to detect repeats — positions repeat per callsign (telemetry varies),
 * messages repeat by exact text. */
/* convo: conversation id for the Messenger (callsign for 1:1, "#GROUP" for a
 * bulletin room). Pass "" for the geo-chat feed (it isn't grouped). */
/* fwd decl: append "lat":..,"lon":.. when known */
static void cat_pos(char *m, unsigned sz, double lat, double lon);
static void chat_append(const char *field, const char *convo, const char *dir,
                        const char *from, const char *text, const char *kind,
                        int recur, const char *meta, double lat, double lon) {
  char t[8]; fmt_time(t);
  char m[500] = "{\"type\":\"ui.chat.append\",\"field\":\"";
  s_cat(m, field, sizeof(m));
  s_cat(m, "\",\"message\":{\"dir\":\"", sizeof(m));
  s_cat(m, dir, sizeof(m));
  s_cat(m, "\",\"convo\":\"", sizeof(m)); jesc(m, sizeof(m), convo);
  s_cat(m, "\",\"from\":\"", sizeof(m)); jesc(m, sizeof(m), from);
  s_cat(m, "\",\"text\":\"", sizeof(m)); jesc(m, sizeof(m), text);
  s_cat(m, "\",\"kind\":\"", sizeof(m)); s_cat(m, kind, sizeof(m));
  s_cat(m, "\",\"meta\":\"", sizeof(m)); jesc(m, sizeof(m), meta);
  if (recur) s_cat(m, "\",\"recur\":true,\"time\":\"", sizeof(m));
  else s_cat(m, "\",\"time\":\"", sizeof(m));
  s_cat(m, t, sizeof(m));
  s_cat(m, "\"", sizeof(m)); cat_pos(m, sizeof(m), lat, lon);
  s_cat(m, "}}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* detail = the station's latest comment/message ("" if none). The host shows
 * it, plus lat/lon and a relative "last heard" time, in the marker popup —
 * so we send the heard epoch (seconds) and let the host format it. */
static void u_itoa(unsigned v, char *out);   /* defined with the messenger code */
static void push_marker(const char *id, double lat, double lon,
                        const char *color, const char *detail) {
  char m[360] = "{\"type\":\"ui.map.marker\",\"id\":\"";
  jesc(m, sizeof(m), id);
  s_cat(m, "\",\"label\":\"", sizeof(m)); jesc(m, sizeof(m), id);
  s_cat(m, "\",\"lat\":", sizeof(m)); append_dbl(m, sizeof(m), lat);
  s_cat(m, ",\"lon\":", sizeof(m)); append_dbl(m, sizeof(m), lon);
  s_cat(m, ",\"heard\":", sizeof(m));
  { char hb[12]; u_itoa((unsigned)hal_time_epoch(), hb); s_cat(m, hb, sizeof(m)); }
  if (detail && detail[0]) {
    s_cat(m, ",\"detail\":\"", sizeof(m)); jesc(m, sizeof(m), detail);
    s_cat(m, "\"", sizeof(m));
  }
  if (color && color[0]) {
    s_cat(m, ",\"color\":\"", sizeof(m)); s_cat(m, color, sizeof(m));
    s_cat(m, "\"", sizeof(m));
  }
  s_cat(m, "}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

static void center_map(void) {
  char m[160] = "{\"type\":\"ui.map.viewport\",\"lat\":";
  append_dbl(m, sizeof(m), g_lat);
  s_cat(m, ",\"lon\":", sizeof(m)); append_dbl(m, sizeof(m), g_lon);
  s_cat(m, ",\"zoom\":9}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Tell the host the coverage circle: my station + the filter radius. */
static void push_radius(void) {
  char m[160] = "{\"type\":\"ui.map.radius\",\"lat\":";
  append_dbl(m, sizeof(m), g_lat);
  s_cat(m, ",\"lon\":", sizeof(m)); append_dbl(m, sizeof(m), g_lon);
  s_cat(m, ",\"km\":", sizeof(m));
  char nb[12]; int v = g_radius, j = 0, k = 0; char t[12];
  if (v == 0) t[j++] = '0'; while (v > 0) { t[j++] = (char)('0' + v % 10); v /= 10; }
  while (j > 0) nb[k++] = t[--j]; nb[k] = 0;
  s_cat(m, nb, sizeof(m)); s_cat(m, "}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Drop the old area's pins + geo-chat when the radius changes. */
static void clear_area(void) {
  const char *a = "{\"type\":\"ui.map.markers.clear\"}";
  hal_msg_send(a, s_len(a));
  const char *b = "{\"type\":\"ui.chat.clear\",\"field\":\"geochat\"}";
  hal_msg_send(b, s_len(b));
}

/* read shared config from a command's bundled fields */
static void read_config(const char *buf) {
  char v[64];
  if (jstr(buf, "callsign", v, sizeof(v)) && v[0]) s_cpy(g_call, v, sizeof(g_call));
  if (jstr(buf, "my_lat", v, sizeof(v))) g_lat = to_dbl(v);
  if (jstr(buf, "my_lon", v, sizeof(v))) g_lon = to_dbl(v);
  if (jstr(buf, "radius_km", v, sizeof(v)) && v[0]) g_radius = to_int(v);
  if (jstr(buf, "symbol", v, sizeof(v)) && s_len(v) >= 2) s_cpy(g_symbol, v, sizeof(g_symbol));
  if (jstr(buf, "path", v, sizeof(v))) s_cpy(g_path, v, sizeof(g_path));
  if (jstr(buf, "beacon_interval", v, sizeof(v)) && v[0]) g_interval = to_int(v);
  /* NOTE: BLE on/off is intentionally NOT read here. read_config runs on
   * every command (connect, sends, …) and the host serialises an unset
   * checkbox as false, which would clobber the on-by-default state before the
   * user ever touches Settings. BLE state is owned by init (default on) and
   * the explicit "Apply Bluetooth" action instead — see the ble_apply cmd. */
}

static void do_connect(const char *buf) {
  read_config(buf);
  s_cpy(g_host, APRS_DEFAULT_HOST, sizeof(g_host));
  g_port = APRS_DEFAULT_PORT;
  char v[64];
  if (jstr(buf, "server", v, sizeof(v)) && v[0]) s_cpy(g_host, v, sizeof(g_host));
  if (jstr(buf, "port", v, sizeof(v)) && v[0]) g_port = to_int(v);
  g_want_connect = 1;                 /* keep it connected (auto-reconnect) */
  if (g_sock >= 0) { aprs_disconnect(g_sock); g_sock = -1; }
  g_logged = 0;
  g_last_reconnect = hal_time_epoch();
  g_sock = aprs_connect(g_host, g_port);
  if (g_sock < 0) { status("connect: socket error (will retry)"); return; }
  char line[128] = "Connecting to "; s_cat(line, g_host, sizeof(line));
  status(line);
}

static void do_beacon(const char *buf, int emergency) {
  read_config(buf);
  int net = (g_sock >= 0 && g_logged);
  if (!net && !g_ble_on) {
    notify("warning", "Connect to APRS-IS or enable Bluetooth first");
    return;
  }
  char typ[16] = "position", comment[120] = "";
  jstr(buf, "beacon_type", typ, sizeof(typ));
  jstr(buf, "beacon_comment", comment, sizeof(comment));
  if (emergency || s_eq(typ, "emergency")) {
    char c[140] = "EMERGENCY "; s_cat(c, comment, sizeof(c));
    if (net) aprs_send_beacon(g_sock, g_call, g_lat, g_lon, "\\!", "TCPIP*", c);
    if (g_ble_on) ble_tx_pos(g_lat, g_lon, c);
    push_marker(g_call, g_lat, g_lon, "red", c);
    status("TX emergency beacon");
    notify("warning", "Emergency beacon sent");
  } else {
    if (net) aprs_send_beacon(g_sock, g_call, g_lat, g_lon, g_symbol, "TCPIP*", comment);
    if (g_ble_on) ble_tx_pos(g_lat, g_lon, comment);
    push_marker(g_call, g_lat, g_lon, "blue", comment);
    status("TX position beacon");
  }
  g_last_beacon = hal_time_epoch();
}

/* ── Messenger conversations ────────────────────────────────────────────
 * The host ConversationsField is app-agnostic: this wapp owns every bit of
 * semantics — what a conversation is (a callsign for 1:1, "#GROUP" for a
 * bulletin room), its title/icon/distance badge, dedup of repeated bulletins,
 * pinning, and the recurring schedule. We drive the host via ui.convo.* and
 * ui.prompt; the host only renders and reports taps back. */

static void u_itoa(unsigned v, char *out) {
  char t[12]; int j = 0;
  if (v == 0) t[j++] = '0';
  while (v > 0) { t[j++] = (char)('0' + v % 10); v /= 10; }
  int k = 0; while (j > 0) out[k++] = t[--j]; out[k] = 0;
}

/* FNV-1a over convo|from|text — a stable content signature (and the pin key
 * shared between a message and its later repeat so the host can promote it). */
static unsigned sig_hash(const char *a, const char *b, const char *c) {
  unsigned h = 2166136261u;
  for (const char *p = a; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
  h ^= 1u; h *= 16777619u;
  for (const char *p = b; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
  h ^= 1u; h *= 16777619u;
  for (const char *p = c; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; }
  return h;
}
static unsigned g_seen[128];
static unsigned g_seen_cnt = 0;
static int seen_has(unsigned h) {
  unsigned n = g_seen_cnt < 128 ? g_seen_cnt : 128;
  for (unsigned i = 0; i < n; i++) if (g_seen[i] == h) return 1;
  return 0;
}
static void seen_add(unsigned h) { g_seen[g_seen_cnt % 128] = h; g_seen_cnt++; }

/* Separate raw-frame dedup (cross-transport + relay loop guard), kept apart
 * from the conversation seen-ring above so it can't evict pin-detection keys.
 * Time-windowed: a frame is suppressed for FSEEN_WINDOW after it is first seen,
 * so a message re-broadcast many times (BLE adverts repeat for their TTL, and
 * the mesh relays them) is shown only once. A plain count ring evicted recent
 * hashes once enough other frames arrived, letting duplicates reappear. */
#define FSEEN_MAX 256
#define FSEEN_WINDOW 3600   /* 60 minutes */
static struct { unsigned h; uint64_t t; } g_fseen[FSEEN_MAX];
static int fseen_has(unsigned h) {
  uint64_t now = hal_time_epoch();
  for (unsigned i = 0; i < FSEEN_MAX; i++)
    if (g_fseen[i].t && g_fseen[i].h == h && now - g_fseen[i].t < FSEEN_WINDOW)
      return 1;
  return 0;
}
static void fseen_add(unsigned h) {
  uint64_t now = hal_time_epoch();
  unsigned oldest = 0;
  for (unsigned i = 0; i < FSEEN_MAX; i++) {
    /* reuse a free or expired slot so an hour of distinct frames can't evict
     * still-valid entries */
    if (!g_fseen[i].t || now - g_fseen[i].t >= FSEEN_WINDOW) {
      g_fseen[i].h = h; g_fseen[i].t = now; return;
    }
    if (g_fseen[i].t < g_fseen[oldest].t) oldest = i;
  }
  g_fseen[oldest].h = h; g_fseen[oldest].t = now;  /* all fresh: drop oldest */
}

/* Content dedup for the Live/Beacons geo-chat. The same message reaches us as
 * different raw frames — over BLE and over APRS-IS — and APRS-IS itself can
 * deliver duplicates via multiple IGates, so the per-frame fseen ring above
 * can't catch them. Dedup on sender+text (transport-independent) so a message
 * shows once per 60 min. Returns 1 if it's a duplicate to drop. */
static int geo_dup(const char *from, const char *text) {
  unsigned h = sig_hash("g", from, text);
  if (fseen_has(h)) return 1;
  fseen_add(h);
  return 0;
}

/* BLE mesh repeater: rebroadcast each received frame once, suppressing any
 * content already repeated within the last 10 minutes (loop/storm control). */
#define RPT_MAX 64
static struct { unsigned h; uint64_t t; } g_rpt[RPT_MAX];
static unsigned g_rpt_cnt = 0;
static int rpt_recent(unsigned h, uint64_t now) {
  unsigned n = g_rpt_cnt < RPT_MAX ? g_rpt_cnt : RPT_MAX;
  for (unsigned i = 0; i < n; i++)
    if (g_rpt[i].h == h && now - g_rpt[i].t < 600) return 1;
  return 0;
}
static void rpt_mark(unsigned h, uint64_t now) {
  g_rpt[g_rpt_cnt % RPT_MAX].h = h; g_rpt[g_rpt_cnt % RPT_MAX].t = now; g_rpt_cnt++;
}

/* Last-known station positions, for the 1:1 distance badge. */
typedef struct { char call[16]; double lat, lon; int used; } pos_t;
static pos_t g_pos[64];
static void pos_set(const char *call, double lat, double lon) {
  int free_i = -1;
  for (int i = 0; i < 64; i++) {
    if (g_pos[i].used) {
      if (s_eq(g_pos[i].call, call)) { g_pos[i].lat = lat; g_pos[i].lon = lon; return; }
    } else if (free_i < 0) free_i = i;
  }
  if (free_i < 0) free_i = (int)(g_seen_cnt % 64);
  s_cpy(g_pos[free_i].call, call, sizeof(g_pos[free_i].call));
  g_pos[free_i].lat = lat; g_pos[free_i].lon = lon; g_pos[free_i].used = 1;
}
static int pos_get(const char *call, double *lat, double *lon) {
  for (int i = 0; i < 64; i++)
    if (g_pos[i].used && s_eq(g_pos[i].call, call)) {
      *lat = g_pos[i].lat; *lon = g_pos[i].lon; return 1;
    }
  return 0;
}
/* cos via Taylor (lat in radians, |x| < pi/2 — well within range) */
static double m_cos(double x) {
  double x2 = x * x;
  return 1.0 - x2 / 2.0 + x2 * x2 / 24.0 - x2 * x2 * x2 / 720.0
         + x2 * x2 * x2 * x2 / 40320.0;
}
/* Equirectangular distance; writes "<n> km"/"<n> m" badge, 1 if known. */
/* Distance from our position (the map pinpoint) to lat/lon, as "<n> km"/"m". */
static int distance_to(double lat, double lon, char *out, unsigned osz) {
  out[0] = 0;
  if (g_lat == 0 && g_lon == 0) return 0;
  const double D2R = 0.0174532925199433;
  double x = (lon - g_lon) * D2R * m_cos((g_lat + lat) * 0.5 * D2R);
  double y = (lat - g_lat) * D2R;
  double km = 6371.0 * __builtin_sqrt(x * x + y * y);
  if (km < 1.0) { u_itoa((unsigned)(km * 1000.0 + 0.5), out); s_cat(out, " m", osz); }
  else { u_itoa((unsigned)(km + 0.5), out); s_cat(out, " km", osz); }
  return 1;
}
/* Distance to a callsign's last-known position (1 if known). */
static int distance_badge(const char *call, char *out, unsigned osz) {
  out[0] = 0;
  double lat, lon;
  if (!pos_get(call, &lat, &lon)) return 0;
  return distance_to(lat, lon, out, osz);
}

/* Conversations the host knows about, so we can refresh the distance badge
 * when a contact's position arrives. */
static char g_convo_ids[32][40];
static int g_convo_n = 0;
static void convo_remember(const char *id) {
  for (int i = 0; i < g_convo_n; i++) if (s_eq(g_convo_ids[i], id)) return;
  if (g_convo_n < 32) s_cpy(g_convo_ids[g_convo_n++], id, 40);
}
static int convo_known(const char *id) {
  for (int i = 0; i < g_convo_n; i++) if (s_eq(g_convo_ids[i], id)) return 1;
  return 0;
}

/* ── generic ui.convo.* senders ── */
/* append "lat":..,"lon":.. to m when the position is known (not 0,0). */
static void cat_pos(char *m, unsigned sz, double lat, double lon) {
  if (lat == 0 && lon == 0) return;
  s_cat(m, ",\"lat\":", sz); append_dbl(m, sz, lat);
  s_cat(m, ",\"lon\":", sz); append_dbl(m, sz, lon);
}
static void convo_msg(const char *id, const char *dir, const char *from,
                      const char *text, const char *key, const char *meta,
                      double lat, double lon) {
  char t[8]; fmt_time(t);
  char m[640] = "{\"type\":\"ui.convo.msg\",\"id\":\"";
  jesc(m, sizeof(m), id);
  s_cat(m, "\",\"dir\":\"", sizeof(m)); s_cat(m, dir, sizeof(m));
  s_cat(m, "\",\"from\":\"", sizeof(m)); jesc(m, sizeof(m), from);
  s_cat(m, "\",\"text\":\"", sizeof(m)); jesc(m, sizeof(m), text);
  s_cat(m, "\",\"key\":\"", sizeof(m)); s_cat(m, key, sizeof(m));
  s_cat(m, "\",\"meta\":\"", sizeof(m)); jesc(m, sizeof(m), meta);
  s_cat(m, "\",\"time\":\"", sizeof(m)); s_cat(m, t, sizeof(m));
  s_cat(m, "\"", sizeof(m)); cat_pos(m, sizeof(m), lat, lon);
  s_cat(m, "}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void convo_pin(const char *id, const char *key, const char *dir,
                      const char *from, const char *text, const char *meta,
                      double lat, double lon) {
  char t[8]; fmt_time(t);
  char m[640] = "{\"type\":\"ui.convo.pin\",\"id\":\"";
  jesc(m, sizeof(m), id);
  s_cat(m, "\",\"key\":\"", sizeof(m)); s_cat(m, key, sizeof(m));
  s_cat(m, "\",\"dir\":\"", sizeof(m)); s_cat(m, dir, sizeof(m));
  s_cat(m, "\",\"from\":\"", sizeof(m)); jesc(m, sizeof(m), from);
  s_cat(m, "\",\"text\":\"", sizeof(m)); jesc(m, sizeof(m), text);
  s_cat(m, "\",\"meta\":\"", sizeof(m)); jesc(m, sizeof(m), meta);
  s_cat(m, "\",\"time\":\"", sizeof(m)); s_cat(m, t, sizeof(m));
  s_cat(m, "\"", sizeof(m)); cat_pos(m, sizeof(m), lat, lon);
  s_cat(m, "}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void convo_unpin(const char *id, const char *key) {
  char m[160] = "{\"type\":\"ui.convo.unpin\",\"id\":\"";
  jesc(m, sizeof(m), id);
  s_cat(m, "\",\"key\":\"", sizeof(m)); s_cat(m, key, sizeof(m));
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
/* Refresh a conversation row (title/preview/icon + distance badge). */
static void convo_touch(const char *id, const char *preview, int select) {
  convo_remember(id);
  const char *icon = (id[0] == '#') ? "campaign" : "person";
  char badge[24] = "";
  if (id[0] != '#') distance_badge(id, badge, sizeof(badge));
  char m[600] = "{\"type\":\"ui.convo.upsert\",\"id\":\"";
  jesc(m, sizeof(m), id);
  s_cat(m, "\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), id);
  s_cat(m, "\",\"subtitle\":\"", sizeof(m)); jesc(m, sizeof(m), preview);
  s_cat(m, "\",\"badge\":\"", sizeof(m)); jesc(m, sizeof(m), badge);
  s_cat(m, "\",\"icon\":\"", sizeof(m)); s_cat(m, icon, sizeof(m));
  if (select) s_cat(m, "\",\"select\":true,\"bump\":true}", sizeof(m));
  else s_cat(m, "\",\"bump\":true}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
/* Distance-only refresh (when a known contact beacons a new position). */
static void convo_badge_only(const char *id) {
  if (id[0] == '#') return;
  char badge[24] = ""; distance_badge(id, badge, sizeof(badge));
  if (!badge[0]) return;
  char m[160] = "{\"type\":\"ui.convo.upsert\",\"id\":\"";
  jesc(m, sizeof(m), id);
  s_cat(m, "\",\"badge\":\"", sizeof(m)); jesc(m, sizeof(m), badge);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Deliver one conversation message: dedup by signature — first time shows in
 * the flow, a repeat is promoted to a pinned item (and further repeats are
 * ignored as updates of the same pin). [forcePin] is set for our own
 * recurring sends (pinned from the first beat). */
static void convo_deliver(const char *id, const char *dir, const char *from,
                          const char *text, const char *preview, int forcePin) {
  unsigned h = sig_hash(id, from, text);
  char key[16]; u_itoa(h, key);
  /* Distance + position of the sender (incoming only), so the host can show
   * them on the map when the distance is tapped. */
  char meta[24] = "";
  double lat = 0, lon = 0;
  if (s_eq(dir, "in") && pos_get(from, &lat, &lon)) {
    distance_to(lat, lon, meta, sizeof(meta));
  }
  int rep = seen_has(h);
  if (!rep) seen_add(h);
  if (forcePin || rep) convo_pin(id, key, dir, from, text, meta, lat, lon);
  else convo_msg(id, dir, from, text, key, meta, lat, lon);
  convo_touch(id, preview, 0);
}

static void do_convo_send(const char *buf) {
  read_config(buf);
  int net = (g_sock >= 0 && g_logged);
  if (!net && !g_ble_on) {
    notify("warning", "Connect to APRS-IS or enable Bluetooth first");
    return;
  }
  char id[40] = "", text[400] = "";
  jstr(buf, "conversations_convo", id, sizeof(id));
  jstr(buf, "conversations_input", text, sizeof(text));
  if (!id[0] || !text[0]) return;
  /* Optionally share our location: with no GPS we use the map pinpoint
   * (g_lat/g_lon from my_lat/my_lon) and transmit a position beacon so the
   * recipient can place us on their map. */
  int loc = jbool(buf, "include_location") && (g_lat != 0 || g_lon != 0);
  if (loc) {
    if (net) aprs_send_beacon(g_sock, g_call, g_lat, g_lon, g_symbol, "TCPIP*", "");
    if (g_ble_on) ble_tx_pos(g_lat, g_lon, "");
    push_marker(g_call, g_lat, g_lon, "blue", "");
  }
  if (id[0] == '#') {
    if (net) aprs_send_bulletin_multi(g_sock, g_call, id + 1, text, APRS_MAX_MSG_LEN);
  } else {
    if (net) aprs_send_message_multi(g_sock, g_call, id, text, APRS_MAX_MSG_LEN, &g_seq);
  }
  if (g_ble_on) ble_tx_msg(id, text);   /* compact BLE: to = callsign or #group */
  convo_deliver(id, "out", g_call, text, text, 0);
  status(loc ? "TX message + position" : "TX message");
}

/* Change the coverage radius: re-filter by reconnecting APRS-IS, and
 * drop the old area's pins/geo-chat so only the new area shows. */
static void do_set_radius(const char *buf) {
  read_config(buf);
  char v[16];
  if (jstr(buf, "map_radius", v, sizeof(v)) && v[0]) g_radius = to_int(v);
  if (g_radius < 1) g_radius = 1;
  clear_area();
  push_radius();
  if (g_sock >= 0) {
    aprs_disconnect(g_sock);
    char host[64] = APRS_DEFAULT_HOST; int port = APRS_DEFAULT_PORT;
    if (jstr(buf, "server", v, sizeof(v)) && v[0]) s_cpy(host, v, sizeof(host));
    { char pv[16]; if (jstr(buf, "port", pv, sizeof(pv)) && pv[0]) port = to_int(pv); }
    g_logged = 0;
    g_sock = aprs_connect(host, port);
  }
  { char b[48] = "radius "; char nb[12]; int x = g_radius, j = 0, k = 0; char t[12];
    if (x == 0) t[j++] = '0'; while (x > 0) { t[j++] = (char)('0' + x % 10); x /= 10; }
    while (j > 0) nb[k++] = t[--j]; nb[k] = 0;
    s_cat(b, nb, sizeof(b)); s_cat(b, " km", sizeof(b)); status(b); }
}

/* Send a geo-chat: a position beacon carrying the typed comment. Real
 * TX over APRS-IS (verified login with the computed passcode). */
static void do_geochat_send(const char *buf) {
  read_config(buf);
  int net = (g_sock >= 0 && g_logged);
  if (!net && !g_ble_on) {
    notify("warning", "Connect to APRS-IS or enable Bluetooth first");
    return;
  }
  char text[400] = "";
  jstr(buf, "geochat_input", text, sizeof(text));
  if (!text[0]) return;
  /* Drop any leading ">>" the user typed; we add it to each chunk. */
  const char *body = text;
  if (body[0] == '>' && body[1] == '>') { body += 2; while (*body == ' ') body++; }
  if (!body[0]) return;
  /* Long geo-chat is sent as several position beacons, each comment chunk
   * prefixed ">>" so every part lands on the Live tab (here and on other
   * Aurora stations). Reserve 2 chars of the comment budget for ">>". */
  int avail = APRS_MAX_MSG_LEN - 2;
  char chunk[80];
  int n = 0;
  while (n < 12 && aprs_split_text(body, avail, n, chunk, sizeof(chunk))) {
    char tagged[80];
    s_cpy(tagged, ">>", sizeof(tagged));
    s_cat(tagged, chunk, sizeof(tagged));
    if (net) aprs_send_beacon(g_sock, g_call, g_lat, g_lon, g_symbol, "TCPIP*", tagged);
    if (g_ble_on) ble_tx_msg("", tagged);   /* compact BLE: area/geo-chat text */
    n++;
  }
  /* Local echo: the whole message as one Live bubble. */
  char echo[420];
  s_cpy(echo, ">>", sizeof(echo));
  s_cat(echo, body, sizeof(echo));
  chat_append("geochat", "", "out", g_call, echo, "msg", 0, "", 0, 0);
  status("TX geo-chat");
}

/* Send one recurring bulletin now and echo it pinned at the top of the room. */
static void recur_broadcast(recur_t *r) {
  if (g_sock >= 0 && g_logged)
    aprs_send_bulletin_multi(g_sock, g_call, r->group, r->text, APRS_MAX_MSG_LEN);
  char convo[40];
  convo[0] = '#'; int j = 1;
  for (int i = 0; r->group[i] && j < 39; i++) convo[j++] = r->group[i];
  convo[j] = 0;
  if (g_ble_on) ble_tx_msg(convo, r->text);
  convo_deliver(convo, "out", g_call, r->text, r->text, 1);
}

/* Begin a recurring bulletin into [group] (re-broadcast every 5 min for
 * [secs], first one now). Reuses the slot for the same group if present. */
static void recur_begin(const char *group, const char *text, int secs) {
  if (g_sock < 0 || !g_logged) { notify("warning", "Connect first"); return; }
  if (!group[0] || !text[0]) { notify("warning", "Pick a group and message"); return; }
  if (secs < RECUR_INTERVAL) secs = RECUR_INTERVAL;
  if (secs > 172800) secs = 172800;             /* 48h cap */
  int slot = -1;
  for (int i = 0; i < RECUR_MAX; i++) {
    if (g_recur[i].active) {
      int same = 1;
      for (int k = 0; group[k] || g_recur[i].group[k]; k++)
        if (up(group[k]) != g_recur[i].group[k]) { same = 0; break; }
      if (same) { slot = i; break; }
    } else if (slot < 0) slot = i;
  }
  if (slot < 0) { notify("warning", "Too many recurring messages"); return; }
  recur_t *r = &g_recur[slot];
  r->active = 1;
  int gi = 0; for (; group[gi] && gi < 5; gi++) r->group[gi] = up(group[gi]);
  r->group[gi] = 0;
  s_cpy(r->text, text, sizeof(r->text));
  uint64_t now = hal_time_epoch();
  r->end = now + (uint64_t)secs;
  r->last = now;
  recur_broadcast(r);
  status("Recurring bulletin every 5 min");
  notify("info", "Recurring bulletin started");
}

/* Stop any recurring bulletin for [group]. */
static void recur_stop_group(const char *group) {
  for (int i = 0; i < RECUR_MAX; i++) {
    if (!g_recur[i].active) continue;
    int gmatch = 1;
    for (int k = 0; group[k] || g_recur[i].group[k]; k++)
      if (up(group[k]) != g_recur[i].group[k]) { gmatch = 0; break; }
    if (gmatch) { g_recur[i].active = 0; status("Recurring bulletin stopped"); }
  }
}

/* normalise to a 1-5 char uppercased alnum group name (no leading '#') */
static void norm_group(const char *src, char *out) {
  int j = 0; const char *p = src; if (*p == '#') p++;
  for (; *p && j < 5; p++) {
    char c = up(*p);
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out[j++] = c;
  }
  out[j] = 0;
}

/* "+" add-group: ask the host to show the preset/custom group picker. */
static const char *PRESET_GROUPS[] = {
  "ALL", "MISC", "TECH", "FUN", "WARN", "INFO", "NEWS", "TRADE",
  "WX", "EMCOM", "ARES", "NET", "DX", "EVENT", "HELP", "SOS"
};
static void prompt_group(void) {
  char chips[700] = "";
  for (unsigned i = 0; i < sizeof(PRESET_GROUPS) / sizeof(PRESET_GROUPS[0]); i++) {
    if (i) s_cat(chips, ",", sizeof(chips));
    s_cat(chips, "{\"label\":\"#", sizeof(chips));
    s_cat(chips, PRESET_GROUPS[i], sizeof(chips));
    s_cat(chips, "\",\"value\":\"", sizeof(chips));
    s_cat(chips, PRESET_GROUPS[i], sizeof(chips));
    s_cat(chips, "\"}", sizeof(chips));
  }
  char m[1100] = "{\"type\":\"ui.prompt\",\"id\":\"group\",\"title\":\"Add a group\","
                 "\"body\":\"Join an APRS bulletin group (max 5 letters).\",\"chips\":[";
  s_cat(m, chips, sizeof(m));
  s_cat(m, "],\"chipMode\":\"instant\",\"input\":{\"hint\":\"Custom\",\"max\":5,"
          "\"prefix\":\"#\"},\"confirm\":\"Add\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void prompt_newchat(void) {
  const char *m = "{\"type\":\"ui.prompt\",\"id\":\"newchat\",\"title\":\"New message\","
    "\"body\":\"Enter a callsign for a 1:1 chat, or #group.\","
    "\"input\":{\"hint\":\"Callsign or #group\",\"max\":20},\"confirm\":\"Open\"}";
  hal_msg_send(m, s_len(m));
}
static void prompt_recur(const char *convo) {
  char m[700] = "{\"type\":\"ui.prompt\",\"id\":\"recur\",\"title\":\"Recurring bulletin\","
                "\"body\":\"Repeat every 5 min into ";
  jesc(m, sizeof(m), convo);
  s_cat(m, " until the period ends.\",\"chips\":["
          "{\"label\":\"1 hour\",\"value\":\"3600\"},"
          "{\"label\":\"2 hours\",\"value\":\"7200\"},"
          "{\"label\":\"4 hours\",\"value\":\"14400\"},"
          "{\"label\":\"8 hours\",\"value\":\"28800\"},"
          "{\"label\":\"1 day\",\"value\":\"86400\"},"
          "{\"label\":\"2 days\",\"value\":\"172800\"}],"
          "\"chipMode\":\"select\",\"input\":{\"hint\":\"Message to repeat\",\"max\":67},"
          "\"confirm\":\"Start\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* "+"/✎/↻ header actions from the conversations widget. */
static void do_new_chat(void) { prompt_newchat(); }
static void do_add_group(void) { prompt_group(); }
static void do_recur(const char *buf) {
  char id[40] = ""; jstr(buf, "conversations_convo", id, sizeof(id));
  if (id[0] != '#') { notify("info", "Recurring is for groups only"); return; }
  prompt_recur(id);
}

/* Unpin a pinned item; if it is a group's recurring bulletin, also stop it. */
static void do_convo_unpin(const char *buf) {
  char id[40] = "", key[16] = "";
  jstr(buf, "conversations_convo", id, sizeof(id));
  jstr(buf, "conversations_pinkey", key, sizeof(key));
  if (id[0] == '#') recur_stop_group(id + 1);
  convo_unpin(id, key);
}

/* Result of a ui.prompt the host showed for us. */
static void do_prompt_result(const char *buf) {
  char pid[24] = "", val[40] = "", inp[80] = "";
  jstr(buf, "prompt_id", pid, sizeof(pid));
  jstr(buf, "prompt_value", val, sizeof(val));
  jstr(buf, "prompt_input", inp, sizeof(inp));
  if (s_eq(pid, "newchat")) {
    const char *src = inp;
    if (src[0] == '#') {
      char g[8]; norm_group(src, g);
      if (g[0]) { char id[10]; id[0] = '#'; s_cpy(id + 1, g, sizeof(id) - 1); convo_touch(id, "", 1); }
    } else if (src[0]) {
      char id[24]; int j = 0; for (int i = 0; src[i] && j < 23; i++) id[j++] = up(src[i]); id[j] = 0;
      convo_touch(id, "", 1);
    }
  } else if (s_eq(pid, "group")) {
    char g[8]; norm_group(val[0] ? val : inp, g);
    if (g[0]) { char id[10]; id[0] = '#'; s_cpy(id + 1, g, sizeof(id) - 1); convo_touch(id, "", 1); }
  } else if (s_eq(pid, "recur")) {
    char id[40] = ""; jstr(buf, "conversations_convo", id, sizeof(id));
    if (id[0] == '#' && inp[0]) recur_begin(id + 1, inp, to_int(val));
  }
}

/* Parse one TNC2 line and route it to the UI; bridge across transports when
 * relaying is on. via_ble = 1 if the frame arrived over BLE, 0 over APRS-IS.
 * A raw-frame dedup makes a message heard on both transports show once and
 * guards the relay against loops. */
/* ── Compact BLE frame ──────────────────────────────────────────────────
 * BLE legacy advertising only fits ~31 bytes, far less than a TNC2 frame, so
 * over BLE we use a compact form: "<from>\x1f<to>\x1f<text>" where `to` is a
 * callsign (1:1), "#GRP" (group), "!" (position; text = "lat,lon[,comment]"),
 * or "" (area/geo-chat text). Receivers (incl. ESP32) reconstruct routing
 * from these fields. The HAL just carries the bytes. */
#define BLE_SEP '\x1f'

static void ble_pack(char *out, unsigned max, const char *from,
                     const char *to, const char *text) {
  char sep[2] = { BLE_SEP, 0 };
  out[0] = 0;
  s_cat(out, from, max); s_cat(out, sep, max);
  s_cat(out, to, max);   s_cat(out, sep, max);
  s_cat(out, text, max);
}
static void ble_tx_from(const char *from, const char *to, const char *text) {
  if (!g_ble_on) return;
  char buf[220];
  ble_pack(buf, sizeof(buf), from, to, text);
  fseen_add(sig_hash("b", "", buf));   /* don't re-handle our own advert */
  ble_send(buf);
}
static void ble_tx_msg(const char *to, const char *text) {
  ble_tx_from(g_call, to, text);
}
static void ble_tx_pos(double lat, double lon, const char *comment) {
  char t[96] = "";
  append_dbl(t, sizeof(t), lat); s_cat(t, ",", sizeof(t));
  append_dbl(t, sizeof(t), lon);
  if (comment && comment[0]) { s_cat(t, ",", sizeof(t)); s_cat(t, comment, sizeof(t)); }
  ble_tx_msg("!", t);
}

/* Route one APRS-IS TNC2 line to the UI; bridge to BLE when relaying. */
static void route_frame(const char *line) {
  unsigned fh = sig_hash("f", "", line);
  if (fseen_has(fh)) return;
  fseen_add(fh);

  aprs_packet_t p;
  if (!aprs_parse(line, &p)) return;
  int mine = 1;
  for (int i = 0; g_call[i] || p.from[i]; i++) {
    if (up(g_call[i]) != up(p.from[i])) { mine = 0; break; }
  }
  if (mine) return;

  if (p.type == APRS_POSITION && p.has_pos) {
    push_marker(p.from, p.lat, p.lon, 0, p.comment);
    pos_set(p.from, p.lat, p.lon);
    if (convo_known(p.from)) convo_badge_only(p.from);
    if (p.comment[0]) {
      char meta[24] = ""; distance_to(p.lat, p.lon, meta, sizeof(meta));
      if (!geo_dup(p.from, p.comment))
        chat_append("geochat", "", "in", p.from, p.comment, "pos", 0, meta, p.lat, p.lon);
    }
  } else if (p.type == APRS_MESSAGE) {
    if (p.text[0] && !is_ack_text(p.text)) {
      if (p.is_bulletin) {
        char convo[40];
        convo[0] = '#'; int j = 1;
        for (int i = 0; p.group[i] && j < 39; i++) convo[j++] = p.group[i];
        convo[j] = 0;
        char preview[120] = ""; s_cpy(preview, p.from, sizeof(preview));
        s_cat(preview, ": ", sizeof(preview)); s_cat(preview, p.text, sizeof(preview));
        convo_deliver(convo, "in", p.from, p.text, preview, 0);
        if (g_ble_relay && g_ble_on) ble_tx_from(p.from, convo, p.text);
      } else {
        char meta[24] = ""; double slat = 0, slon = 0;
        if (pos_get(p.from, &slat, &slon)) distance_to(slat, slon, meta, sizeof(meta));
        if (!geo_dup(p.from, p.text))
          chat_append("geochat", "", "in", p.from, p.text, "msg", 0, meta, slat, slon);
        int amine = 1;
        for (int i = 0; g_call[i] || p.addressee[i]; i++) {
          if (up(g_call[i]) != up(p.addressee[i])) { amine = 0; break; }
        }
        if (amine) convo_deliver(p.from, "in", p.from, p.text, p.text, 0);
        if (g_ble_relay && g_ble_on) ble_tx_from(p.from, p.addressee, p.text);
      }
    }
  }
  /* relay a position to BLE with its real coords/comment */
  if (g_ble_relay && g_ble_on && p.type == APRS_POSITION && p.has_pos) {
    char t[96] = "";
    append_dbl(t, sizeof(t), p.lat); s_cat(t, ",", sizeof(t));
    append_dbl(t, sizeof(t), p.lon);
    if (p.comment[0]) { s_cat(t, ",", sizeof(t)); s_cat(t, p.comment, sizeof(t)); }
    ble_tx_from(p.from, "!", t);
  }
}

/* Handle one compact frame received over BLE; bridge to APRS-IS when relaying. */
static void ble_handle(const char *compact) {
  unsigned h = sig_hash("b", "", compact);
  if (fseen_has(h)) return;
  fseen_add(h);

  char from[16] = "", to[24] = "", text[200] = "";
  int seg = 0, fi = 0, ti = 0, xi = 0;
  for (const char *q = compact; *q; q++) {
    if (*q == BLE_SEP) { seg++; continue; }
    if (seg == 0) { if (fi < 15) from[fi++] = *q; }
    else if (seg == 1) { if (ti < 23) to[ti++] = *q; }
    else { if (xi < 199) text[xi++] = *q; }
  }
  from[fi] = 0; to[ti] = 0; text[xi] = 0;
  if (!from[0]) return;
  int mine = 1;
  for (int i = 0; g_call[i] || from[i]; i++) {
    if (up(g_call[i]) != up(from[i])) { mine = 0; break; }
  }
  if (mine) return;

  /* Mesh repeater: rebroadcast this frame once (within ~2s, via the advertise
   * rotation), ignoring content already repeated in the last 10 minutes. */
  {
    uint64_t now = hal_time_epoch();
    if (!rpt_recent(h, now)) { rpt_mark(h, now); ble_send(compact); }
  }

  if (s_eq(to, "!")) {                    /* position: "lat,lon[,comment]" */
    char a[24] = "", b[24] = "", comment[80] = "";
    int s2 = 0, ai = 0, bi = 0, ci = 0;
    for (const char *q = text; *q; q++) {
      if (*q == ',' && s2 < 2) { s2++; continue; }
      if (s2 == 0) { if (ai < 23) a[ai++] = *q; }
      else if (s2 == 1) { if (bi < 23) b[bi++] = *q; }
      else { if (ci < 79) comment[ci++] = *q; }
    }
    a[ai] = 0; b[bi] = 0; comment[ci] = 0;
    double lat = to_dbl(a), lon = to_dbl(b);
    push_marker(from, lat, lon, 0, comment);
    pos_set(from, lat, lon);
    if (convo_known(from)) convo_badge_only(from);
    if (comment[0]) {
      char meta[24] = ""; distance_to(lat, lon, meta, sizeof(meta));
      if (!geo_dup(from, comment))
        chat_append("geochat", "", "in", from, comment, "pos", 0, meta, lat, lon);
    }
  } else if (to[0] == '#') {              /* group */
    char preview[120] = ""; s_cpy(preview, from, sizeof(preview));
    s_cat(preview, ": ", sizeof(preview)); s_cat(preview, text, sizeof(preview));
    convo_deliver(to, "in", from, text, preview, 0);
    if (g_ble_relay && g_logged) {
      char line[260]; aprs_build_bulletin(line, sizeof(line), from, to + 1, '0', text);
      char tp[340]; s_cpy(tp, g_call, sizeof(tp));
      s_cat(tp, ">APRS,TCPIP*:}", sizeof(tp)); s_cat(tp, line, sizeof(tp));
      aprs_send_raw(g_sock, tp);
    }
  } else if (!to[0]) {                    /* area / geo-chat broadcast text */
    char meta[24] = ""; double slat = 0, slon = 0;
    if (pos_get(from, &slat, &slon)) distance_to(slat, slon, meta, sizeof(meta));
    if (!geo_dup(from, text))
      chat_append("geochat", "", "in", from, text, "msg", 0, meta, slat, slon);
  } else {                               /* 1:1 to a callsign */
    int amine = 1;
    for (int i = 0; g_call[i] || to[i]; i++) {
      if (up(g_call[i]) != up(to[i])) { amine = 0; break; }
    }
    if (amine) {
      convo_deliver(from, "in", from, text, text, 0);
    } else {
      char meta[24] = ""; double slat = 0, slon = 0;
      if (pos_get(from, &slat, &slon)) distance_to(slat, slon, meta, sizeof(meta));
      if (!geo_dup(from, text))
        chat_append("geochat", "", "in", from, text, "msg", 0, meta, slat, slon);
    }
    if (g_ble_relay && g_logged) {
      char line[260]; aprs_build_message(line, sizeof(line), from, to, text, 0);
      char tp[340]; s_cpy(tp, g_call, sizeof(tp));
      s_cat(tp, ">APRS,TCPIP*:}", sizeof(tp)); s_cat(tp, line, sizeof(tp));
      aprs_send_raw(g_sock, tp);
    }
  }
}

/* Reconcile the BLE transport with the g_ble_on setting (start/stop scan). */
static void ble_reconcile(void) {
  if (g_ble_on && !g_ble_started) {
    ble_start();
    g_ble_started = 1;
    status("Bluetooth on");
    notify("info", "Exchanging APRS over Bluetooth");
  } else if (!g_ble_on && g_ble_started) {
    ble_stop();
    g_ble_started = 0;
    status("Bluetooth off");
  }
}

/* ── module entry points ────────────────────────────────────────────── */
void module_init(void) {
  hal_log(1, "[aprs] init", 11);
  /* Default callsign = THIS device's profile callsign (so each device
   * transmits as itself, not a hardcoded one). The user's Settings callsign,
   * if set, overrides this via read_config. */
  char id[16];
  uint32_t n = hal_identity(id, sizeof(id) - 1);
  if (n > 0 && n < sizeof(id)) { id[n] = 0; if (id[0]) s_cpy(g_call, id, sizeof(g_call)); }
  status("APRS ready - connecting to APRS-IS automatically...");
  /* Ask the host to run our "connect" command with the current settings
   * (auto-connect on load; no manual Connect needed). */
  const char *m = "{\"type\":\"host.run_command\",\"command\":\"connect\"}";
  hal_msg_send(m, s_len(m));
}

void module_tick(void) {
  /* BLE runs independently of the internet link (off-grid). Reconcile the
   * scan/advertise state, drain inbound frames, and beacon our position. */
  ble_reconcile();
  push_status();   /* refresh APRS-IS / BLE indicators (only on change) */
  if (g_ble_on) {
    char rec[400];
    for (int guard = 0; guard < 20; guard++) {
      if (ble_poll(rec, sizeof(rec)) <= 0) break;
      char frame[300]; jstr(rec, "data", frame, sizeof(frame));
      if (frame[0]) ble_handle(frame);
    }
    if (g_lat != 0 || g_lon != 0) {
      uint64_t now = hal_time_epoch();
      int iv = g_interval > 0 ? g_interval : 600;
      if (now - g_ble_last_beacon >= (uint64_t)iv) {
        ble_tx_pos(g_lat, g_lon, "");   /* keep it short to fit legacy adverts */
        g_ble_last_beacon = now;
      }
    }
  }

  /* Auto-reconnect: keep retrying (5s backoff) while we want a link. */
  if (g_sock < 0) {
    if (!g_want_connect) return;
    uint64_t now = hal_time_epoch();
    if (now - g_last_reconnect < 5) return;
    g_last_reconnect = now;
    g_logged = 0;
    g_sock = aprs_connect(g_host, g_port);
    if (g_sock >= 0) status("Reconnecting to APRS-IS...");
    return;
  }

  if (!g_logged) {
    int st = hal_socket_status(g_sock);
    if (st == 1) {
      int pass = aprs_passcode(g_call);
      aprs_login(g_sock, g_call, pass, g_lat, g_lon, g_radius);
      g_logged = 1;
      char b[64] = "Connected. passcode "; char nb[16];
      { int v = pass, j = 0; char t[12]; if (v == 0) t[j++]='0'; while (v>0){t[j++]=(char)('0'+v%10);v/=10;} int k=0; while(j>0)nb[k++]=t[--j]; nb[k]=0; }
      s_cat(b, nb, sizeof(b)); status(b);
      /* No toast on (re)connect — the APRS-IS indicator shows the state and a
       * flapping link would otherwise flicker notifications. */
      center_map();
      push_radius();
    } else if (st == 2) {
      /* connect failed — drop and let the reconnect path retry */
      aprs_disconnect(g_sock); g_sock = -1;
      status("Connection failed - retrying...");
    }
    return;
  }

  /* logged in: detect a dropped connection and reconnect */
  if (hal_socket_status(g_sock) == 2) {
    aprs_disconnect(g_sock); g_sock = -1; g_logged = 0;
    status("Connection lost - reconnecting...");
    /* No toast — the APRS-IS indicator turns grey; reconnection is automatic. */
    return;
  }

  /* drain inbound packets from APRS-IS */
  char line[512];
  for (int guard = 0; guard < 40; guard++) {
    int n = aprs_poll_line(g_sock, line, sizeof(line));
    if (n <= 0) break;
    route_frame(line);
  }

  /* timed beacon */
  if (g_auto && g_logged) {
    uint64_t now = hal_time_epoch();
    if (now - g_last_beacon >= (uint64_t)g_interval) {
      aprs_send_beacon(g_sock, g_call, g_lat, g_lon, g_symbol, "TCPIP*", "Aurora auto-beacon");
      push_marker(g_call, g_lat, g_lon, "blue", "Aurora auto-beacon");
      g_last_beacon = now;
      status("TX auto-beacon");
    }
  }

  /* recurring group bulletins: re-broadcast every 5 min until the period ends */
  if (g_logged || g_ble_on) {
    uint64_t now = hal_time_epoch();
    for (int i = 0; i < RECUR_MAX; i++) {
      recur_t *r = &g_recur[i];
      if (!r->active) continue;
      if (now >= r->end) { r->active = 0; continue; }
      if (now - r->last >= RECUR_INTERVAL) {
        recur_broadcast(r);
        r->last = now;
      }
    }
  }
}

void module_handle_event(void) {
  char buf[4096];
  if (hal_msg_available() == 0) return;
  uint32_t n = hal_msg_recv(buf, sizeof(buf) - 1);
  if (n == 0) return;
  buf[n] = 0;

  char cmd[40];
  if (!jstr(buf, "command", cmd, sizeof(cmd))) return;
  if (s_eq(cmd, "connect")) do_connect(buf);
  else if (s_eq(cmd, "disconnect")) {
    g_want_connect = 0;            /* stop auto-reconnect */
    if (g_sock >= 0) { aprs_disconnect(g_sock); g_sock = -1; g_logged = 0; }
    status("Disconnected"); notify("info", "Disconnected");
  } else if (s_eq(cmd, "center")) { read_config(buf); center_map(); }
  else if (s_eq(cmd, "send_beacon")) do_beacon(buf, 0);
  else if (s_eq(cmd, "send_emergency")) do_beacon(buf, 1);
  else if (s_eq(cmd, "toggle_timed")) {
    read_config(buf); g_auto = !g_auto; g_last_beacon = 0;
    status(g_auto ? "Auto-beacon ON" : "Auto-beacon OFF");
    notify("info", g_auto ? "Auto-beacon enabled" : "Auto-beacon disabled");
  } else if (s_eq(cmd, "conversations_send")) do_convo_send(buf);
  else if (s_eq(cmd, "conversations_unpin")) do_convo_unpin(buf);
  else if (s_eq(cmd, "new_chat")) do_new_chat();
  else if (s_eq(cmd, "add_group")) do_add_group();
  else if (s_eq(cmd, "recur")) do_recur(buf);
  else if (s_eq(cmd, "prompt")) do_prompt_result(buf);
  else if (s_eq(cmd, "set_radius")) do_set_radius(buf);
  else if (s_eq(cmd, "geochat_send")) do_geochat_send(buf);
  else if (s_eq(cmd, "ble_apply")) {
    read_config(buf);
    g_ble_on = jbool_def(buf, "ble_enabled", 1);
    g_ble_relay = jbool_def(buf, "ble_relay", 0);
    ble_reconcile();
  }
  else if (s_eq(cmd, "marker_tap")) {
    char id[24] = ""; jstr(buf, "id", id, sizeof(id));
    char b[64] = "Station: "; s_cat(b, id, sizeof(b)); status(b);
  }
}

void module_destroy(void) {
  if (g_sock >= 0) { aprs_disconnect(g_sock); g_sock = -1; }
}

int32_t module_tick_interval_ms(void) { return 1000; }
