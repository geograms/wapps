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
static int   g_mail_days = 7;           /* ?MAIL look-back window sent to iGates */
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

/* ── Public-key beacon ───────────────────────────────────────────────────
 * Periodically broadcast this station's public key so peers can map our
 * callsign -> pubkey and later send us encrypted messages. The host hands us
 * the key as base64url of the raw 32 bytes (43 chars — an npub bech32 string
 * would be 63, too tight for one 67-char APRS message and bulky on a BLE
 * advert). Sent as an APRS bulletin to the well-known group "NOSTR" (the
 * addressee BLN..NOSTR is the "code"; the frame's from-field is the callsign;
 * the text is the base64 key) and, identically, over BLE as "#NOSTR". The key
 * never changes, so the rate is low. ON by default; the user can disable it in
 * Settings (persisted to KV). Receivers base64url-decode it back to 32 bytes.
 */
#define PKBEACON_GROUP    "NOSTR"
#define PKBEACON_INTERVAL 1800            /* seconds (30 min) */
static char  g_pubkey[80] = "";           /* our pubkey (base64url), cached at init */
static int   g_pubkey_beacon = 1;         /* broadcast it? (default on) */
static uint64_t g_last_pkbeacon = 0;

/* ── Message signing (APRX verifiable authorship) ────────────────────────
 * When enabled, outgoing messages carry a short-Schnorr signature so peers can
 * verify the author. The signature is 48 bytes -> 60 base85 chars, appended as
 * " ~<sig>" (one extra APRS line). Verification needs the sender's pubkey, kept
 * in a callsign->pubkey map filled from received NOSTR beacons (§10). Both the
 * crypto and the base85 live host-side (hal_identity_sign / hal_verify); the
 * private key never reaches the wapp. OFF by default (a signature ~doubles a
 * short message); persisted in KV. */
static int g_sign_msgs = 0;
#define PK_MAX 64
static char g_pk_call[PK_MAX][16];        /* callsign -> */
static char g_pk_key[PK_MAX][48];         /* pubkey (base64url, ~43 chars) */
static uint64_t g_pk_ts[PK_MAX];          /* last time we heard this station's key */
static int  g_pk_n = 0;
static void pk_render(void);              /* fwd: refresh the Keys list view */

/* ── followed callsigns (Activity feed) ─────────────────────────────────────
 * A Twitter-style stream: callsigns we "follow" have their public activity
 * (posts, replies, likes, status) surfaced in the Activity tab. We pull their
 * packets from APRS-IS with a b/ budlist filter (every packet FROM them) and,
 * over BLE, hear them whenever they're in range. The list persists in KV
 * "follows" (";"-joined). Our own micro-posts go out as bulletins to a shared
 * feed group so followers see them too. */
#define FOLLOW_MAX 32
#define FEED_GROUP  "FEED"                 /* shared micro-blog group for posts */
static char g_follow[FOLLOW_MAX][16];
static char g_ftag[FOLLOW_MAX][48];        /* space-separated tags, per follow */
static int  g_follow_n = 0;
/* Stations that follow US — learned from directed "?FOLLOW"/"?UNFOLLOW"
 * control messages peers send when they (un)follow a callsign. */
static char g_follower[FOLLOW_MAX][16];
static int  g_follower_n = 0;
static void follow_render(void);          /* fwd: push the people list */
static void profile_show(const char *call);   /* fwd: station profile sheet */
static void prompt_ftag(const char *call);    /* fwd: edit-tags prompt */
static int is_following(const char *call) {
  for (int i = 0; i < g_follow_n; i++) if (s_eq(g_follow[i], call)) return 1;
  return 0;
}
static int is_follower(const char *call) {
  for (int i = 0; i < g_follower_n; i++) if (s_eq(g_follower[i], call)) return 1;
  return 0;
}

/* ── BLE ping (Tools tab): local reach test across digipeaters ──────────
 * A ping is a BLE-only broadcast (never APRS-IS, never shown on the Live
 * feed). Every BLE station answers once with its callsign + position and
 * forwards the ping (ttl) so it travels further; replies are forwarded back
 * (pttl) so multi-hop responders still reach the pinger. */
#define PING_TO "?PING"
#define PONG_TO "?PONG"
#define PING_DEFAULT_TTL 3
#define GPS_NA (-2147483647 - 1)   /* hal_sensor_gps_* "unavailable" sentinel */
static int      g_ping_active = 0;   /* collecting replies for our ping */
static unsigned g_ping_id = 0;
static uint64_t g_ping_start = 0;
static unsigned g_ping_seq = 0;

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
/* via: transport the message arrived on ("BLE"/"NET"); "" for our own sends.
 * The host renders it as a small origin chip so users can tell where a
 * received message came from. */
static void chat_append(const char *field, const char *convo, const char *dir,
                        const char *from, const char *text, const char *kind,
                        int recur, const char *meta, double lat, double lon,
                        const char *via) {
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
  s_cat(m, "\"", sizeof(m));
  if (via && via[0]) {
    s_cat(m, ",\"via\":\"", sizeof(m)); jesc(m, sizeof(m), via);
    s_cat(m, "\"", sizeof(m));
  }
  if (recur) s_cat(m, ",\"recur\":true,\"time\":\"", sizeof(m));
  else s_cat(m, ",\"time\":\"", sizeof(m));
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

/* Ask the host to replay archived Live geo-chat for the current area into the
 * Live tab. The host persists every geo-tagged Live message and answers this
 * by centre+radius, so opening the wapp (or changing the radius) brings back
 * the older messages that happened in the selected region. */
static void request_history(void) {
  char m[200] = "{\"type\":\"ui.chat.history\",\"field\":\"geochat\",\"lat\":";
  append_dbl(m, sizeof(m), g_lat);
  s_cat(m, ",\"lon\":", sizeof(m)); append_dbl(m, sizeof(m), g_lon);
  s_cat(m, ",\"radius_km\":", sizeof(m));
  { char nb[12]; u_itoa((unsigned)g_radius, nb); s_cat(m, nb, sizeof(m)); }
  s_cat(m, ",\"limit\":200}", sizeof(m));
  hal_msg_send(m, s_len(m));
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
  if (jstr(buf, "mail_days", v, sizeof(v)) && v[0]) { g_mail_days = to_int(v); if (g_mail_days < 1) g_mail_days = 1; }
  /* NOTE: BLE on/off is intentionally NOT read here. read_config runs on
   * every command (connect, sends, …) and the host serialises an unset
   * checkbox as false, which would clobber the on-by-default state before the
   * user ever touches Settings. BLE state is owned by init (default on) and
   * the explicit "Apply Bluetooth" action instead — see the ble_apply cmd. */
}

static void do_connect(const char *buf) {
  read_config(buf);
  request_history();   /* bring back this area's older Live messages on open */
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

/* ── SHA-1 (RFC 3174) — only for short, stable message ids ────────────────
 * A reply references its parent by a 4-hex-char id both sender and receiver
 * derive from the same content (from|text), so threads work across APRS-IS and
 * BLE without any extra wire fields. Inputs are short (<~220B); a fixed buffer
 * is plenty. */
static uint32_t sha1_rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
static void sha1(const unsigned char *msg, unsigned len, unsigned char out[20]) {
  uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
  unsigned char buf[384];
  if (len > 256) len = 256;
  for (unsigned i = 0; i < len; i++) buf[i] = msg[i];
  uint64_t ml = (uint64_t)len * 8u;
  unsigned n = len;
  buf[n++] = 0x80;
  while ((n % 64u) != 56u) buf[n++] = 0;
  for (int i = 7; i >= 0; i--) buf[n++] = (unsigned char)((ml >> (i * 8)) & 0xffu);
  for (unsigned off = 0; off < n; off += 64) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
      w[i] = ((uint32_t)buf[off + i*4] << 24) | ((uint32_t)buf[off + i*4 + 1] << 16) |
             ((uint32_t)buf[off + i*4 + 2] << 8) | (uint32_t)buf[off + i*4 + 3];
    for (int i = 16; i < 80; i++) w[i] = sha1_rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
      uint32_t f, k;
      if (i < 20)      { f = (b & c) | ((~b) & d);            k = 0x5A827999u; }
      else if (i < 40) { f = b ^ c ^ d;                       k = 0x6ED9EBA1u; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d);     k = 0x8F1BBCDCu; }
      else             { f = b ^ c ^ d;                       k = 0xCA62C1D6u; }
      uint32_t t = sha1_rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = sha1_rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  for (int i = 0; i < 5; i++) {
    out[i*4]   = (unsigned char)(h[i] >> 24);
    out[i*4+1] = (unsigned char)(h[i] >> 16);
    out[i*4+2] = (unsigned char)(h[i] >> 8);
    out[i*4+3] = (unsigned char)(h[i]);
  }
}
/* First 4 hex chars of sha1("from|text") — the thread id of a message. */
static void msg_id(const char *from, const char *text, char out[5]) {
  unsigned char in[280]; unsigned n = 0;
  for (const char *p = from; *p && n < 270; p++) in[n++] = (unsigned char)*p;
  in[n++] = '|';
  for (const char *p = text; *p && n < 279; p++) in[n++] = (unsigned char)*p;
  unsigned char d[20]; sha1(in, n, d);
  static const char hx[] = "0123456789abcdef";
  out[0] = hx[d[0] >> 4]; out[1] = hx[d[0] & 15];
  out[2] = hx[d[1] >> 4]; out[3] = hx[d[1] & 15]; out[4] = 0;
}
/* Thread reply marker on the wire: "+<4hex> <text>". If present, copies the
 * parent id into [parent] (5 bytes) and points *disp at the text after the
 * marker; otherwise parent="" and *disp = wire. Returns 1 if a marker was found. */
static int thread_parse(const char *wire, char parent[5], const char **disp) {
  parent[0] = 0; *disp = wire;
  if (wire[0] != '+') return 0;
  for (int i = 0; i < 4; i++) {
    char ch = wire[1 + i];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return 0;
  }
  if (wire[5] != ' ') return 0;
  for (int i = 0; i < 4; i++) parent[i] = wire[1 + i];
  parent[4] = 0; *disp = wire + 6;
  return 1;
}
/* "Like" marker on the wire: "<4hex>:like" / "<4hex>:unlike" — a vote on the
 * message whose thread id is <4hex>. Deliberately human-readable (no special
 * leading byte) so any APRS client, not just Aurora, can like a topic by
 * sending e.g. "b9fb:like" to the group. On match copies the target id into
 * [tgt] (5 bytes), sets *unlike, and returns 1. */
static int like_parse(const char *wire, char tgt[5], int *unlike) {
  tgt[0] = 0; *unlike = 0;
  for (int i = 0; i < 4; i++) {
    char ch = wire[i];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return 0;
  }
  if (wire[4] != ':') return 0;
  const char *v = wire + 5;
  if (s_eq(v, "like")) *unlike = 0;
  else if (s_eq(v, "unlike")) *unlike = 1;
  else return 0;
  for (int i = 0; i < 4; i++) tgt[i] = wire[i];
  tgt[4] = 0;
  return 1;
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

/* Popup notification for an incoming chat message. Only for: Live-tab geo-chat
 * messages, direct messages addressed to us, and group/bulletin messages for a
 * group we are subscribed to (i.e. in our conversation list). NOT beacons. The
 * host shows it as a system notification when we're in the background (so the
 * user is alerted with the screen off / app closed) and an in-app card when the
 * page is open. Content-deduped (own ring) so the same message arriving via both
 * APRS-IS and BLE — or a repeated bulletin — pops only once. */
static int notif_dup(const char *from, const char *text) {
  unsigned h = sig_hash("ntf", from, text);
  if (fseen_has(h)) return 1;
  fseen_add(h);
  return 0;
}
static void notify_msg(const char *title, const char *from, const char *text,
                       const char *body) {
  if (notif_dup(from, text)) return;
  char m[480] = "{\"type\":\"notify\",\"level\":\"info\",\"title\":\"";
  jesc(m, sizeof(m), title);
  s_cat(m, "\",\"body\":\"", sizeof(m));
  jesc(m, sizeof(m), body);
  s_cat(m, "\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
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
/* km from us to a callsign's last-known position, or -1 if unknown. */
static double km_to_call(const char *call) {
  double lat, lon;
  if (!pos_get(call, &lat, &lon)) return -1.0;
  if (g_lat == 0 && g_lon == 0) return -1.0;
  const double D2R = 0.0174532925199433;
  double x = (lon - g_lon) * D2R * m_cos((g_lat + lat) * 0.5 * D2R);
  double y = (lat - g_lat) * D2R;
  return 6371.0 * __builtin_sqrt(x * x + y * y);
}
/* True only when we positively know the sender sits inside our coverage radius
 * (so a local group bulletin can be filed as "local"); unknown position = no. */
static int within_radius(const char *call) {
  double km = km_to_call(call);
  return km >= 0 && km <= (double)g_radius;
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
static void groups_save(void);   /* fwd: persist subscribed groups to KV */

/* ── generic ui.convo.* senders ── */
/* append "lat":..,"lon":.. to m when the position is known (not 0,0). */
static void cat_pos(char *m, unsigned sz, double lat, double lon) {
  if (lat == 0 && lon == 0) return;
  s_cat(m, ",\"lat\":", sz); append_dbl(m, sz, lat);
  s_cat(m, ",\"lon\":", sz); append_dbl(m, sz, lon);
}
/* Append optional thread fields ("mid" = this message's 4-hex id, "parent" =
 * the id it replies to). Empty values are omitted so non-threaded chats and the
 * host's generic store are unaffected. */
static void cat_thread(char *m, unsigned sz, const char *mid, const char *parent,
                       const char *auth, int enc) {
  if (mid && mid[0]) { s_cat(m, ",\"mid\":\"", sz); s_cat(m, mid, sz); s_cat(m, "\"", sz); }
  if (parent && parent[0]) { s_cat(m, ",\"parent\":\"", sz); s_cat(m, parent, sz); s_cat(m, "\"", sz); }
  /* Signature verdict (APRX): verified / bad / unverified. Empty = unsigned. */
  if (auth && auth[0]) { s_cat(m, ",\"auth\":\"", sz); s_cat(m, auth, sz); s_cat(m, "\"", sz); }
  /* Encrypted (APRX 1:1): host shows a lock badge. */
  if (enc) s_cat(m, ",\"enc\":true", sz);
}
static void convo_msg(const char *id, const char *dir, const char *from,
                      const char *text, const char *key, const char *meta,
                      double lat, double lon, const char *via,
                      const char *mid, const char *parent, const char *auth, int enc) {
  char t[8]; fmt_time(t);
  char m[640] = "{\"type\":\"ui.convo.msg\",\"id\":\"";
  jesc(m, sizeof(m), id);
  s_cat(m, "\",\"dir\":\"", sizeof(m)); s_cat(m, dir, sizeof(m));
  s_cat(m, "\",\"from\":\"", sizeof(m)); jesc(m, sizeof(m), from);
  s_cat(m, "\",\"text\":\"", sizeof(m)); jesc(m, sizeof(m), text);
  s_cat(m, "\",\"key\":\"", sizeof(m)); s_cat(m, key, sizeof(m));
  s_cat(m, "\",\"meta\":\"", sizeof(m)); jesc(m, sizeof(m), meta);
  s_cat(m, "\"", sizeof(m));
  if (via && via[0]) {
    s_cat(m, ",\"via\":\"", sizeof(m)); jesc(m, sizeof(m), via);
    s_cat(m, "\"", sizeof(m));
  }
  s_cat(m, ",\"time\":\"", sizeof(m)); s_cat(m, t, sizeof(m));
  s_cat(m, "\"", sizeof(m)); cat_pos(m, sizeof(m), lat, lon);
  cat_thread(m, sizeof(m), mid, parent, auth, enc);
  s_cat(m, "}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
static void convo_pin(const char *id, const char *key, const char *dir,
                      const char *from, const char *text, const char *meta,
                      double lat, double lon, const char *via,
                      const char *mid, const char *parent, const char *auth, int enc) {
  char t[8]; fmt_time(t);
  char m[640] = "{\"type\":\"ui.convo.pin\",\"id\":\"";
  jesc(m, sizeof(m), id);
  s_cat(m, "\",\"key\":\"", sizeof(m)); s_cat(m, key, sizeof(m));
  s_cat(m, "\",\"dir\":\"", sizeof(m)); s_cat(m, dir, sizeof(m));
  s_cat(m, "\",\"from\":\"", sizeof(m)); jesc(m, sizeof(m), from);
  s_cat(m, "\",\"text\":\"", sizeof(m)); jesc(m, sizeof(m), text);
  s_cat(m, "\",\"meta\":\"", sizeof(m)); jesc(m, sizeof(m), meta);
  s_cat(m, "\"", sizeof(m));
  if (via && via[0]) {
    s_cat(m, ",\"via\":\"", sizeof(m)); jesc(m, sizeof(m), via);
    s_cat(m, "\"", sizeof(m));
  }
  s_cat(m, ",\"time\":\"", sizeof(m)); s_cat(m, t, sizeof(m));
  s_cat(m, "\"", sizeof(m)); cat_pos(m, sizeof(m), lat, lon);
  cat_thread(m, sizeof(m), mid, parent, auth, enc);
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
/* A "like" vote on message [mid] by station [from]. The host owns the tally:
 * it keeps the set of likers per message id (so each callsign counts once) and
 * derives the count + whether *we* liked it. [remove] retracts the like;
 * [mine] flags our own vote so the host can light the heart. App-agnostic on
 * the host side (a generic reaction by an opaque actor id). */
static void convo_react(const char *id, const char *mid, const char *from,
                        int remove, int mine) {
  char m[220] = "{\"type\":\"ui.convo.react\",\"id\":\"";
  jesc(m, sizeof(m), id);
  s_cat(m, "\",\"mid\":\"", sizeof(m)); s_cat(m, mid, sizeof(m));
  s_cat(m, "\",\"from\":\"", sizeof(m)); jesc(m, sizeof(m), from);
  s_cat(m, "\"", sizeof(m));
  if (remove) s_cat(m, ",\"remove\":true", sizeof(m));
  if (mine) s_cat(m, ",\"mine\":true", sizeof(m));
  s_cat(m, "}", sizeof(m));
  hal_msg_send(m, s_len(m));
}
/* Display title for a conversation row. Groups show the bare name; local vs
 * global (trailing '*') is conveyed by the row icon (campaign vs public/globe)
 * plus a " · global"/" · local" tag (ASCII — the host renders titles as latin1,
 * so no emoji). 1:1 chats keep the callsign. */
static void convo_title(const char *id, char *out, unsigned osz) {
  if (id[0] != '#') { s_cpy(out, id, osz); return; }
  out[0] = 0;
  char name[8]; int j = 0;
  for (int i = 1; id[i] && id[i] != '*' && j < 6; i++) name[j++] = id[i];
  name[j] = 0;
  int global = 0; for (int i = 1; id[i]; i++) if (id[i] == '*') global = 1;
  s_cat(out, name, osz);
  s_cat(out, global ? " (global)" : " (local)", osz);   /* ASCII-only tag */
}
/* Refresh a conversation row (title/preview/icon + distance badge). */
static void convo_touch(const char *id, const char *preview, int select) {
  convo_remember(id);
  int global = 0; for (int i = 1; id[i]; i++) if (id[i] == '*') global = 1;
  const char *icon = (id[0] == '#') ? (global ? "public" : "campaign") : "person";
  char badge[24] = "";
  if (id[0] != '#') distance_badge(id, badge, sizeof(badge));
  char title[24]; convo_title(id, title, sizeof(title));
  char m[600] = "{\"type\":\"ui.convo.upsert\",\"id\":\"";
  jesc(m, sizeof(m), id);
  s_cat(m, "\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), title);
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

/* ── APRX message signatures ──────────────────────────────────────────── */
/* base85 alphabet — must match the host (lib/util/aprx_sign.dart). */
static int is_b85(char c) {
  if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
    return 1;
  const char *p = ".-+=^!/*?&<>()[]%$#@,;_";
  for (; *p; p++) if (*p == c) return 1;
  return 0;
}
/* A signed message ends with " ~<60 base85 chars>". If present, copy the body
 * (without that suffix) into [core] and the 60-char signature into [sig], and
 * return 1; else 0. */
#define SIG_B85_LEN 60
static int sig_split(const char *text, char *core, unsigned coresz,
                     char *sig, unsigned sigsz) {
  int n = (int)s_len(text);
  if (n < SIG_B85_LEN + 2) return 0;
  int s0 = n - SIG_B85_LEN;
  if (text[s0 - 1] != '~' || text[s0 - 2] != ' ') return 0;
  for (int i = s0; i < n; i++) if (!is_b85(text[i])) return 0;
  int clen = s0 - 2;
  if ((unsigned)clen >= coresz) clen = (int)coresz - 1;
  for (int i = 0; i < clen; i++) core[i] = text[i];
  core[clen] = 0;
  unsigned j = 0;
  for (int i = s0; i < n && j + 1 < sigsz; i++) sig[j++] = text[i];
  sig[j] = 0;
  return 1;
}
/* canonical signed bytes = "<from>|<core>" (must match the signer) */
static void sig_canon(char *out, unsigned sz, const char *from, const char *core) {
  out[0] = 0; s_cat(out, from, sz); s_cat(out, "|", sz); s_cat(out, core, sz);
}

/* ── callsign -> pubkey map (filled from received NOSTR beacons) ───────── */
static char g_pk_scratch[PK_MAX * 64];
static const char *pk_get(const char *call) {
  for (int i = 0; i < g_pk_n; i++) if (s_eq(g_pk_call[i], call)) return g_pk_key[i];
  return 0;
}
static void pk_save(void) {
  g_pk_scratch[0] = 0;
  for (int i = 0; i < g_pk_n; i++) {
    char tb[12]; u_itoa((unsigned)g_pk_ts[i], tb);
    s_cat(g_pk_scratch, g_pk_call[i], sizeof(g_pk_scratch));
    s_cat(g_pk_scratch, "=", sizeof(g_pk_scratch));
    s_cat(g_pk_scratch, g_pk_key[i], sizeof(g_pk_scratch));
    s_cat(g_pk_scratch, "=", sizeof(g_pk_scratch));   /* base64url has no '=' */
    s_cat(g_pk_scratch, tb, sizeof(g_pk_scratch));
    s_cat(g_pk_scratch, ";", sizeof(g_pk_scratch));
  }
  hal_kv_set("pubkeys", 7, g_pk_scratch, s_len(g_pk_scratch));
}
static void pk_store(const char *call, const char *key) {
  if (!call[0] || !key[0] || s_eq(call, g_call)) return;
  for (int i = 0; i < g_pk_n; i++) if (s_eq(g_pk_call[i], call)) {
    g_pk_ts[i] = hal_time_epoch();
    if (!s_eq(g_pk_key[i], key)) { s_cpy(g_pk_key[i], key, sizeof(g_pk_key[i])); pk_save(); }
    pk_render();
    return;
  }
  if (g_pk_n >= PK_MAX) return;
  s_cpy(g_pk_call[g_pk_n], call, sizeof(g_pk_call[0]));
  s_cpy(g_pk_key[g_pk_n], key, sizeof(g_pk_key[0]));
  g_pk_ts[g_pk_n] = hal_time_epoch();
  g_pk_n++;
  pk_save();
  pk_render();
}
static void pk_load(void) {
  uint32_t n = hal_kv_get("pubkeys", 7, g_pk_scratch, sizeof(g_pk_scratch) - 1);
  if (n == 0) return;
  g_pk_scratch[n] = 0;
  char call[16], key[48], ts[12]; int ci = 0, ki = 0, ti = 0, stage = 0;
  for (unsigned i = 0; i <= n; i++) {
    char c = (i < n) ? g_pk_scratch[i] : ';';
    if (c == ';') {
      if (ci > 0 && ki > 0 && g_pk_n < PK_MAX) {
        call[ci] = 0; key[ki] = 0; ts[ti] = 0;
        s_cpy(g_pk_call[g_pk_n], call, 16); s_cpy(g_pk_key[g_pk_n], key, 48);
        g_pk_ts[g_pk_n] = ti > 0 ? (uint64_t)to_int(ts) : 0;   /* legacy: no ts */
        g_pk_n++;
      }
      ci = 0; ki = 0; ti = 0; stage = 0;
    } else if (c == '=' && stage < 2) stage++;
    else if (stage == 0) { if (ci < 15) call[ci++] = c; }
    else if (stage == 1) { if (ki < 47) key[ki++] = c; }
    else { if (ti < 11) ts[ti++] = c; }
  }
}
/* Intercept a NOSTR key beacon (group "NOSTR"): record from->pubkey and report
 * it was handled (so it isn't shown as a chat message). */
static int pk_intercept(const char *group, const char *from, const char *text) {
  if (!s_eq(group, "NOSTR")) return 0;
  pk_store(from, text);
  return 1;
}

/* ── follow list persistence + mutation ─────────────────────────────────── */
/* KV "follows": "CALL=tag1 tag2;CALL;…" — '=' starts the optional tag list
 * (callsigns never contain '='); the legacy "CALL;" form still parses. */
static void follows_save(void) {
  char buf[FOLLOW_MAX * 64]; buf[0] = 0;
  for (int i = 0; i < g_follow_n; i++) {
    s_cat(buf, g_follow[i], sizeof(buf));
    if (g_ftag[i][0]) { s_cat(buf, "=", sizeof(buf)); s_cat(buf, g_ftag[i], sizeof(buf)); }
    s_cat(buf, ";", sizeof(buf));
  }
  hal_kv_set("follows", 7, buf, s_len(buf));
}
static void follows_load(void) {
  char buf[FOLLOW_MAX * 64];
  uint32_t n = hal_kv_get("follows", 7, buf, sizeof(buf) - 1);
  if (n == 0) return;
  buf[n] = 0;
  char c[16], t[48]; int j = 0, ti = 0, stage = 0;
  for (unsigned i = 0; i <= n; i++) {
    char ch = (i < n) ? buf[i] : ';';
    if (ch == ';') {
      c[j] = 0; t[ti] = 0;
      if (c[0] && g_follow_n < FOLLOW_MAX && !is_following(c)) {
        s_cpy(g_follow[g_follow_n], c, 16);
        s_cpy(g_ftag[g_follow_n], t, 48);
        g_follow_n++;
      }
      j = 0; ti = 0; stage = 0;
    } else if (ch == '=' && stage == 0) stage = 1;
    else if (stage == 0) { if (j < 15) c[j++] = ch; }
    else { if (ti < 47) t[ti++] = ch; }
  }
}
static void followers_save(void) {
  char buf[FOLLOW_MAX * 17]; buf[0] = 0;
  for (int i = 0; i < g_follower_n; i++) {
    s_cat(buf, g_follower[i], sizeof(buf)); s_cat(buf, ";", sizeof(buf));
  }
  hal_kv_set("followers", 9, buf, s_len(buf));
}
static void followers_load(void) {
  char buf[FOLLOW_MAX * 17];
  uint32_t n = hal_kv_get("followers", 9, buf, sizeof(buf) - 1);
  if (n == 0) return;
  buf[n] = 0;
  char c[16]; int j = 0;
  for (unsigned i = 0; i <= n; i++) {
    char ch = (i < n) ? buf[i] : ';';
    if (ch == ';') { c[j] = 0; if (c[0] && g_follower_n < FOLLOW_MAX && !is_follower(c)) s_cpy(g_follower[g_follower_n++], c, 16); j = 0; }
    else if (j < 15) c[j++] = ch;
  }
}
static void follow_add(const char *call) {
  char up_call[16]; int j = 0;            /* callsigns are upper-case on the wire */
  for (int i = 0; call[i] && j < 15; i++) up_call[j++] = up(call[i]);
  up_call[j] = 0;
  if (!up_call[0] || s_eq(up_call, g_call) || is_following(up_call)) return;
  if (g_follow_n >= FOLLOW_MAX) { notify("warning", "Following list is full"); return; }
  s_cpy(g_follow[g_follow_n], up_call, 16);
  g_ftag[g_follow_n][0] = 0;
  g_follow_n++;
  follows_save();
  follow_render();
  /* Tell the station (Twitter-style): a directed ?FOLLOW control message on
   * both transports; their wapp records us in its Followers list. */
  if (g_sock >= 0 && g_logged)
    aprs_send_message_multi(g_sock, g_call, up_call, "?FOLLOW", APRS_MAX_MSG_LEN, &g_seq);
  if (g_ble_on) ble_tx_msg(up_call, "?FOLLOW");
  { char b[40] = "Following "; s_cat(b, up_call, sizeof(b)); notify("info", b); }
}
static void follow_remove(const char *call) {
  for (int i = 0; i < g_follow_n; i++) if (s_eq(g_follow[i], call)) {
    char gone[16]; s_cpy(gone, g_follow[i], sizeof(gone));
    for (int k = i; k < g_follow_n - 1; k++) {
      s_cpy(g_follow[k], g_follow[k + 1], 16);
      s_cpy(g_ftag[k], g_ftag[k + 1], 48);
    }
    g_follow_n--;
    follows_save();
    follow_render();
    if (g_sock >= 0 && g_logged)
      aprs_send_message_multi(g_sock, g_call, gone, "?UNFOLLOW", APRS_MAX_MSG_LEN, &g_seq);
    if (g_ble_on) ble_tx_msg(gone, "?UNFOLLOW");
    { char b[40] = "Unfollowed "; s_cat(b, gone, sizeof(b)); notify("info", b); }
    return;
  }
}
/* Set (or clear) the space-separated tags on a followed callsign. */
static void ftag_set(const char *call, const char *tags) {
  for (int i = 0; i < g_follow_n; i++) if (s_eq(g_follow[i], call)) {
    s_cpy(g_ftag[i], tags, sizeof(g_ftag[i]));
    follows_save();
    follow_render();
    return;
  }
}
/* A peer announced they (un)followed us. Update the Followers list; this is
 * control traffic, never shown as a chat message. */
static void follower_add(const char *call) {
  if (!call[0] || s_eq(call, g_call) || is_follower(call)) return;
  if (g_follower_n >= FOLLOW_MAX) return;
  s_cpy(g_follower[g_follower_n++], call, 16);
  followers_save();
  follow_render();
  { char b[48] = ""; s_cat(b, call, sizeof(b));
    s_cat(b, " started following you", sizeof(b)); notify("info", b); }
}
static void follower_remove(const char *call) {
  for (int i = 0; i < g_follower_n; i++) if (s_eq(g_follower[i], call)) {
    for (int k = i; k < g_follower_n - 1; k++) s_cpy(g_follower[k], g_follower[k + 1], 16);
    g_follower_n--;
    followers_save();
    follow_render();
    return;
  }
}
/* Intercept a directed ?FOLLOW / ?UNFOLLOW control message (returns 1). */
static int follow_intercept(const char *from, const char *text) {
  if (s_eq(text, "?FOLLOW"))   { follower_add(from);    return 1; }
  if (s_eq(text, "?UNFOLLOW")) { follower_remove(from); return 1; }
  return 0;
}

/* Activity dedup: the same packet can reach us twice (APRS-IS + a BLE iGate), so
 * collapse identical (sender,line) entries to one feed item. */
#define ACT_SEEN 64
static unsigned g_act_seen[ACT_SEEN];
static unsigned g_act_seen_n = 0;
static int act_seen_has(unsigned h) {
  unsigned n = g_act_seen_n < ACT_SEEN ? g_act_seen_n : ACT_SEEN;
  for (unsigned i = 0; i < n; i++) if (g_act_seen[i] == h) return 1;
  return 0;
}
static void act_seen_add(unsigned h) { g_act_seen[g_act_seen_n % ACT_SEEN] = h; g_act_seen_n++; }

/* Surface one activity item from a followed station into the Activity feed.
 * [grp] is the group context ("" for a direct/status item). Deduped on the
 * sender + rendered line so dual-path delivery shows once. */
static void activity_capture(const char *from, const char *grp,
                             const char *text, const char *via) {
  if (!is_following(from)) return;
  char line[300]; line[0] = 0;
  if (grp && grp[0]) { s_cat(line, "#", sizeof(line)); s_cat(line, grp, sizeof(line)); s_cat(line, ": ", sizeof(line)); }
  s_cat(line, text, sizeof(line));
  unsigned h = sig_hash("act", from, line);
  if (act_seen_has(h)) return;
  act_seen_add(h);
  double lat = 0, lon = 0; pos_get(from, &lat, &lon);
  char meta[24] = ""; if (lat != 0 || lon != 0) distance_to(lat, lon, meta, sizeof(meta));
  chat_append("activity", "", "in", from, line, "msg", 0, meta, lat, lon, via);
}

/* Deliver one conversation message: dedup by signature — first time shows in
 * the flow, a repeat is promoted to a pinned item (and further repeats are
 * ignored as updates of the same pin). [forcePin] is set for our own
 * recurring sends (pinned from the first beat). */
static void convo_deliver(const char *id, const char *dir, const char *from,
                          const char *text, const char *preview, int forcePin,
                          const char *via) {
  /* Threading is group-only: derive this message's id from the wire text and,
   * if it carries a "+<4hex> " reply marker, split off the parent + show the
   * text without the marker. 1:1 chats are untouched. */
  char mid[5] = "", parent[5] = "";
  /* APRX signature: split off a trailing " ~<sig>" and verify it. The core
   * (sig stripped) is what we thread/id/display; the sig never affects mid. */
  char core[700]; char sigstr[80]; char auth[12] = ""; int have_sig = 0;
  const char *body = text;
  if (sig_split(text, core, sizeof(core), sigstr, sizeof(sigstr))) { body = core; have_sig = 1; }

  /* Encrypted 1:1 message ("ENC1:<base64>"): canonicalise (strip spaces that
   * multi-line reassembly inserts into the space-less base64) so the signature
   * matches, then decrypt with the peer's key (sender for incoming, recipient
   * for our own echo). Groups are never encrypted. */
  int enc = 0; char plain[460]; char canon_content[700];
  s_cpy(canon_content, body, sizeof(canon_content));
  const char *disp = body;
  if (id[0] != '#' && s_len(body) > 5 &&
      body[0]=='E'&&body[1]=='N'&&body[2]=='C'&&body[3]=='1'&&body[4]==':') {
    enc = 1;
    char b64[680]; unsigned bi = 0;
    for (const char *p = body + 5; *p; p++) if (*p != ' ' && bi + 1 < sizeof(b64)) b64[bi++] = *p;
    b64[bi] = 0;
    s_cpy(canon_content, "ENC1:", sizeof(canon_content)); s_cat(canon_content, b64, sizeof(canon_content));
    const char *peer = s_eq(dir, "out") ? id : from;
    const char *ppk = pk_get(peer);
    plain[0] = 0;
    if (ppk) {
      uint32_t pn = hal_decrypt(ppk, s_len(ppk), b64, s_len(b64), plain, sizeof(plain) - 1);
      if (pn > 0 && pn < sizeof(plain)) plain[pn] = 0;
      else s_cpy(plain, "[encrypted - cannot decrypt]", sizeof(plain));
    } else {
      s_cpy(plain, "[encrypted - no key]", sizeof(plain));
    }
    disp = plain;
  }

  /* Verify the signature over the canonical (space-normalised) content. */
  if (have_sig) {
    if (s_eq(dir, "out")) {
      s_cpy(auth, "verified", sizeof(auth));     /* we signed it ourselves */
    } else {
      const char *pk = pk_get(from);
      if (!pk) {
        s_cpy(auth, "unverified", sizeof(auth)); /* sender's key not known yet */
      } else {
        char canon[760]; sig_canon(canon, sizeof(canon), from, canon_content);
        int ok = hal_verify(pk, s_len(pk), canon, s_len(canon), sigstr, s_len(sigstr));
        s_cpy(auth, ok ? "verified" : "bad", sizeof(auth));
      }
    }
  }

  if (id[0] == '#') {
    /* A like vote ("<4hex>:like") is not a chat message: register the reaction
     * and stop (no bubble). Works for our own echo (mine) and others' votes. */
    char tgt[5]; int unlike;
    if (like_parse(body, tgt, &unlike)) {
      convo_react(id, tgt, from, unlike, s_eq(from, g_call));
      return;
    }
    msg_id(from, body, mid);
    thread_parse(body, parent, &disp);
  }
  /* Dedup on the signature-stripped (and for encrypted, space-normalised) core,
   * so the SAME message arriving via two transports (APRS-IS + a BLE iGate), or
   * signed vs unsigned forms, collapses to one. */
  unsigned h = sig_hash(id, from, enc ? canon_content : body);
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
  /* A repeated INCOMING direct message is a duplicate (dual-path delivery from
   * APRS-IS + a BLE iGate, or a resend) — drop it. Repeated bulletins are
   * intentional (recurring) → promote to pinned; our own sends are never dropped. */
  if (rep && id[0] != '#' && s_eq(dir, "in")) return;
  if (forcePin || rep) convo_pin(id, key, dir, from, disp, meta, lat, lon, via, mid, parent, auth, enc);
  else convo_msg(id, dir, from, disp, key, meta, lat, lon, via, mid, parent, auth, enc);
  convo_touch(id, enc ? disp : preview, 0);   /* show decrypted text in the list */
}

/* ── decentralized file discovery (FILES group, files DESIGN.md §7) ────────
 * A media token (file:<sha256>.<ext>) only names content. When we send a
 * message carrying one, we also broadcast where the bytes can be fetched —
 * a FILES bulletin "HAVE <token> <blossom-url> ih:<infohash>" — so receivers
 * record the source and pull the file (becoming providers themselves). The
 * group is never shown as chat; it's intercepted like the NOSTR key beacon. */
#define FILES_GROUP "FILES"

/* Find the first file:<43>.<ext> token in [text]; 1 + copy to out, else 0. */
static int find_file_token(const char *text, char *out, unsigned max) {
  for (const char *p = text; *p; p++) {
    if (p[0]=='f'&&p[1]=='i'&&p[2]=='l'&&p[3]=='e'&&p[4]==':') {
      const char *q = p + 5; int n = 0;
      while (((*q>='A'&&*q<='Z')||(*q>='a'&&*q<='z')||(*q>='0'&&*q<='9')||
              *q=='-'||*q=='_') && n < 43) { q++; n++; }
      if (n != 43 || *q != '.') continue;
      const char *r = q + 1; int e = 0;
      while (((*r>='a'&&*r<='z')||(*r>='0'&&*r<='9')) && e < 18) { r++; e++; }
      if (e < 1) continue;
      unsigned len = (unsigned)(r - p);
      if (len >= max) return 0;
      for (unsigned i = 0; i < len; i++) out[i] = p[i];
      out[len] = 0;
      return 1;
    }
  }
  return 0;
}

/* Broadcast a HAVE for every media token in [text] we can serve. */
static void files_announce(const char *text) {
  char token[80];
  if (!find_file_token(text, token, sizeof(token))) return;
  /* Where can peers fetch it? Our Blossom URL and/or BitTorrent infohash. */
  char st[1024]; uint32_t n = hal_share_status(st, sizeof(st) - 1);
  char url[120] = ""; if (n) { st[n] = 0; jstr(st, "url", url, sizeof(url)); }
  char ih[48] = ""; hal_media_infohash(token, s_len(token), ih, sizeof(ih) - 1);
  if (!url[0] && !ih[0]) return;            /* nothing to point peers at */
  char body[220]; body[0] = 0;
  s_cat(body, "HAVE ", sizeof(body)); s_cat(body, token, sizeof(body));
  if (url[0]) { s_cat(body, " ", sizeof(body)); s_cat(body, url, sizeof(body)); }
  if (ih[0]) { s_cat(body, " ih:", sizeof(body)); s_cat(body, ih, sizeof(body)); }
  if (g_sock >= 0 && g_logged)
    aprs_send_bulletin_multi(g_sock, g_call, FILES_GROUP, body, APRS_MAX_MSG_LEN);
  if (g_ble_on) ble_tx_msg("#" FILES_GROUP, body);
}

/* Handle a FILES bulletin (returns 1 = consumed, not chat). On "HAVE <token>
 * <url|ih:..>" record each source and pull the file so we can render + reseed
 * it. On "WANT <token>" answer with our own HAVE when we hold the bytes. */
static int files_intercept(const char *group, const char *from, const char *text) {
  (void)from;
  if (!s_eq(group, FILES_GROUP)) return 0;
  char token[80] = "";
  if (!find_file_token(text, token, sizeof(token))) return 1;
  if (text[0]=='W'&&text[1]=='A'&&text[2]=='N'&&text[3]=='T') {
    char meta[600]; uint32_t mn = hal_media_meta(token, s_len(token), meta, sizeof(meta)-1);
    if (mn > 0) files_announce(token);      /* we have it → re-announce */
    return 1;
  }
  /* HAVE: scan the remaining words for a blossom URL and an ih:<hex>. */
  for (const char *p = text; *p; p++) {
    if (p[0]=='h'&&p[1]=='t'&&p[2]=='t'&&p[3]=='p') {
      char url[120]; int i = 0;
      while (p[i] && p[i] != ' ' && i < 119) { url[i] = p[i]; i++; }
      url[i] = 0;
      hal_media_add_source(token, s_len(token), "blossom", 7, url, s_len(url));
      p += i ? i - 1 : 0;
    } else if (p[0]=='i'&&p[1]=='h'&&p[2]==':') {
      char ih[48]; int i = 0; const char *q = p + 3;
      while (q[i] && q[i] != ' ' && i < 47) { ih[i] = q[i]; i++; }
      ih[i] = 0;
      if (i >= 32) hal_media_add_source(token, s_len(token), "infohash", 8, ih, s_len(ih));
      p += 2 + i;
    }
  }
  hal_media_fetch(token, s_len(token));     /* pull now → render + reseed */
  return 1;
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
  /* Encrypt a 1:1 message to a callsign whose public key we know (ENC1: + a
   * base64url AES blob); group messages are never encrypted. The encrypted body
   * is what gets signed + transmitted, so only the recipient can read it but
   * anyone can still verify who sent it. */
  char core[700]; s_cpy(core, text, sizeof(core));
  int encrypted = 0;
  if (id[0] != '#') {
    const char *rpk = pk_get(id);
    if (rpk) {
      char ct[640];
      uint32_t cn = hal_encrypt(rpk, s_len(rpk), text, s_len(text), ct, sizeof(ct) - 1);
      if (cn > 0 && cn < sizeof(ct)) {
        ct[cn] = 0;
        s_cpy(core, "ENC1:", sizeof(core)); s_cat(core, ct, sizeof(core));
        encrypted = 1;
      }
    }
  }

  /* Sign (APRX) when enabled OR when encrypted (encryption always carries a
   * signature). The signed body is word-split by the multi-line senders so the
   * 60-char signature lands on its own final APRS line; the receiver
   * reassembles and verifies. Likes are left unsigned. */
  char wire[800];
  s_cpy(wire, core, sizeof(wire));
  {
    char tgt[5]; int ul;
    if ((g_sign_msgs || encrypted) && !like_parse(text, tgt, &ul)) {
      char canon[720]; sig_canon(canon, sizeof(canon), g_call, core);
      char sg[80];
      uint32_t sn = hal_identity_sign(canon, s_len(canon), sg, sizeof(sg) - 1);
      if (sn > 0 && sn < sizeof(sg)) {
        sg[sn] = 0;
        s_cat(wire, " ~", sizeof(wire));
        s_cat(wire, sg, sizeof(wire));
      }
    }
  }
  if (id[0] == '#') {
    /* Strip the scope marker: a global group "#NEWS*" transmits the same
     * "NEWS" bulletin as the local "#NEWS" — scope is only a local view. */
    char gname[8]; int gj = 0;
    for (int i = 1; id[i] && id[i] != '*' && gj < 6; i++) gname[gj++] = id[i];
    gname[gj] = 0;
    if (net) aprs_send_bulletin_multi(g_sock, g_call, gname, wire, APRS_MAX_MSG_LEN);
    if (g_ble_on) {
      char bid[10]; bid[0] = '#'; s_cpy(bid + 1, gname, sizeof(bid) - 1);
      ble_tx_msg(bid, wire);            /* compact BLE: to = "#group" (no scope) */
    }
  } else {
    if (net) aprs_send_message_multi(g_sock, g_call, id, wire, APRS_MAX_MSG_LEN, &g_seq);
    if (g_ble_on) ble_tx_msg(id, wire);
  }
  convo_deliver(id, "out", g_call, wire, text, 0, "");
  files_announce(text);   /* if it carries a file: token, advertise the source */
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
  request_history();   /* reload archived Live messages for the new area */
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
  /* Geo-tag our own message with our position so it is archived for this
   * area and reappears in the Live history later. */
  chat_append("geochat", "", "out", g_call, echo, "msg", 0, "", g_lat, g_lon, "");
  files_announce(body);
  status("TX geo-chat");
}

/* Post a micro-update to the shared feed group (FEED): a Twitter-style status
 * that everyone following us sees in their Activity tab. Sent as a bulletin
 * (multi-line, optionally signed) over APRS-IS and BLE, then echoed into our own
 * Activity feed. */
static void do_activity_send(const char *buf) {
  read_config(buf);
  int net = (g_sock >= 0 && g_logged);
  if (!net && !g_ble_on) {
    notify("warning", "Connect to APRS-IS or enable Bluetooth first");
    return;
  }
  char text[400] = "";
  jstr(buf, "activity_input", text, sizeof(text));
  if (!text[0]) return;
  /* Sign when enabled (the receiver reassembles + verifies the trailing line). */
  char wire[480]; s_cpy(wire, text, sizeof(wire));
  if (g_sign_msgs) {
    char canon[480]; sig_canon(canon, sizeof(canon), g_call, text);
    char sg[80];
    uint32_t sn = hal_identity_sign(canon, s_len(canon), sg, sizeof(sg) - 1);
    if (sn > 0 && sn < sizeof(sg)) { sg[sn] = 0; s_cat(wire, " ~", sizeof(wire)); s_cat(wire, sg, sizeof(wire)); }
  }
  if (net) aprs_send_bulletin_multi(g_sock, g_call, FEED_GROUP, wire, APRS_MAX_MSG_LEN);
  if (g_ble_on) ble_tx_msg("#" FEED_GROUP, wire);
  /* Local echo of our own post (plaintext, no #group prefix). */
  chat_append("activity", "", "out", g_call, text, "msg", 0, "", g_lat, g_lon, "");
  files_announce(text);
  status("TX post");
}

/* Prompt to follow a callsign. */
static void prompt_follow(void) {
  const char *m = "{\"type\":\"ui.prompt\",\"id\":\"follow\",\"title\":\"Follow a callsign\","
    "\"body\":\"Enter a callsign to follow. Their posts, replies, likes and status "
    "will appear in your Activity feed.\","
    "\"input\":{\"hint\":\"Callsign e.g. N0CALL\",\"max\":15},\"confirm\":\"Follow\"}";
  hal_msg_send(m, s_len(m));
}
/* Prompt to unfollow: chips of the currently-followed callsigns. */
static void prompt_unfollow(void) {
  if (g_follow_n == 0) { notify("info", "You aren't following anyone yet"); return; }
  char m[900] = "{\"type\":\"ui.prompt\",\"id\":\"unfollow\",\"title\":\"Unfollow\","
                "\"body\":\"Pick a callsign to stop following.\",\"chips\":[";
  for (int i = 0; i < g_follow_n; i++) {
    if (i) s_cat(m, ",", sizeof(m));
    s_cat(m, "{\"label\":\"", sizeof(m)); jesc(m, sizeof(m), g_follow[i]);
    s_cat(m, "\",\"value\":\"", sizeof(m)); jesc(m, sizeof(m), g_follow[i]);
    s_cat(m, "\"}", sizeof(m));
  }
  s_cat(m, "],\"chipMode\":\"select\",\"confirm\":\"Unfollow\"}", sizeof(m));
  hal_msg_send(m, s_len(m));
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
  convo_deliver(convo, "out", g_call, r->text, r->text, 1, "");
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
  "WX", "EMCOM", "ARES", "NET", "DX", "EVENT", "HELP", "SOS",
  /* 4chan-style boards */
  "B", "POL", "FIN", "G"
};
static void prompt_group(void) {
  char chips[1000] = "";
  for (unsigned i = 0; i < sizeof(PRESET_GROUPS) / sizeof(PRESET_GROUPS[0]); i++) {
    if (i) s_cat(chips, ",", sizeof(chips));
    s_cat(chips, "{\"label\":\"#", sizeof(chips));
    s_cat(chips, PRESET_GROUPS[i], sizeof(chips));
    s_cat(chips, "\",\"value\":\"", sizeof(chips));
    s_cat(chips, PRESET_GROUPS[i], sizeof(chips));
    s_cat(chips, "\"}", sizeof(chips));
  }
  char m[1600] = "{\"type\":\"ui.prompt\",\"id\":\"group\",\"title\":\"Add a group\","
                 "\"body\":\"Pick or type a group (max 5 letters). Global follows it "
                 "worldwide; local follows it only within your radius.\",\"chips\":[";
  s_cat(m, chips, sizeof(m));
  s_cat(m, "],\"chipMode\":\"select\",\"input\":{\"hint\":\"Custom\",\"max\":5,"
          "\"prefix\":\"#\"},\"toggle\":{\"label\":\"Global (worldwide)\"},"
          "\"confirm\":\"Add\"}", sizeof(m));
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
    if (g[0]) {
      char id[12]; id[0] = '#'; s_cpy(id + 1, g, sizeof(id) - 1);
      if (jbool(buf, "prompt_toggle")) s_cat(id, "*", sizeof(id));   /* global */
      convo_touch(id, "", 1);
      groups_save();
    }
  } else if (s_eq(pid, "recur")) {
    char id[40] = ""; jstr(buf, "conversations_convo", id, sizeof(id));
    if (id[0] == '#' && inp[0]) recur_begin(id + 1, inp, to_int(val));
  } else if (s_eq(pid, "follow")) {
    if (inp[0]) follow_add(inp);
  } else if (s_eq(pid, "unfollow")) {
    if (val[0]) follow_remove(val);
  } else if (pid[0]=='p'&&pid[1]=='r'&&pid[2]=='o'&&pid[3]=='f'&&pid[4]==':') {
    /* Profile sheet action for pid "prof:<CALL>". */
    const char *call = pid + 5;
    if (s_eq(val, "follow")) follow_add(call);
    else if (s_eq(val, "unfollow")) follow_remove(call);
    else if (s_eq(val, "tags")) prompt_ftag(call);
  } else if (pid[0]=='f'&&pid[1]=='t'&&pid[2]=='a'&&pid[3]=='g'&&pid[4]==':') {
    ftag_set(pid + 5, inp);    /* empty input clears the tags */
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

/* ── Store-and-forward: BLE iGate mailbox for heard stations ──────────────
 * When this station is online (APRS-IS up) it acts as an iGate for nearby
 * BLE-only stations: it remembers the callsigns it hears over BLE in a
 * persistent registry (<=100, 1-year LRU), adds them to the APRS-IS `g/`
 * filter so the server pushes messages addressed to them, and holds those
 * messages in a per-callsign mailbox. A BLE-only station pulls its mail by
 * broadcasting "?MAIL <call>" every 5 min while an iGate (?IGATE beacon) is in
 * reach; we reply with each held message and clear the mailbox. No UI. */
#define MAIL_TO       "?MAIL"
#define IGATE_TO      "?IGATE"
#define SDEV_MAX      100
#define SDEV_TTL      (365ULL * 24 * 3600)   /* 1 year */
#define GFILTER_CAP   30                      /* heard calls put in the g/ filter */

typedef struct { char call[12]; uint64_t ts; } sdev_t;
static sdev_t g_sdev[SDEV_MAX];
static int g_sdev_n = 0;
static int g_sdev_dirty = 0;

static uint64_t g_last_igate_heard  = 0;   /* we (client) heard an iGate beacon */
static uint64_t g_last_igate_beacon = 0;   /* we (iGate) last announced ourselves */
static uint64_t g_last_mail_query   = 0;
static uint64_t g_last_filter_check = 0;
static uint64_t g_sdev_saved        = 0;
static char g_gfilter[600] = "";           /* g/ + b/ extra filter currently in use */

static int sdev_find(const char *c) {
  for (int i = 0; i < g_sdev_n; i++) if (s_eq(g_sdev[i].call, c)) return i;
  return -1;
}
static int sdev_has(const char *c) { return sdev_find(c) >= 0; }
static void mailbox_clear(const char *call);   /* fwd */

/* Remember a callsign heard over BLE (not us): refresh its timestamp, add it,
 * or evict the least-recently-seen when full. */
static void sdev_touch(const char *c) {
  if (!c || !c[0] || s_eq(c, g_call)) return;
  uint64_t now = hal_time_epoch();
  int i = sdev_find(c);
  if (i >= 0) { g_sdev[i].ts = now; g_sdev_dirty = 1; return; }
  if (g_sdev_n < SDEV_MAX) { i = g_sdev_n++; }
  else {
    int lru = 0;
    for (int k = 1; k < g_sdev_n; k++) if (g_sdev[k].ts < g_sdev[lru].ts) lru = k;
    mailbox_clear(g_sdev[lru].call);
    i = lru;
  }
  s_cpy(g_sdev[i].call, c, sizeof(g_sdev[i].call));
  g_sdev[i].ts = now; g_sdev_dirty = 1;
}

static void sdev_save(void) {
  char buf[1800]; buf[0] = 0;
  for (int i = 0; i < g_sdev_n; i++) {
    s_cat(buf, g_sdev[i].call, sizeof(buf)); s_cat(buf, ",", sizeof(buf));
    char tb[20]; u_itoa((unsigned)g_sdev[i].ts, tb); s_cat(buf, tb, sizeof(buf));
    s_cat(buf, ";", sizeof(buf));
  }
  hal_kv_set("seendev", 7, buf, s_len(buf));
}
static void sdev_load(void) {
  char buf[1800];
  uint32_t n = hal_kv_get("seendev", 7, buf, sizeof(buf) - 1);
  if (n == 0) return;
  buf[n] = 0;
  uint64_t now = hal_time_epoch();
  g_sdev_n = 0;
  char *p = buf;
  while (*p && g_sdev_n < SDEV_MAX) {
    char call[12]; int ci = 0;
    while (*p && *p != ',' && ci < 11) call[ci++] = *p++;
    call[ci] = 0;
    if (*p == ',') p++;
    uint64_t ts = 0;
    while (*p >= '0' && *p <= '9') { ts = ts * 10 + (uint64_t)(*p - '0'); p++; }
    if (*p == ';') p++;
    if (call[0] && (now < SDEV_TTL || ts >= now - SDEV_TTL)) {   /* prune >1yr */
      s_cpy(g_sdev[g_sdev_n].call, call, sizeof(g_sdev[g_sdev_n].call));
      g_sdev[g_sdev_n].ts = ts; g_sdev_n++;
    }
  }
}

/* g/ extra filter: our own call, the most-recently-seen stations (so APRS-IS
 * pushes their direct messages), and the bulletin addressee pattern for every
 * GLOBAL group we subscribe to (id ending in '*') so we hear that group
 * worldwide. Local groups (no '*') need nothing extra — the always-on r/ range
 * filter already brings in-radius bulletins. */
static void build_gfilter(char *out, unsigned max) {
  out[0] = 0;
  s_cat(out, "g/", max); s_cat(out, g_call, max);
  int used[SDEV_MAX]; for (int i = 0; i < g_sdev_n; i++) used[i] = 0;
  int cnt = g_sdev_n < GFILTER_CAP ? g_sdev_n : GFILTER_CAP;
  for (int k = 0; k < cnt; k++) {
    int best = -1;
    for (int i = 0; i < g_sdev_n; i++)
      if (!used[i] && (best < 0 || g_sdev[i].ts > g_sdev[best].ts)) best = i;
    if (best < 0) break;
    used[best] = 1;
    s_cat(out, "/", max); s_cat(out, g_sdev[best].call, max);
  }
  /* Any GLOBAL group (#NAME*) → pull bulletins worldwide. APRS-IS g/ only
   * supports a trailing '*' (no mid-string wildcard, verified live), and a
   * bulletin's addressee is "BLN<id><GROUP>", so we can't match a specific
   * group server-side. Bulletin volume is tiny (a few per minute globally), so
   * one catch-all "g/BLN*" is fine; deliver_bulletin() then files only the
   * groups we actually subscribed to. */
  for (int i = 0; i < g_convo_n; i++) {
    const char *id = g_convo_ids[i];
    unsigned L = s_len(id);
    if (id[0] == '#' && L >= 3 && id[L - 1] == '*') { s_cat(out, "/BLN*", max); break; }
  }
  /* Followed stations: a b/ budlist pulls EVERY packet FROM them (posts,
   * replies, likes, status) regardless of group, so their Activity stream
   * arrives even for groups we don't subscribe to. */
  if (g_follow_n) {
    s_cat(out, " b", max);
    for (int i = 0; i < g_follow_n; i++) { s_cat(out, "/", max); s_cat(out, g_follow[i], max); }
  }
}
/* True if any global group (#NAME*) is subscribed — i.e. g/BLN* is active and
 * worldwide bulletins are arriving, so a local group must verify proximity. */
static int any_global_group(void) {
  for (int i = 0; i < g_convo_n; i++) {
    const char *id = g_convo_ids[i];
    unsigned L = s_len(id);
    if (id[0] == '#' && L >= 3 && id[L - 1] == '*') return 1;
  }
  return 0;
}

/* Subscribed groups persist in KV "groups" (";"-joined ids) so the APRS-IS
 * filter is correct immediately after a restart, before any row is reopened. */
static void groups_save(void) {
  char buf[600]; buf[0] = 0;
  for (int i = 0; i < g_convo_n; i++)
    if (g_convo_ids[i][0] == '#') { s_cat(buf, g_convo_ids[i], sizeof(buf)); s_cat(buf, ";", sizeof(buf)); }
  hal_kv_set("groups", 6, buf, s_len(buf));
}
static void groups_load(void) {
  char buf[600];
  uint32_t n = hal_kv_get("groups", 6, buf, sizeof(buf) - 1);
  if (n == 0) return;
  buf[n] = 0;
  char id[40]; int j = 0;
  for (unsigned i = 0; i <= n; i++) {
    char ch = (i < n) ? buf[i] : ';';
    if (ch == ';') { id[j] = 0; if (id[0] == '#') convo_remember(id); j = 0; }
    else if (j < 39) id[j++] = ch;
  }
}

/* The public-key beacon on/off state persists in KV "pkbeacon" ("1"/"0"), so
 * the user's choice survives a restart (unlike the per-session BLE toggle). */
static void pkbeacon_save(void) {
  hal_kv_set("pkbeacon", 8, g_pubkey_beacon ? "1" : "0", 1);
}
static void pkbeacon_load(void) {
  char b[4];
  uint32_t n = hal_kv_get("pkbeacon", 8, b, sizeof(b) - 1);
  if (n >= 1) g_pubkey_beacon = (b[0] != '0');   /* absent -> keep default (on) */
}
/* Broadcast our npub once: APRS-IS bulletin to group "NOSTR" + same over BLE.
 * Receivers map the sender callsign (frame from-field) to the npub text. */
static void pkbeacon_send(void) {
  if (!g_pubkey_beacon || !g_pubkey[0]) return;
  if (g_sock >= 0 && g_logged)
    aprs_send_bulletin_multi(g_sock, g_call, PKBEACON_GROUP, g_pubkey, APRS_MAX_MSG_LEN);
  if (g_ble_on)
    ble_tx_msg("#" PKBEACON_GROUP, g_pubkey);
  g_last_pkbeacon = hal_time_epoch();
}

/* ---- per-callsign mailbox (KV "m.<call>", lines "<from>|<text>") ---- */
static void mailbox_key(char *out, unsigned max, const char *call) {
  out[0] = 0; s_cat(out, "m.", max); s_cat(out, call, max);
}
static void mailbox_clear(const char *call) {
  char key[20]; mailbox_key(key, sizeof(key), call);
  hal_kv_delete(key, s_len(key));
}
/* The "<from>|<text>" body of a stored line is everything after the first '|'
 * (which separates the leading timestamp). Returns NULL if malformed. */
static const char *mail_line_body(const char *line) {
  const char *p = line; while (*p && *p != '|') p++;
  return (*p == '|') ? p + 1 : 0;
}
/* Dedup on the body (sender+text), ignoring the per-line timestamp. */
static int contains_body(const char *buf, const char *body) {
  unsigned bl = s_len(body);
  const char *p = buf;
  while (*p) {
    const char *e = p; while (*e && *e != '\n') e++;
    const char *b = mail_line_body(p);
    if (b && b <= e && (unsigned)(e - b) == bl) {
      int eq = 1; for (unsigned i = 0; i < bl; i++) if (b[i] != body[i]) { eq = 0; break; }
      if (eq) return 1;
    }
    p = (*e == '\n') ? e + 1 : e;
  }
  return 0;
}
/* Hold a message addressed to a heard station until it pulls its mail. Each line
 * is "<ts>|<from>|<text>" (ts = epoch when held) so a ?MAIL can window by age. */
static void mailbox_add(const char *call, const char *from, const char *text) {
  if (!call[0] || !from[0]) return;
  char key[20]; mailbox_key(key, sizeof(key), call);
  char buf[1300];
  uint32_t n = hal_kv_get(key, s_len(key), buf, sizeof(buf) - 1);
  buf[n] = 0;
  char body[420]; body[0] = 0;                    /* "<from>|<text>" (newline-free) */
  s_cat(body, from, sizeof(body)); s_cat(body, "|", sizeof(body));
  for (const char *t = text; *t && s_len(body) < sizeof(body) - 2; t++) {
    char c = (*t == '\n' || *t == '\r') ? ' ' : *t;
    char cc[2] = { c, 0 }; s_cat(body, cc, sizeof(body));
  }
  if (n && contains_body(buf, body)) return;      /* dedup ignoring ts */
  char line[440]; line[0] = 0;
  { char tb[12]; u_itoa((unsigned)hal_time_epoch(), tb); s_cat(line, tb, sizeof(line)); }
  s_cat(line, "|", sizeof(line)); s_cat(line, body, sizeof(line));
  char out[1500]; out[0] = 0;
  if (n) { s_cat(out, buf, sizeof(out)); s_cat(out, "\n", sizeof(out)); }
  s_cat(out, line, sizeof(out));
  char *o = out;                                  /* cap: drop oldest lines */
  while (s_len(o) > 1100) {
    char *nl = o; while (*nl && *nl != '\n') nl++;
    if (*nl == '\n') o = nl + 1; else break;
  }
  hal_kv_set(key, s_len(key), o, s_len(o));
}

#define MAIL_QUERY_CAP 30   /* most-recent messages delivered per ?MAIL pull */

/* A heard station broadcast "?MAIL <days> <nonce>": deliver the messages we hold
 * for it that are within the requested day-window (default 7), newest first,
 * capped, each as a 1:1 frame from the original sender. Delivered lines are
 * removed; out-of-window lines are kept for a possible later, wider pull. */
static void handle_mail_query(const char *from, const char *text) {
  if (s_eq(from, g_call)) return;
  sdev_touch(from);
  int days = text ? to_int(text) : 0;             /* leading integer = look-back days */
  if (days <= 0) days = 7;
  uint64_t now = hal_time_epoch();
  uint64_t cutoff = (now > (uint64_t)days * 86400) ? now - (uint64_t)days * 86400 : 0;

  char key[20]; mailbox_key(key, sizeof(key), from);
  char buf[1400];
  uint32_t n = hal_kv_get(key, s_len(key), buf, sizeof(buf) - 1);
  if (n == 0) return;
  buf[n] = 0;

  /* First pass: split into NUL-terminated lines; collect in-window pointers
   * (oldest..newest as stored), accumulate out-of-window lines into `keep`. */
  const char *lines[64]; int nl = 0;
  char keep[1500]; keep[0] = 0;                   /* out-of-window lines, kept */
  char *p = buf;
  while (*p && nl < 64) {
    char *e = p; while (*e && *e != '\n') e++;
    int had_nl = (*e == '\n');
    *e = 0;                                        /* terminate this line */
    uint64_t ts = (uint64_t)(unsigned)to_int(p);  /* leading ts */
    if (ts >= cutoff) { lines[nl++] = p; }
    else { if (keep[0]) s_cat(keep, "\n", sizeof(keep)); s_cat(keep, p, sizeof(keep)); }
    p = had_nl ? e + 1 : e;
  }
  /* Deliver the newest MAIL_QUERY_CAP in-window (they sit at the tail). */
  int start = (nl > MAIL_QUERY_CAP) ? nl - MAIL_QUERY_CAP : 0;
  for (int i = start; i < nl; i++) {
    const char *body = mail_line_body(lines[i]);
    if (!body) continue;
    char mfrom[16] = ""; const char *bar = body; int bi = 0;
    while (*bar && *bar != '|' && bi < 15) mfrom[bi++] = *bar++;
    mfrom[bi] = 0;
    const char *mtext = (*bar == '|') ? bar + 1 : "";
    if (mfrom[0]) ble_tx_from(mfrom, from, mtext);
  }
  /* Keep out-of-window lines plus any in-window ones we didn't deliver (cap). */
  for (int i = 0; i < start; i++) {
    if (keep[0]) s_cat(keep, "\n", sizeof(keep));
    s_cat(keep, lines[i], sizeof(keep));
  }
  if (keep[0]) hal_kv_set(key, s_len(key), keep, s_len(keep));
  else mailbox_clear(from);
}

/* Route one APRS-IS TNC2 line to the UI; bridge to BLE when relaying. */
/* File a received group bulletin into the right conversation(s):
 *   - global (#NAME*) when subscribed — always (it arrived because we asked for
 *     the group worldwide, or it's in range);
 *   - local (#NAME) when subscribed AND the sender is within radius, OR when
 *     only the local view is subscribed;
 *   - if neither is subscribed, surface it once under the local id (discovery).
 * [within] = sender known to be inside our radius; [via] = "NET"/"BLE". */
static void deliver_bulletin(const char *gname, const char *from,
                             const char *text, int within, const char *via) {
  char nm[8]; int nj = 0;                 /* clean group name (no '#'/'*') */
  for (int i = 0; gname[i] && gname[i] != '*' && nj < 6; i++) nm[nj++] = gname[i];
  nm[nj] = 0;
  if (!nm[0]) return;
  /* NOSTR key beacon: record the sender's pubkey and stop (not a chat). */
  if (pk_intercept(nm, from, text)) return;
  if (files_intercept(nm, from, text)) return;   /* file source announcements */
  /* Strip any APRX signature for the preview / like detection; convo_deliver
   * still gets the full text and re-verifies the signature. */
  char core[400]; char sg[80]; const char *cbody = text;
  if (sig_split(text, core, sizeof(core), sg, sizeof(sg))) cbody = core;
  /* A like vote is silent (no notification): convo_deliver registers it. */
  int is_like; char ltgt[5]; { int u; is_like = like_parse(cbody, ltgt, &u); }
  char par[5]; const char *disp_body; thread_parse(cbody, par, &disp_body);
  /* Followed station → surface this in the Activity feed, BEFORE the group
   * subscription gate (we want their stream even for groups we don't follow).
   * The feed group (FEED) is the plain micro-post stream; other groups are
   * shown with their #group context; likes/replies are summarised. */
  if (is_following(from)) {
    if (is_like) activity_capture(from, "", "liked a post", via);
    else if (par[0]) { char t[300]; s_cpy(t, "replied: ", sizeof(t)); s_cat(t, disp_body, sizeof(t)); activity_capture(from, s_eq(nm, FEED_GROUP) ? "" : nm, t, via); }
    else activity_capture(from, s_eq(nm, FEED_GROUP) ? "" : nm, disp_body, via);
  }
  char lid[14]; lid[0] = '#'; s_cpy(lid + 1, nm, sizeof(lid) - 1);
  char gid[16]; s_cpy(gid, lid, sizeof(gid)); s_cat(gid, "*", sizeof(gid));
  int has_g = convo_known(gid), has_l = convo_known(lid);
  if (!has_g && !has_l) return;            /* only listen to groups we subscribed */
  char preview[140] = ""; s_cpy(preview, from, sizeof(preview)); s_cat(preview, ": ", sizeof(preview));
  s_cat(preview, disp_body, sizeof(preview));
  if (has_g) {                              /* global: every bulletin for the group */
    convo_deliver(gid, "in", from, text, preview, 0, via);
    if (!is_like) notify_msg(gid, from, cbody, preview);
  }
  /* Local: a nearby sender, OR — when no global pull is active (g/BLN* off) —
   * trust the region filter that the bulletin is in-range. */
  if (has_l && (within || !any_global_group())) {
    convo_deliver(lid, "in", from, text, preview, 0, via);
    if (!is_like) notify_msg(lid, from, cbody, preview);
  }
}

/* A standalone APRX signature line: "~" + exactly 60 base85 chars. The signed
 * body's word-split puts the signature on its own final line, so this marks the
 * end of a multi-line signed message. */
static int is_sig_line(const char *t) {
  if (t[0] != '~' || s_len(t) != SIG_B85_LEN + 1) return 0;
  for (int i = 1; i <= SIG_B85_LEN; i++) if (!is_b85(t[i])) return 0;
  return 1;
}

/* ── Multi-line bulletin reassembly (APRS-IS) ─────────────────────────────
 * aprs_send_bulletin_multi splits a long body (incl. a signed message, whose
 * 60-char signature is its own final line) across BLN0..BLNk; rejoin them.
 * Buffer lines per (from,group), keyed by line id, flush after a brief idle,
 * joining the contiguous run from line 0 with single spaces (matching the
 * splitter). Single-line bulletins flush the same way. BLE arrives whole. */
#define RA_MAX 4
#define RA_FLUSH 2            /* seconds idle before flushing */
typedef struct {
  int used; char from[16]; char grp[8]; char line[10][72];
  int seen; uint64_t t; int within; char via[4];
} ra_t;
static ra_t g_ra[RA_MAX];
static void ra_emit(ra_t *e) {
  char full[720]; full[0] = 0;
  for (int i = 0; i < 10; i++) {
    if (!(e->seen & (1 << i))) break;
    if (full[0]) s_cat(full, " ", sizeof(full));
    s_cat(full, e->line[i], sizeof(full));
  }
  e->used = 0; e->seen = 0;
  if (full[0]) deliver_bulletin(e->grp, e->from, full, e->within, e->via);
}
static void ra_add(const char *grp, const char *from, char line_id,
                   const char *text, int within, const char *via) {
  int idx = (int)(line_id - '0'); if (idx < 0 || idx > 9) idx = 0;
  ra_t *e = 0;
  for (int i = 0; i < RA_MAX; i++)
    if (g_ra[i].used && s_eq(g_ra[i].from, from) && s_eq(g_ra[i].grp, grp)) { e = &g_ra[i]; break; }
  if (!e) {
    for (int i = 0; i < RA_MAX; i++) if (!g_ra[i].used) { e = &g_ra[i]; break; }
    if (!e) { e = &g_ra[0]; for (int i = 1; i < RA_MAX; i++) if (g_ra[i].t < e->t) e = &g_ra[i]; ra_emit(e); }
    e->used = 1; e->seen = 0; e->within = 0;
    s_cpy(e->from, from, sizeof(e->from)); s_cpy(e->grp, grp, sizeof(e->grp));
    s_cpy(e->via, via, sizeof(e->via));
  }
  s_cpy(e->line[idx], text, sizeof(e->line[idx]));
  e->seen |= (1 << idx); e->t = hal_time_epoch();
  if (within) e->within = 1;
}
/* Flush every cycle (after the read loop): a single-line bulletin is delivered
 * the same cycle it arrived (no delay); a multi-line bulletin's lines arrive in
 * the same cycle and reassemble by line id. */
static void ra_flush(void) {
  for (int i = 0; i < RA_MAX; i++)
    if (g_ra[i].used) ra_emit(&g_ra[i]);
}

/* ── Multi-line direct-message reassembly (APRS-IS) ───────────────────────
 * aprs_send_message_multi splits a long DM into parts with consecutive seq;
 * a signed DM's last part is a pure signature line. Buffer parts per sender,
 * flush after a brief idle: each run from a body up to a signature line is one
 * signed message (rejoined); any trailing run with no signature line is
 * delivered as separate plain messages (no spurious merging of normal chat). */
#define DA_MAX 6
#define DA_PARTS 16        /* an encrypted message can span ~10 APRS lines */
typedef struct { int used; char from[16]; char via[4]; char part[DA_PARTS][72]; int n; uint64_t t; } da_t;
static da_t g_da[DA_MAX];
static void da_emit_one(const char *from, const char *full, const char *via) {
  char prev[256], sg[80]; const char *pv = full;
  if (sig_split(full, prev, sizeof(prev), sg, sizeof(sg))) pv = prev;
  convo_deliver(from, "in", from, full, pv, 0, via);
}
/* Buffer one direct-message part, keyed by (from, transport). A message that
 * arrives over BOTH transports (directly from APRS-IS AND re-broadcast by a BLE
 * iGate) is reassembled per-transport and dedups in convo_deliver — shown once. */
static void da_add(const char *from, const char *text, const char *via) {
  da_t *e = 0;
  for (int i = 0; i < DA_MAX; i++)
    if (g_da[i].used && s_eq(g_da[i].from, from) && s_eq(g_da[i].via, via)) { e = &g_da[i]; break; }
  if (!e) {
    for (int i = 0; i < DA_MAX; i++) if (!g_da[i].used) { e = &g_da[i]; break; }
    if (!e) { e = &g_da[0]; e->used = 0; e->n = 0; }   /* spill: drop oldest slot (rare) */
    e->used = 1; e->n = 0;
    s_cpy(e->from, from, sizeof(e->from)); s_cpy(e->via, via, sizeof(e->via));
  }
  if (e->n < DA_PARTS) s_cpy(e->part[e->n++], text, sizeof(e->part[0]));
  e->t = hal_time_epoch();
}
/* Decide whether a buffered entry is ready to deliver. A complete single plain
 * message (one short, non-ENC, non-signature part) flushes immediately — no
 * delay. A multi-part message (signed/encrypted, whose parts may arrive across
 * poll cycles via APRS-IS) is held until its trailing signature line arrives;
 * an idle safety net flushes anything stuck after ~2s. */
static int da_ready(da_t *d, uint64_t now) {
  if (d->n == 0) return 1;
  const char *last = d->part[d->n - 1];
  if (is_sig_line(last)) return 1;                 /* complete signed/encrypted */
  int enc_head = (s_len(d->part[0]) > 5 && d->part[0][0]=='E' && d->part[0][1]=='N'
                  && d->part[0][2]=='C' && d->part[0][3]=='1' && d->part[0][4]==':');
  if (d->n == 1 && !enc_head && s_len(last) < 66) return 1; /* plain short single */
  return now - d->t >= 2;                           /* idle safety net */
}
static void da_flush(void) {
  uint64_t now = hal_time_epoch();
  for (int x = 0; x < DA_MAX; x++) {
    if (!g_da[x].used || !da_ready(&g_da[x], now)) continue;
    da_t *d = &g_da[x]; int i = 0;
    while (i < d->n) {
      int j = i; while (j < d->n && !is_sig_line(d->part[j])) j++;
      if (j < d->n) {                       /* parts i..j-1 = body, j = signature */
        if (j == i) { i = j + 1; continue; }  /* lone signature fragment → drop */
        char full[1200]; full[0] = 0;
        for (int k = i; k <= j; k++) { if (full[0]) s_cat(full, " ", sizeof(full)); s_cat(full, d->part[k], sizeof(full)); }
        da_emit_one(d->from, full, d->via); i = j + 1;
      } else {                              /* no signature → deliver separately */
        for (int k = i; k < d->n; k++) da_emit_one(d->from, d->part[k], d->via);
        i = d->n;
      }
    }
    d->used = 0; d->n = 0;
  }
}

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
        chat_append("geochat", "", "in", p.from, p.comment, "pos", 0, meta, p.lat, p.lon, "NET");
      /* Followed station's status/geo-chat comment → Activity feed. A ">>"
       * geo-chat message is shown as a plain post; anything else as a status. */
      if (is_following(p.from)) {
        const char *c = p.comment;
        if (c[0] == '>' && c[1] == '>') { c += 2; while (*c == ' ') c++; if (c[0]) activity_capture(p.from, "", c, "NET"); }
        else { char t[300]; s_cpy(t, "status: ", sizeof(t)); s_cat(t, c, sizeof(t)); activity_capture(p.from, "", t, "NET"); }
      }
    }
  } else if (p.type == APRS_MESSAGE) {
    if (p.text[0] && !is_ack_text(p.text)) {
      if (p.is_bulletin) {
        /* Buffer the line; multi-line bulletins are reassembled before delivery. */
        ra_add(p.group, p.from, p.bulletin_id ? p.bulletin_id : '0', p.text,
               within_radius(p.from), "NET");
        if (g_ble_relay && g_ble_on) {
          char convo[12]; convo[0] = '#'; s_cpy(convo + 1, p.group, sizeof(convo) - 1);
          ble_tx_from(p.from, convo, p.text);
        }
      } else {
        int amine = 1;
        for (int i = 0; g_call[i] || p.addressee[i]; i++) {
          if (up(g_call[i]) != up(p.addressee[i])) { amine = 0; break; }
        }
        /* ?FOLLOW / ?UNFOLLOW notifications are control traffic — record the
         * follower and keep them off the Live tab / chat / notifications. */
        if (amine && follow_intercept(p.from, p.text)) return;
        /* A bare signature line is a continuation fragment, not a message:
         * keep it off the Live tab + notifications; da_ reassembles it. */
        int sigln = is_sig_line(p.text);
        char meta[24] = ""; double slat = 0, slon = 0;
        if (pos_get(p.from, &slat, &slon)) distance_to(slat, slon, meta, sizeof(meta));
        if (!sigln && !geo_dup(p.from, p.text))
          chat_append("geochat", "", "in", p.from, p.text, "msg", 0, meta, slat, slon, "NET");
        /* Buffer DM parts; multi-line (incl. signed) messages reassemble in da_. */
        if (amine) da_add(p.from, p.text, "NET");
        else if (sdev_has(p.addressee))   /* store-and-forward for a heard station */
          mailbox_add(p.addressee, p.from, p.text);
        /* Direct message to us, or a message on the Live tab — pop a notice. */
        if (!sigln) notify_msg(p.from, p.from, p.text, p.text);
        /* Bridge to BLE only for messages NOT addressed to us — we are the
         * endpoint of our own mail, so re-broadcasting it would just echo back
         * as a duplicate. Relay (general bridge) or store-and-forward to a heard
         * station are the only reasons to put a message on BLE. */
        if (g_ble_on && !amine && (g_ble_relay || sdev_has(p.addressee)))
          ble_tx_from(p.from, p.addressee, p.text);
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
/* ── Ping reach-test helpers ──────────────────────────────────────────── */

/* Per-id / per-(responder,id) dedup so each station answers + forwards a
 * given ping once, and forwards each pong once. */
#define PSEEN_MAX 96
static unsigned g_pseen[PSEEN_MAX];
static unsigned g_pseen_cnt = 0;
static int pseen_has(unsigned h) {
  unsigned n = g_pseen_cnt < PSEEN_MAX ? g_pseen_cnt : PSEEN_MAX;
  for (unsigned i = 0; i < n; i++) if (g_pseen[i] == h) return 1;
  return 0;
}
static void pseen_add(unsigned h) { g_pseen[g_pseen_cnt % PSEEN_MAX] = h; g_pseen_cnt++; }

/* Deferred digipeat queue: instead of rebroadcasting a received frame
 * immediately, hold it a short, per-frame-staggered delay (a few ticks) and
 * re-advertise when due. The stagger (derived from the frame hash so peers
 * pick different delays) cuts collisions and widens effective reach. */
#define RQ_MAX 16
static struct { char frame[300]; uint64_t due; int used; } g_rq[RQ_MAX];
static void rq_push(const char *frame, uint64_t due) {
  for (int i = 0; i < RQ_MAX; i++)
    if (!g_rq[i].used) { s_cpy(g_rq[i].frame, frame, sizeof(g_rq[i].frame)); g_rq[i].due = due; g_rq[i].used = 1; return; }
  /* queue full: drop (storm protection) */
}
static void rq_flush(uint64_t now) {
  for (int i = 0; i < RQ_MAX; i++)
    if (g_rq[i].used && now >= g_rq[i].due) { ble_send(g_rq[i].frame); g_rq[i].used = 0; }
}

/* Best position: live device GPS (hal_sensor_gps_*) if the host provides it,
 * else the configured station position. Returns 1 when a position is known. */
static int my_position(double *lat, double *lon) {
  int32_t la = hal_sensor_gps_lat();
  int32_t lo = hal_sensor_gps_lon();
  if (la != GPS_NA && lo != GPS_NA) {
    *lat = (double)la / 1e7; *lon = (double)lo / 1e7; return 1;
  }
  if (g_lat != 0.0 || g_lon != 0.0) { *lat = g_lat; *lon = g_lon; return 1; }
  *lat = 0; *lon = 0; return 0;
}

/* Append one line to a $type:"log" field. */
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

/* Compact "time since" into out (e.g. "12s", "5m", "3h", "2d", "-" if unknown). */
static void rel_time(uint64_t ts, char *out, unsigned sz) {
  if (ts == 0) { s_cpy(out, "-", sz); return; }
  uint64_t now = hal_time_epoch();
  uint64_t d = now > ts ? now - ts : 0;
  unsigned v; char unit;
  if (d < 60) { v = (unsigned)d; unit = 's'; }
  else if (d < 3600) { v = (unsigned)(d / 60); unit = 'm'; }
  else if (d < 86400) { v = (unsigned)(d / 3600); unit = 'h'; }
  else { v = (unsigned)(d / 86400); unit = 'd'; }
  char nb[12]; u_itoa(v, nb);
  out[0] = 0; s_cat(out, nb, sz); { char u[2] = { unit, 0 }; s_cat(out, u, sz); }
}

/* Rebuild the Keys list view from the callsign->pubkey database. Per station two
 * lines: "<callsign>  (<age>)" then the full npub. The host encodes the stored
 * base64url key to npub; the raw key stays in KV for encryption. */
static void pk_render(void) {
  log_clear("keys_list");
  if (g_pk_n == 0) { log_line("keys_list", "No public keys received yet."); return; }
  for (int i = 0; i < g_pk_n; i++) {
    char hdr[40]; hdr[0] = 0;
    s_cat(hdr, g_pk_call[i], sizeof(hdr));
    char age[12]; rel_time(g_pk_ts[i], age, sizeof(age));
    s_cat(hdr, "  (", sizeof(hdr)); s_cat(hdr, age, sizeof(hdr)); s_cat(hdr, ")", sizeof(hdr));
    log_line("keys_list", hdr);
    char npub[72];
    uint32_t nn = hal_npub(g_pk_key[i], s_len(g_pk_key[i]), npub, sizeof(npub) - 1);
    if (nn > 0 && nn < sizeof(npub)) { npub[nn] = 0; log_line("keys_list", npub); }
    else log_line("keys_list", g_pk_key[i]);   /* fallback: raw base64url key */
  }
}

/* One row of the Follows people list. [following] selects the trailing
 * button: "Following" (outlined, unfollows) vs "Follow back" (filled). */
static void people_item(char *m, unsigned sz, const char *call, int following) {
  s_cat(m, "{\"id\":\"", sz); jesc(m, sz, call);
  s_cat(m, "\",\"title\":\"", sz); jesc(m, sz, call);
  /* subtitle: known pubkey -> npub prefix + how long since their key beacon */
  char sub[64] = "";
  for (int k = 0; k < g_pk_n; k++) if (s_eq(g_pk_call[k], call)) {
    char npub[72];
    uint32_t nn = hal_npub(g_pk_key[k], s_len(g_pk_key[k]), npub, sizeof(npub) - 1);
    if (nn > 14) { npub[14] = 0; s_cat(sub, npub, sizeof(sub)); s_cat(sub, "...", sizeof(sub)); }
    char age[12]; rel_time(g_pk_ts[k], age, sizeof(age));
    if (sub[0]) s_cat(sub, " - ", sizeof(sub));
    s_cat(sub, "heard ", sizeof(sub)); s_cat(sub, age, sizeof(sub));
    s_cat(sub, " ago", sizeof(sub));
    break;
  }
  s_cat(m, "\",\"subtitle\":\"", sz); jesc(m, sz, sub);
  s_cat(m, "\",\"tags\":[", sz);
  for (int k = 0; k < g_follow_n; k++) if (s_eq(g_follow[k], call)) {
    const char *t = g_ftag[k]; int first = 1; char one[48]; int oi = 0;
    for (int x = 0;; x++) {
      char ch = t[x];
      if (ch == ' ' || ch == 0) {
        if (oi) {
          one[oi] = 0;
          if (!first) s_cat(m, ",", sz);
          s_cat(m, "\"", sz); jesc(m, sz, one); s_cat(m, "\"", sz);
          first = 0; oi = 0;
        }
        if (!ch) break;
      } else if (oi < 47) one[oi++] = ch;
    }
    break;
  }
  s_cat(m, "],", sz);
  if (following)
    s_cat(m, "\"action\":\"row_unfollow\",\"actionLabel\":\"Following\","
             "\"actionStyle\":\"outlined\"}", sz);
  else
    s_cat(m, "\"action\":\"row_follow\",\"actionLabel\":\"Follow back\","
             "\"actionStyle\":\"filled\"}", sz);
}

/* Push the Follows people list (Following | Followers sections) to the host's
 * social-style list view. */
static char g_people[8192];
static void follow_render(void) {
  char *m = g_people; const unsigned sz = sizeof(g_people);
  m[0] = 0;
  s_cat(m, "{\"type\":\"ui.people.set\",\"field\":\"follows_list\",\"sections\":[", sz);
  s_cat(m, "{\"title\":\"Following\",\"items\":[", sz);
  for (int i = 0; i < g_follow_n; i++) {
    if (i) s_cat(m, ",", sz);
    people_item(m, sz, g_follow[i], 1);
  }
  s_cat(m, "]},{\"title\":\"Followers\",\"items\":[", sz);
  for (int i = 0; i < g_follower_n; i++) {
    if (i) s_cat(m, ",", sz);
    people_item(m, sz, g_follower[i], is_following(g_follower[i]));
  }
  s_cat(m, "]}]}", sz);
  hal_msg_send(m, s_len(m));
}

/* Station profile sheet: identity facts + instant Follow/Unfollow/tags
 * actions. Rendered by the host's generic prompt (chips act on tap). */
static void profile_show(const char *call) {
  char up_call[16]; int j = 0;
  for (int i = 0; call[i] && j < 15; i++) up_call[j++] = up(call[i]);
  up_call[j] = 0;
  if (!up_call[0] || s_eq(up_call, g_call)) return;
  int fol = is_following(up_call);
  int fan = is_follower(up_call);
  char body[420] = "";
  if (fol && fan) s_cat(body, "You follow each other.", sizeof(body));
  else if (fol)   s_cat(body, "You are following.", sizeof(body));
  else if (fan)   s_cat(body, "Follows you.", sizeof(body));
  else            s_cat(body, "Not following.", sizeof(body));
  for (int i = 0; i < g_follow_n; i++)
    if (s_eq(g_follow[i], up_call) && g_ftag[i][0]) {
      s_cat(body, "\nTags: ", sizeof(body));
      s_cat(body, g_ftag[i], sizeof(body));
      break;
    }
  const char *pk = pk_get(up_call);
  if (pk) {
    char npub[72];
    uint32_t nn = hal_npub(pk, s_len(pk), npub, sizeof(npub) - 1);
    if (nn > 0 && nn < sizeof(npub)) {
      npub[nn] = 0;
      s_cat(body, "\nKey: ", sizeof(body)); s_cat(body, npub, sizeof(body));
    }
    for (int i = 0; i < g_pk_n; i++) if (s_eq(g_pk_call[i], up_call)) {
      char age[12]; rel_time(g_pk_ts[i], age, sizeof(age));
      s_cat(body, "\nKey heard ", sizeof(body)); s_cat(body, age, sizeof(body));
      s_cat(body, " ago", sizeof(body));
      break;
    }
  } else {
    s_cat(body, "\nNo public key received yet.", sizeof(body));
  }
  { double la, lo; char d[24];
    if (pos_get(up_call, &la, &lo) && distance_to(la, lo, d, sizeof(d))) {
      s_cat(body, "\nDistance: ", sizeof(body)); s_cat(body, d, sizeof(body));
    } }
  char m[1000] = "{\"type\":\"ui.prompt\",\"id\":\"prof:";
  jesc(m, sizeof(m), up_call);
  s_cat(m, "\",\"title\":\"", sizeof(m)); jesc(m, sizeof(m), up_call);
  s_cat(m, "\",\"body\":\"", sizeof(m)); jesc(m, sizeof(m), body);
  s_cat(m, "\",\"chips\":[", sizeof(m));
  if (fol)
    s_cat(m, "{\"label\":\"Unfollow\",\"value\":\"unfollow\"},"
             "{\"label\":\"Edit tags\",\"value\":\"tags\"}", sizeof(m));
  else
    s_cat(m, "{\"label\":\"Follow\",\"value\":\"follow\"}", sizeof(m));
  s_cat(m, "]}", sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Edit the tags on a followed callsign (result handled as "ftag:<call>"). */
static void prompt_ftag(const char *call) {
  char m[420] = "{\"type\":\"ui.prompt\",\"id\":\"ftag:";
  jesc(m, sizeof(m), call);
  s_cat(m, "\",\"title\":\"Tags for ", sizeof(m)); jesc(m, sizeof(m), call);
  s_cat(m, "\",\"body\":\"Space-separated tags, e.g. dx friend club. "
           "Leave empty to clear.\","
           "\"input\":{\"hint\":\"tags\",\"max\":40},\"confirm\":\"Save\"}",
        sizeof(m));
  hal_msg_send(m, s_len(m));
}

/* Extract the idx-th comma-separated field of s into out (NUL-terminated). */
static void csv_field(const char *s, int idx, char *out, unsigned osz) {
  out[0] = 0;
  int f = 0;
  const char *start = s;
  for (const char *p = s;; p++) {
    if (*p == ',' || *p == 0) {
      if (f == idx) {
        unsigned n = (unsigned)(p - start);
        if (n >= osz) n = osz - 1;
        for (unsigned i = 0; i < n; i++) out[i] = start[i];
        out[n] = 0;
        return;
      }
      if (*p == 0) return;
      f++; start = p + 1;
    }
  }
}

/* RSSI -> rough distance (metres) via log-distance path loss:
 *   d = 10^((TXREF - rssi)/(10*N)),  TXREF ~ RSSI at 1 m, N ~ path-loss exp.
 * Coarse, but close enough for a direct hop. -1 when rssi is unknown. */
static int est_dist_m(int rssi) {
  if (rssi >= 0) return -1;
  double d = __builtin_pow(10.0, (double)(-59 - rssi) / 25.0);
  if (d < 1.0) d = 1.0;
  if (d > 5000.0) d = 5000.0;
  return (int)(d + 0.5);
}

/* Straight-line distance from our position to lat/lon in metres, or -1 when
 * our own position is unknown. (Metres twin of distance_to.) */
static int dist_m_to(double lat, double lon) {
  if (g_lat == 0 && g_lon == 0) return -1;
  const double D2R = 0.0174532925199433;
  double x = (lon - g_lon) * D2R * m_cos((g_lat + lat) * 0.5 * D2R);
  double y = (lat - g_lat) * D2R;
  double km = 6371.0 * __builtin_sqrt(x * x + y * y);
  return (int)(km * 1000.0 + 0.5);
}

/* Format metres as "<n> m" (<1 km) or "<n> km". */
static void fmt_dist_m(int m, char *out, unsigned osz) {
  if (m < 1000) { u_itoa((unsigned)m, out); s_cat(out, " m", osz); }
  else { u_itoa((unsigned)((m + 500) / 1000), out); s_cat(out, " km", osz); }
}

/* Per-ping responder results, so we can keep the best route per responder and
 * re-render the list as replies arrive. by_pos = distance came from a real
 * position (accurate); else it's an RF (RSSI) estimate. */
#define PRES_MAX 32
static struct { char call[16]; int hops; int dist_m; int by_pos; int used; } g_pres[PRES_MAX];
static int g_pres_n = 0;
static void pres_reset(void) { g_pres_n = 0; for (int i = 0; i < PRES_MAX; i++) g_pres[i].used = 0; }
/* Best route: prefer a position fix; among RF estimates keep the smallest
 * (most-direct) one; track the fewest hops seen. */
static void pres_update(const char *call, int hops, int dist_m, int by_pos) {
  int idx = -1;
  for (int i = 0; i < g_pres_n; i++)
    if (g_pres[i].used && s_eq(g_pres[i].call, call)) { idx = i; break; }
  if (idx < 0) {
    if (g_pres_n >= PRES_MAX) return;
    idx = g_pres_n++;
    s_cpy(g_pres[idx].call, call, sizeof(g_pres[idx].call));
    g_pres[idx].used = 1; g_pres[idx].hops = hops;
    g_pres[idx].dist_m = dist_m; g_pres[idx].by_pos = by_pos;
    return;
  }
  if (by_pos && !g_pres[idx].by_pos) { g_pres[idx].by_pos = 1; g_pres[idx].dist_m = dist_m; }
  else if (by_pos == g_pres[idx].by_pos && dist_m >= 0 &&
           (g_pres[idx].dist_m < 0 || dist_m < g_pres[idx].dist_m)) {
    g_pres[idx].dist_m = dist_m;
  }
  if (hops >= 0 && hops < g_pres[idx].hops) g_pres[idx].hops = hops;
}
static void pres_render(void) {
  const char *c = "{\"type\":\"ui.log.clear\",\"field\":\"pingresults\"}";
  hal_msg_send(c, s_len(c));
  for (int i = 0; i < g_pres_n; i++) {
    if (!g_pres[i].used) continue;
    char line[128] = ""; s_cat(line, g_pres[i].call, sizeof(line)); s_cat(line, "  ", sizeof(line));
    { char t[8]; u_itoa((unsigned)(g_pres[i].hops < 0 ? 0 : g_pres[i].hops), t); s_cat(line, t, sizeof(line)); }
    s_cat(line, (g_pres[i].hops == 1) ? " hop" : " hops", sizeof(line));
    if (g_pres[i].dist_m >= 0) {
      char d[24]; fmt_dist_m(g_pres[i].dist_m, d, sizeof(d));
      s_cat(line, "  -  ", sizeof(line));
      if (!g_pres[i].by_pos) s_cat(line, "~", sizeof(line));   /* RF estimate */
      s_cat(line, d, sizeof(line));
      if (!g_pres[i].by_pos) s_cat(line, " (RF)", sizeof(line));
    }
    log_line("pingresults", line);
  }
}

/* Inbound ping: answer once with our callsign + position, then forward it on
 * (ttl) so it reaches further stations. text = "id,ttl,hops". */
static void handle_ping(const char *from, const char *text) {
  char ids[16], ttls[8], hopss[8];
  csv_field(text, 0, ids, sizeof(ids));
  csv_field(text, 1, ttls, sizeof(ttls));
  csv_field(text, 2, hopss, sizeof(hopss));
  if (!ids[0]) return;
  unsigned key = sig_hash("P", "", ids);
  if (pseen_has(key)) return;
  pseen_add(key);
  int ttl = to_int(ttls), hops = to_int(hopss);
  if (hops < 0) hops = 0;

  /* reply "id,hops,lat,lon,pttl" (lat/lon empty when unknown) */
  double la, lo; int have = my_position(&la, &lo);
  char body[96] = ""; s_cat(body, ids, sizeof(body)); s_cat(body, ",", sizeof(body));
  { char t[8]; u_itoa((unsigned)hops, t); s_cat(body, t, sizeof(body)); }
  s_cat(body, ",", sizeof(body));
  if (have) append_dbl(body, sizeof(body), la);
  s_cat(body, ",", sizeof(body));
  if (have) append_dbl(body, sizeof(body), lo);
  s_cat(body, ",", sizeof(body));
  { char t[8]; u_itoa((unsigned)PING_DEFAULT_TTL, t); s_cat(body, t, sizeof(body)); }
  s_cat(body, ",0", sizeof(body));   /* dM: cumulative RF distance starts at 0 */
  ble_tx_from(g_call, PONG_TO, body);

  if (ttl > 1) {     /* digipeat the ping further */
    char fwd[40] = ""; s_cat(fwd, ids, sizeof(fwd)); s_cat(fwd, ",", sizeof(fwd));
    { char t[8]; u_itoa((unsigned)(ttl - 1), t); s_cat(fwd, t, sizeof(fwd)); }
    s_cat(fwd, ",", sizeof(fwd));
    { char t[8]; u_itoa((unsigned)(hops + 1), t); s_cat(fwd, t, sizeof(fwd)); }
    ble_tx_from(from, PING_TO, fwd);   /* keep the original pinger as 'from' */
  }
}

/* Inbound pong: if it answers our active ping, record it (best route) + drop a
 * map marker; forward it back across the mesh, accumulating an RF distance.
 * text = "id,hops,lat,lon,pttl,dM". [rssi] = strength we received it at.
 *
 * Distance estimate per responder:
 *  - if the reply carries a position AND we know ours -> exact (by_pos);
 *  - else RF: dM (sum of prior hops' RSSI distances) + this hop's RSSI distance.
 * For multi-hop, several routes may arrive; we keep the smallest (best). */
static void handle_pong(const char *from, const char *text, int rssi) {
  char ids[16], hopss[8], las[24], los[24], pttls[8], dms[12];
  csv_field(text, 0, ids, sizeof(ids));
  csv_field(text, 1, hopss, sizeof(hopss));
  csv_field(text, 2, las, sizeof(las));
  csv_field(text, 3, los, sizeof(los));
  csv_field(text, 4, pttls, sizeof(pttls));
  csv_field(text, 5, dms, sizeof(dms));
  if (!ids[0]) return;

  int hops = to_int(hopss);
  double lat = to_dbl(las), lon = to_dbl(los);
  int has_pos = (las[0] != 0);
  int dM = to_int(dms);                  /* RF metres accumulated so far */
  int hop_m = est_dist_m(rssi);          /* this hop's RF distance (-1 unknown) */

  /* Record for our active ping — for EVERY arriving copy, so best-route wins. */
  char gids[16]; u_itoa(g_ping_id, gids);
  if (g_ping_active && s_eq(gids, ids)) {
    int by_pos = 0, dist = -1;
    if (has_pos) {
      int dm = dist_m_to(lat, lon);      /* needs our own position */
      if (dm >= 0) { dist = dm; by_pos = 1; }
    }
    if (!by_pos && hop_m >= 0) dist = dM + hop_m;   /* RF total along this route */
    pres_update(from, hops, dist, by_pos);
    if (has_pos) push_marker(from, lat, lon, "green", "ping reply");
    pres_render();
  }

  /* Forward the reply once (per responder+id), adding this hop's RF distance so
   * the running total reflects the path back to the pinger. Skip if we're the
   * pinger (we're the destination). */
  unsigned key = sig_hash("Q", from, ids);
  if (pseen_has(key)) return;
  pseen_add(key);
  int pttl = to_int(pttls);
  if (pttl > 1 && !(g_ping_active && s_eq(gids, ids))) {
    int dM2 = dM + (hop_m >= 0 ? hop_m : 0);
    char fwd[110] = ""; s_cat(fwd, ids, sizeof(fwd)); s_cat(fwd, ",", sizeof(fwd));
    { char t[8]; u_itoa((unsigned)hops, t); s_cat(fwd, t, sizeof(fwd)); }
    s_cat(fwd, ",", sizeof(fwd)); s_cat(fwd, las, sizeof(fwd));
    s_cat(fwd, ",", sizeof(fwd)); s_cat(fwd, los, sizeof(fwd));
    s_cat(fwd, ",", sizeof(fwd));
    { char t[8]; u_itoa((unsigned)(pttl - 1), t); s_cat(fwd, t, sizeof(fwd)); }
    s_cat(fwd, ",", sizeof(fwd));
    { char t[12]; u_itoa((unsigned)dM2, t); s_cat(fwd, t, sizeof(fwd)); }
    ble_tx_from(from, PONG_TO, fwd);   /* keep the responder as 'from' */
  }
}

/* Tools "Send ping": broadcast a fresh ping and start collecting replies. */
static void do_ping(const char *buf) {
  read_config(buf);
  if (!g_ble_on) { notify("warning", "Enable Bluetooth exchange first (Settings)"); return; }
  int ttl = PING_DEFAULT_TTL;
  { char v[8]; if (jstr(buf, "ping_ttl", v, sizeof(v)) && v[0]) ttl = to_int(v); }
  if (ttl < 1) ttl = 1; if (ttl > 8) ttl = 8;

  g_ping_seq++;
  g_ping_id = (unsigned)hal_time_epoch() ^ (g_ping_seq * 2654435761u);
  g_ping_active = 1;
  g_ping_start = hal_time_epoch();
  pres_reset();

  { const char *c = "{\"type\":\"ui.log.clear\",\"field\":\"pingresults\"}";
    hal_msg_send(c, s_len(c)); }

  char ids[16]; u_itoa(g_ping_id, ids);
  pseen_add(sig_hash("P", "", ids));   /* never answer our own ping */

  char body[40] = ""; s_cat(body, ids, sizeof(body)); s_cat(body, ",", sizeof(body));
  { char t[8]; u_itoa((unsigned)ttl, t); s_cat(body, t, sizeof(body)); }
  s_cat(body, ",0", sizeof(body));
  ble_tx_from(g_call, PING_TO, body);

  log_line("pingresults", "Ping sent - waiting for replies...");
  status("TX ping");
}

static void ble_handle(const char *compact, int rssi) {
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

  /* Remember every BLE-local station we hear (store-and-forward registry). */
  sdev_touch(from);

  /* Control frames are handled on EVERY receipt — BEFORE the content-dedup below
   * — because they carry no unique body: a repeated ?IGATE beacon must keep
   * refreshing g_last_igate_heard (else the client stops pulling mail after the
   * window), and each ?MAIL must be answered. (Our own re-scanned control frames
   * are dropped by the `mine` check above.)
   *  ?IGATE = an online iGate announcing itself -> note it's in reach.
   *  ?MAIL  = a station pulling its held mail (text = "<days> <nonce>"). */
  if (s_eq(to, IGATE_TO)) { g_last_igate_heard = hal_time_epoch(); return; }
  if (s_eq(to, MAIL_TO))  { handle_mail_query(from, text); return; }

  /* Ping reach-test frames (Tools tab): handled here and NEVER digipeated
   * verbatim, relayed to APRS-IS, or shown on the Live feed — they have their
   * own ttl-based forwarding. */
  if (s_eq(to, PING_TO)) { handle_ping(from, text); return; }
  if (s_eq(to, PONG_TO)) { handle_pong(from, text, rssi); return; }

  /* Content frames: dedup so the digipeater and the chat handle each only once. */
  unsigned h = sig_hash("b", "", compact);
  if (fseen_has(h)) return;
  fseen_add(h);

  /* Digipeater: rebroadcast this frame once, after a short staggered delay
   * (see rq_*), ignoring content already repeated in the last 10 minutes. */
  {
    uint64_t now = hal_time_epoch();
    if (!rpt_recent(h, now)) { rpt_mark(h, now); rq_push(compact, now + 1 + (h % 3)); }
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
        chat_append("geochat", "", "in", from, comment, "pos", 0, meta, lat, lon, "BLE");
      if (is_following(from)) {
        const char *c = comment;
        if (c[0] == '>' && c[1] == '>') { c += 2; while (*c == ' ') c++; if (c[0]) activity_capture(from, "", c, "BLE"); }
        else { char t[300]; s_cpy(t, "status: ", sizeof(t)); s_cat(t, c, sizeof(t)); activity_capture(from, "", t, "BLE"); }
      }
    }
  } else if (to[0] == '#') {              /* group bulletin over BLE (in range = local) */
    deliver_bulletin(to + 1, from, text, 1, "BLE");
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
      chat_append("geochat", "", "in", from, text, "msg", 0, meta, slat, slon, "BLE");
    if (is_following(from)) {
      const char *c = text;
      if (c[0] == '>' && c[1] == '>') { c += 2; while (*c == ' ') c++; }
      if (c[0]) activity_capture(from, "", c, "BLE");
    }
    notify_msg(from, from, text, text);   /* message on the Live tab */
  } else {                               /* 1:1 to a callsign */
    int amine = 1;
    for (int i = 0; g_call[i] || to[i]; i++) {
      if (up(g_call[i]) != up(to[i])) { amine = 0; break; }
    }
    /* Follow notifications are control traffic, not chat (see route_frame). */
    if (amine && follow_intercept(from, text)) return;
    int sigln = is_sig_line(text);
    char dcore[256], dsg[80]; const char *dprev = text;
    if (sig_split(text, dcore, sizeof(dcore), dsg, sizeof(dsg))) dprev = dcore;
    if (amine) {
      /* Buffer through the same reassembler as APRS-IS: a multi-line message
       * forwarded by a BLE iGate as separate parts is rejoined, and a message
       * also received directly over APRS-IS dedups (shown once). */
      da_add(from, text, "BLE");
    } else {
      char meta[24] = ""; double slat = 0, slon = 0;
      if (pos_get(from, &slat, &slon)) distance_to(slat, slon, meta, sizeof(meta));
      if (!geo_dup(from, text))
        chat_append("geochat", "", "in", from, text, "msg", 0, meta, slat, slon, "BLE");
    }
    /* Direct message to us, or a message shown on the Live tab — pop a notice
     * (but not for a bare signature fragment). */
    if (!sigln) notify_msg(from, from, dprev, dprev);
    /* Bridge BLE -> APRS-IS, but never for a message addressed to us (we are the
     * endpoint; re-injecting it would loop back as a duplicate). */
    if (g_ble_relay && g_logged && !amine) {
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
  sdev_load();     /* restore the seen-devices registry (store-and-forward) */
  groups_load();   /* restore subscribed groups so the g/ filter is correct now */
  /* Cache our public key (base64url) and the persisted pubkey-beacon pref. */
  { uint32_t pn = hal_identity_pubkey(g_pubkey, sizeof(g_pubkey) - 1);
    if (pn < sizeof(g_pubkey)) g_pubkey[pn] = 0; else g_pubkey[0] = 0; }
  pkbeacon_load();
  pk_load();       /* restore known callsign -> pubkey map (for verification) */
  pk_render();     /* populate the Keys list view from the restored database */
  follows_load();  /* restore followed callsigns so the b/ filter is correct now */
  followers_load();
  follow_render(); /* push the Follows people list (Following | Followers) */
  { char b[4]; uint32_t n = hal_kv_get("signmsgs", 8, b, sizeof(b) - 1);
    if (n >= 1) g_sign_msgs = (b[0] != '0'); }
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

  /* Flush any digipeat rebroadcasts whose staggered delay is now due. */
  rq_flush(hal_time_epoch());

  /* Close the ping collection window. */
  if (g_ping_active && hal_time_epoch() - g_ping_start > 12) {
    g_ping_active = 0;
    log_line("pingresults", "Ping complete.");
  }

  /* ── Store-and-forward housekeeping (automatic, no UI) ── */
  {
    uint64_t now = hal_time_epoch();
    int online = (g_sock >= 0 && g_logged);   /* we are an APRS-IS iGate */

    /* iGate beacon: announce ourselves so BLE-local stations know to pull mail. */
    if (online && g_ble_on && now - g_last_igate_beacon >= 120) {
      ble_tx_from(g_call, IGATE_TO, "");
      g_last_igate_beacon = now;
    }
    /* Client: while an iGate is in reach, pull our mail every 5 minutes. */
    if (g_ble_on && now - g_last_igate_heard < 600 && now - g_last_mail_query >= 300) {
      /* text = "<days> <nonce>": the look-back window the iGate should honour,
       * plus a nonce so each query is distinct on the wire. */
      char mq[24]; u_itoa((unsigned)g_mail_days, mq); s_cat(mq, " ", sizeof(mq));
      { char nb[12]; u_itoa((unsigned)now, nb); s_cat(mq, nb, sizeof(mq)); }
      ble_tx_from(g_call, MAIL_TO, mq);
      g_last_mail_query = now;
    }
    /* Persist the seen registry (debounced). */
    if (g_sdev_dirty && now - g_sdev_saved >= 60) {
      sdev_save(); g_sdev_saved = now; g_sdev_dirty = 0;
    }
    /* Re-evaluate the APRS-IS g/ filter; reconnect to apply it if it changed. */
    if (online && now - g_last_filter_check >= 30) {
      g_last_filter_check = now;
      char nf[600]; build_gfilter(nf, sizeof(nf));
      if (!s_eq(nf, g_gfilter)) {
        aprs_disconnect(g_sock); g_sock = -1; g_logged = 0;   /* re-login w/ new filter */
      }
    }
  }

  if (g_ble_on) {
    char rec[400];
    for (int guard = 0; guard < 20; guard++) {
      if (ble_poll(rec, sizeof(rec)) <= 0) break;
      char frame[300]; jstr(rec, "data", frame, sizeof(frame));
      int rssi = 0; { char rv[12]; if (jstr(rec, "rssi", rv, sizeof(rv))) rssi = to_int(rv); }
      if (frame[0]) ble_handle(frame, rssi);
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
      /* Include the heard stations in the server-side g/ filter so APRS-IS
       * pushes messages addressed to them (store-and-forward iGate). */
      build_gfilter(g_gfilter, sizeof(g_gfilter));
      aprs_login_ex(g_sock, g_call, pass, g_lat, g_lon, g_radius, g_gfilter);
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
  ra_flush();   /* deliver multi-line bulletins once their parts have arrived */
  da_flush();   /* deliver multi-line direct messages (reassemble signed ones) */

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

  /* Public-key beacon: broadcast our pubkey on whatever transport is up. */
  if (g_pubkey_beacon && g_pubkey[0] && (g_logged || g_ble_on)) {
    uint64_t now = hal_time_epoch();
    if (now - g_last_pkbeacon >= (uint64_t)PKBEACON_INTERVAL) pkbeacon_send();
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
  else if (s_eq(cmd, "ping")) do_ping(buf);
  else if (s_eq(cmd, "geochat_send")) do_geochat_send(buf);
  else if (s_eq(cmd, "activity_send")) do_activity_send(buf);
  else if (s_eq(cmd, "follow")) prompt_follow();
  else if (s_eq(cmd, "unfollow")) prompt_unfollow();
  else if (s_eq(cmd, "profile")) {            /* sender name tapped in a chat */
    char c[16] = ""; jstr(buf, "profile_call", c, sizeof(c));
    profile_show(c);
  } else if (s_eq(cmd, "follows_list_tap")) { /* people-list row tapped */
    char c[16] = ""; jstr(buf, "follows_list_id", c, sizeof(c));
    profile_show(c);
  } else if (s_eq(cmd, "row_follow")) {       /* trailing button on a row */
    char c[16] = ""; jstr(buf, "follows_list_id", c, sizeof(c));
    follow_add(c);
  } else if (s_eq(cmd, "row_unfollow")) {
    char c[16] = ""; jstr(buf, "follows_list_id", c, sizeof(c));
    follow_remove(c);
  }
  else if (s_eq(cmd, "ble_apply")) {
    read_config(buf);
    g_ble_on = jbool_def(buf, "ble_enabled", 1);
    g_ble_relay = jbool_def(buf, "ble_relay", 0);
    ble_reconcile();
  }
  else if (s_eq(cmd, "pubkey_apply")) {
    /* Explicit apply (like ble_apply) so the on-by-default state isn't clobbered
     * by an unset checkbox serialised as false on unrelated commands. */
    g_pubkey_beacon = jbool_def(buf, "pubkey_beacon", 1);
    pkbeacon_save();
    if (g_pubkey_beacon) {
      if (!g_pubkey[0]) { notify("warning", "No profile public key to broadcast"); }
      else { g_last_pkbeacon = 0; pkbeacon_send();    /* send one now */
             status("Public-key broadcast ON");
             notify("success", "Broadcasting your public key"); }
    } else {
      status("Public-key broadcast OFF");
      notify("info", "Public-key broadcast disabled");
    }
  }
  else if (s_eq(cmd, "keys_refresh")) pk_render();
  else if (s_eq(cmd, "sign_apply")) {
    g_sign_msgs = jbool_def(buf, "sign_msgs", 0);
    hal_kv_set("signmsgs", 8, g_sign_msgs ? "1" : "0", 1);
    if (g_sign_msgs && !g_pubkey[0])
      notify("warning", "No profile key — messages can't be signed");
    else {
      status(g_sign_msgs ? "Message signing ON" : "Message signing OFF");
      notify("info", g_sign_msgs ? "Signing outgoing messages"
                                 : "Message signing disabled");
    }
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
