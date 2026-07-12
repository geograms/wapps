/*
 * messages — one inbox.
 *
 * Merges the messaging that used to live twice: in the Chat wapp (callsign-keyed,
 * its own ECDH+AES envelope, riding APRS/BLE/Reticulum) and in the Social wapp
 * (npub-keyed NOSTR kind-4). Here there is exactly one kind of message:
 *
 *     a NOSTR encrypted note (kind-4, NIP-04) addressed to a person's KEY.
 *
 * A conversation is keyed by the peer's 32-byte x-only pubkey (hex). A callsign
 * is not an address — it is a nickname that RESOLVES to a key. That inversion is
 * the whole point of the merge: identity first, transport second.
 *
 * ONE screen — the conversation list. No tabs: a messenger's job is the thread
 * you are in, and a second tab listing the same people you can already see is
 * noise. (Status lives in the overflow menu, not as a tab.)
 *
 *   Messages  ($type:"conversations")  per-peer encrypted threads
 *   Status    ($type:"log", menu)      where messages went, and what came back
 *
 * TWO LANES, ONE MESSAGE
 * ----------------------
 * The same message is published on both:
 *
 *   1. hal_nostr_dm_send   → the configured relay set (wss:// internet,
 *                            rns:// Reticulum, and the local store).
 *   2. hal_relay_dm_send   → Reticulum relay NODES, discovered dynamically via a
 *                            rendezvous set derived from the recipient's key
 *                            (hal_relay_for) — no configuration, works with no
 *                            internet, and stores-and-forwards while the peer is
 *                            offline.
 *
 * They are complementary, not redundant: lane 1 needs a relay you configured,
 * lane 2 finds one on the mesh. Either can be down.
 *
 * The catch: each lane signs its OWN kind-4 event, so the two copies have
 * DIFFERENT event ids and the receiver would show the message twice. So every
 * message carries a dedup id INSIDE the encrypted plaintext:
 *
 *     \x01 <8 hex> \x02 <text>
 *
 * The id is minted once per message and travels with it down both lanes, so the
 * receiver folds the copies into one bubble. It lives inside the ciphertext, so
 * a relay cannot read it or correlate the two copies. (Chat learned this the
 * hard way and carries the same trick under the name `rmid`.)
 *
 * The seen-ring is PERSISTENT: a store-and-forward copy that lands minutes or a
 * reboot later must not re-notify. Same reason Chat persists its `midseen`.
 *
 * All crypto is host-side — the nsec never enters this sandbox.
 *
 * Build: cd wapps/messages && WASI_SDK_PATH=~/wasi-sdk make
 */
#include "../hal/geogram_wasm_hal.h"

/* ── String helpers (no libc: -nostartfiles) ─────────────────────────── */
static unsigned str_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int str_eq(const char *a, const char *b) { while (*a && *b && *a == *b) { a++; b++; } return *a == *b; }
static void str_copy(char *d, const char *s, unsigned m) { unsigned i = 0; while (i < m - 1 && s[i]) { d[i] = s[i]; i++; } d[i] = '\0'; }
static void str_cat(char *d, const char *s, unsigned m) { unsigned l = str_len(d); unsigned i = 0; while (l + i < m - 1 && s[i]) { d[l + i] = s[i]; i++; } d[l + i] = '\0'; }
static char lower1(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static void str_lower(char *s) { for (unsigned i = 0; s[i]; i++) s[i] = lower1(s[i]); }
static void str_upper(char *s) { for (unsigned i = 0; s[i]; i++) if (s[i] >= 'a' && s[i] <= 'z') s[i] = (char)(s[i] - 32); }

/* Find "key":<value> in flat JSON; copy the raw value. Escape-aware inside
 * strings so a message containing a quote is not truncated. */
static int json_raw(const char *json, const char *key, char *out, unsigned m) {
    char pat[48];
    str_copy(pat, "\"", sizeof(pat));
    str_cat(pat, key, sizeof(pat));
    str_cat(pat, "\":", sizeof(pat));
    unsigned pl = str_len(pat);
    for (const char *p = json; *p; p++) {
        unsigned i = 0;
        while (i < pl && p[i] == pat[i]) i++;
        if (i != pl) continue;
        p += pl;
        unsigned o = 0;
        int instr = 0;
        if (*p == '"') { instr = 1; p++; }
        while (*p && o < m - 1) {
            if (instr) {
                if (*p == '\\' && p[1]) { out[o++] = *p++; if (o < m - 1) out[o++] = *p++; continue; }
                if (*p == '"') break;
            } else if (*p == ',' || *p == '}' || *p == ']') break;
            out[o++] = *p++;
        }
        out[o] = '\0';
        return 1;
    }
    return 0;
}

/* Value of the first ["p","<value>"] tag — for our OWN kind-4, that is the
 * recipient, i.e. the peer whose thread the message belongs to. */
static int find_p_tag(const char *evt, char *out, unsigned m) {
    for (const char *p = evt; *p; p++) {
        if (p[0] == '[' && p[1] == '"' && p[2] == 'p' && p[3] == '"' && p[4] == ',') {
            const char *q = p + 5;
            while (*q == ' ') q++;
            if (*q != '"') continue;
            q++;
            unsigned o = 0;
            while (*q && *q != '"' && o < m - 1) out[o++] = *q++;
            out[o] = '\0';
            return o > 0;
        }
    }
    return 0;
}

static void json_escape_cat(char *dst, const char *s, unsigned m) {
    unsigned l = str_len(dst);
    for (unsigned i = 0; s[i] && l < m - 2; i++) {
        char c = s[i];
        if (c == '"' || c == '\\') { dst[l++] = '\\'; dst[l++] = c; }
        else if (c == '\n') { dst[l++] = '\\'; dst[l++] = 'n'; }
        else if (c == '\r') { dst[l++] = '\\'; dst[l++] = 'r'; }
        else if (c == '\t') { dst[l++] = '\\'; dst[l++] = 't'; }
        else if ((unsigned char)c < 0x20) { continue; }
        else dst[l++] = c;
    }
    dst[l] = '\0';
}

static int hexval(char c); /* defined with the key encodings below */

/* Decode JSON string escapes IN PLACE.
 *
 * json_raw hands back the RAW value, escapes and all, and the host JSON-encodes
 * everything it passes us. Our dedup envelope is built from control bytes
 * (\x01 / \x02), which Dart encodes as the six literal characters "" — so
 * without this the envelope is invisible on the relay lane and its id never
 * matches the copy that came down the NOSTR lane. The two copies then show as
 * two messages. (That is exactly what happened on-device: +4 receives, 0 folded.)
 *
 * It also matters for the plain text: a message containing a quote or a newline
 * would otherwise render with a stray backslash. */
static void json_unescape(char *s) {
    unsigned r = 0, w = 0;
    while (s[r]) {
        if (s[r] != '\\') { s[w++] = s[r++]; continue; }
        r++;
        switch (s[r]) {
            case 'n':  s[w++] = '\n'; r++; break;
            case 'r':  s[w++] = '\r'; r++; break;
            case 't':  s[w++] = '\t'; r++; break;
            case 'b':  s[w++] = '\b'; r++; break;
            case 'f':  s[w++] = '\f'; r++; break;
            case '"':  s[w++] = '"';  r++; break;
            case '\\': s[w++] = '\\'; r++; break;
            case '/':  s[w++] = '/';  r++; break;
            case 'u': {
                int h[4], ok = 1;
                for (int i = 0; i < 4; i++) {
                    h[i] = hexval(s[r + 1 + i]);
                    if (h[i] < 0) { ok = 0; break; }
                }
                if (!ok) { s[w++] = '\\'; break; } /* not an escape we grok */
                unsigned cp = (unsigned)((h[0] << 12) | (h[1] << 8) | (h[2] << 4) | h[3]);
                r += 5;
                /* UTF-8 encode. Surrogate pairs are left as-is (the host does not
                 * emit them for our fields, and a lone surrogate is not worth a
                 * mangled message). */
                if (cp < 0x80) {
                    s[w++] = (char)cp;
                } else if (cp < 0x800) {
                    s[w++] = (char)(0xC0 | (cp >> 6));
                    s[w++] = (char)(0x80 | (cp & 0x3F));
                } else {
                    s[w++] = (char)(0xE0 | (cp >> 12));
                    s[w++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                    s[w++] = (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: s[w++] = '\\'; break; /* keep an unknown escape verbatim */
        }
    }
    s[w] = '\0';
}

static void send_msg(const char *json) { hal_msg_send(json, str_len(json)); }

static void u64_str(unsigned long long v, char *out) {
    char tmp[24]; int n = 0;
    if (v == 0) { out[0] = '0'; out[1] = '\0'; return; }
    while (v > 0 && n < 23) { tmp[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    int i = 0; while (n > 0) out[i++] = tmp[--n]; out[i] = '\0';
}

static void fmt_hhmm(const char *unix_s, char *out) {
    long v = 0;
    for (const char *p = unix_s; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
    int hh = (int)((v / 3600) % 24), mm = (int)((v / 60) % 60);
    out[0] = (char)('0' + hh / 10); out[1] = (char)('0' + hh % 10); out[2] = ':';
    out[3] = (char)('0' + mm / 10); out[4] = (char)('0' + mm % 10); out[5] = '\0';
}

static void cat_time_fields(char *dst, const char *ts, unsigned m) {
    str_cat(dst, "\"time\":\"", m);
    if (ts[0]) { char hm[8]; fmt_hhmm(ts, hm); str_cat(dst, hm, m); }
    str_cat(dst, "\",\"t\":", m);
    str_cat(dst, ts[0] ? ts : "0", m);
    str_cat(dst, "000", m); /* seconds → ms */
}

/* ── Key encodings ───────────────────────────────────────────────────────
 * Three encodings of the SAME 32 bytes are in play, and mixing them up fails
 * silently (a wrong-format key just never delivers), so they are converted in
 * one place and everything downstream is keyed on hex:
 *
 *   hex     (64 chars)  — NOSTR / hal_nostr_*      ← our canonical form
 *   b64url  (43 chars)  — hal_relay_dm_* / resolve
 *   npub1…  (bech32)    — what a human pastes
 */
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int is_hex64(const char *s) {
    unsigned i = 0;
    for (; s[i]; i++) { if (i >= 64 || hexval(s[i]) < 0) return 0; }
    return i == 64;
}
/* 32 raw bytes → 64 lowercase hex */
static void bytes_hex(const unsigned char *b, unsigned n, char *out) {
    static const char *H = "0123456789abcdef";
    unsigned i = 0;
    for (; i < n; i++) { out[i * 2] = H[b[i] >> 4]; out[i * 2 + 1] = H[b[i] & 15]; }
    out[n * 2] = '\0';
}
/* 64 hex → 32 raw bytes. 1 on success. */
static int hex_bytes(const char *hex, unsigned char *out, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        int hi = hexval(hex[i * 2]), lo = hexval(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return 1;
}
static const char B64U[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static int b64u_val(char c) {
    for (int i = 0; i < 64; i++) if (B64U[i] == c) return i;
    return -1;
}
/* 32 bytes → 43-char unpadded base64url (what hal_relay_dm_* speaks). */
static void hex_to_b64url(const char *hex, char *out, unsigned m) {
    unsigned char b[32];
    out[0] = '\0';
    if (!hex_bytes(hex, b, 32) || m < 44) return;
    unsigned o = 0;
    for (unsigned i = 0; i < 32; i += 3) {
        unsigned rem = 32 - i;
        unsigned v = (unsigned)b[i] << 16;
        if (rem > 1) v |= (unsigned)b[i + 1] << 8;
        if (rem > 2) v |= (unsigned)b[i + 2];
        out[o++] = B64U[(v >> 18) & 63];
        out[o++] = B64U[(v >> 12) & 63];
        if (rem > 1) out[o++] = B64U[(v >> 6) & 63];
        if (rem > 2) out[o++] = B64U[v & 63];
    }
    out[o] = '\0'; /* 43 chars, unpadded */
}
/* 43-char base64url → 64 hex. 1 on success. */
static int b64url_to_hex(const char *s, char *out, unsigned m) {
    unsigned char b[32];
    unsigned n = str_len(s), bi = 0;
    out[0] = '\0';
    if (n != 43 || m < 65) return 0;
    for (unsigned i = 0; i < n; i += 4) {
        int c0 = b64u_val(s[i]);
        int c1 = (i + 1 < n) ? b64u_val(s[i + 1]) : -1;
        int c2 = (i + 2 < n) ? b64u_val(s[i + 2]) : -1;
        int c3 = (i + 3 < n) ? b64u_val(s[i + 3]) : -1;
        if (c0 < 0 || c1 < 0) return 0;
        unsigned v = ((unsigned)c0 << 18) | ((unsigned)c1 << 12);
        if (c2 >= 0) v |= (unsigned)c2 << 6;
        if (c3 >= 0) v |= (unsigned)c3;
        if (bi < 32) b[bi++] = (unsigned char)((v >> 16) & 0xFF);
        if (c2 >= 0 && bi < 32) b[bi++] = (unsigned char)((v >> 8) & 0xFF);
        if (c3 >= 0 && bi < 32) b[bi++] = (unsigned char)(v & 0xFF);
    }
    if (bi != 32) return 0;
    bytes_hex(b, 32, out);
    return 1;
}
/* bech32 npub1… → 64 hex. The host does not expose a decoder to wasm, and a
 * pasted npub is the normal way a human names a person, so we decode it here.
 * Checksum is NOT verified: a corrupt npub simply yields a key nobody holds, and
 * the message is encrypted TO that key — it leaks nothing and reaches nobody. */
static const char *BECH32 = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
static int bech32_val(char c) {
    c = lower1(c);
    for (int i = 0; i < 32; i++) if (BECH32[i] == c) return i;
    return -1;
}
static int npub_to_hex(const char *npub, char *out, unsigned m) {
    out[0] = '\0';
    if (m < 65) return 0;
    if (!(npub[0] == 'n' && npub[1] == 'p' && npub[2] == 'u' && npub[3] == 'b' &&
          npub[4] == '1')) return 0;
    const char *data = npub + 5;
    unsigned n = str_len(data);
    if (n < 6 + 52) return 0;
    n -= 6; /* drop the 6-char checksum */
    /* 5-bit groups → 8-bit bytes */
    unsigned char b[40];
    unsigned bi = 0, acc = 0, bits = 0;
    for (unsigned i = 0; i < n; i++) {
        int v = bech32_val(data[i]);
        if (v < 0) return 0;
        acc = (acc << 5) | (unsigned)v;
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            if (bi < sizeof(b)) b[bi++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    if (bi < 32) return 0;
    bytes_hex(b, 32, out);
    return 1;
}
/* Normalise anything a human or a HAL can hand us into 64-char hex. */
static int key_to_hex(const char *in, char *out, unsigned m) {
    char t[128];
    str_copy(t, in, sizeof(t));
    if (t[0] == 'n' && t[1] == 'p') return npub_to_hex(t, out, m);
    if (str_len(t) == 43) return b64url_to_hex(t, out, m);
    str_lower(t);
    if (is_hex64(t)) { str_copy(out, t, m); return 1; }
    return 0;
}

/* ── State ───────────────────────────────────────────────────────────── */
#define SEEN_MAX 192          /* dedup ring (rmid / event id)             */
#define PEER_MAX 64           /* known peers                              */
#define RELAY_POLL_SEC 60     /* store-and-forward pull cadence           */
#define NAME_REFRESH_SEC 60   /* kind-0 name lookups: cosmetic, so throttle */

static char g_self[80]      = "";   /* our x-only pubkey (hex)            */
static char g_sub_dm[64]    = "";   /* kind-4 subscription id             */
static char g_open[80]      = "";   /* conversation currently on screen   */

static char g_seen[SEEN_MAX][20];   /* dedup keys (rmid, else event id)   */
static int  g_seen_n = 0;
static int  g_seen_w = 0;           /* ring write cursor                  */

static char g_peer[PEER_MAX][68];   /* peer pubkey (hex)                  */
static char g_pname[PEER_MAX][40];  /* display name (kind-0), if known    */
static char g_pcall[PEER_MAX][16];  /* callsign alias, if known           */
static int  g_peer_unread[PEER_MAX];
static int  g_peer_n = 0;

static unsigned long long g_last_fetch = 0;
static unsigned long long g_last_names = 0;
static int g_sent = 0, g_recv = 0, g_dupes = 0;

static char g_evt[8192];
static char g_plain[4096];
static char g_msg[6144];
static char g_relays[1024];

/* ── Dedup ring (persistent) ─────────────────────────────────────────────
 * A store-and-forward copy can arrive minutes — or a reboot — after the direct
 * one. An in-memory ring would re-notify on every restart, so it is persisted. */
static int seen_has(const char *k) {
    if (!k[0]) return 0;
    for (int i = 0; i < g_seen_n; i++) if (str_eq(g_seen[i], k)) return 1;
    return 0;
}
static void seen_save(void) {
    char buf[SEEN_MAX * 20];
    buf[0] = '\0';
    for (int i = 0; i < g_seen_n; i++) {
        str_cat(buf, g_seen[i], sizeof(buf));
        str_cat(buf, ";", sizeof(buf));
    }
    hal_kv_set("seen", 4, buf, str_len(buf));
}
static void seen_add(const char *k) {
    if (!k[0] || seen_has(k)) return;
    str_copy(g_seen[g_seen_w], k, sizeof(g_seen[0]));
    g_seen_w = (g_seen_w + 1) % SEEN_MAX;
    if (g_seen_n < SEEN_MAX) g_seen_n++;
    seen_save();
}
static void seen_load(void) {
    char buf[SEEN_MAX * 20];
    unsigned n = hal_kv_get("seen", 4, buf, sizeof(buf) - 1);
    if (n == 0 || n >= sizeof(buf)) return;
    buf[n] = '\0';
    char cur[20];
    unsigned c = 0;
    for (unsigned i = 0; buf[i]; i++) {
        if (buf[i] == ';') {
            cur[c] = '\0';
            if (c > 0 && g_seen_n < SEEN_MAX) {
                str_copy(g_seen[g_seen_n], cur, sizeof(cur));
                g_seen_n++;
            }
            c = 0;
        } else if (c < sizeof(cur) - 1) {
            cur[c++] = buf[i];
        }
    }
    g_seen_w = g_seen_n % SEEN_MAX;
}

/* ── Peers ───────────────────────────────────────────────────────────── */
static int peer_idx(const char *hex) {
    for (int i = 0; i < g_peer_n; i++) if (str_eq(g_peer[i], hex)) return i;
    return -1;
}
static void peers_save(void) {
    char buf[PEER_MAX * 90];
    buf[0] = '\0';
    for (int i = 0; i < g_peer_n; i++) {
        str_cat(buf, g_peer[i], sizeof(buf));
        str_cat(buf, "=", sizeof(buf));
        str_cat(buf, g_pcall[i], sizeof(buf));
        str_cat(buf, ";", sizeof(buf));
    }
    hal_kv_set("peers", 5, buf, str_len(buf));
}
static int peer_add(const char *hex) {
    int i = peer_idx(hex);
    if (i >= 0) return i;
    if (g_peer_n >= PEER_MAX) return -1;
    i = g_peer_n++;
    str_copy(g_peer[i], hex, sizeof(g_peer[0]));
    g_pname[i][0] = '\0';
    g_pcall[i][0] = '\0';
    g_peer_unread[i] = 0;
    peers_save();
    return i;
}
static void peers_load(void) {
    char buf[PEER_MAX * 90];
    unsigned n = hal_kv_get("peers", 5, buf, sizeof(buf) - 1);
    if (n == 0 || n >= sizeof(buf)) return;
    buf[n] = '\0';
    char cur[96];
    unsigned c = 0;
    for (unsigned i = 0; buf[i]; i++) {
        if (buf[i] == ';') {
            cur[c] = '\0';
            if (c > 0) {
                char hex[68] = "", call[16] = "";
                unsigned j = 0;
                for (; cur[j] && cur[j] != '=' && j < 67; j++) hex[j] = cur[j];
                hex[j] = '\0';
                if (cur[j] == '=') {
                    unsigned k = 0;
                    for (unsigned q = j + 1; cur[q] && k < 15; q++) call[k++] = cur[q];
                    call[k] = '\0';
                }
                int idx = peer_add(hex);
                if (idx >= 0 && call[0]) str_copy(g_pcall[idx], call, sizeof(g_pcall[0]));
            }
            c = 0;
        } else if (c < sizeof(cur) - 1) {
            cur[c++] = buf[i];
        }
    }
}

/* The best human name we have for a peer: kind-0 name → callsign → key prefix.
 * Never show a bare hex key when we can help it. */
static void peer_title(int i, char *out, unsigned m) {
    if (i >= 0 && g_pname[i][0]) { str_copy(out, g_pname[i], m); return; }
    if (i >= 0 && g_pcall[i][0]) { str_copy(out, g_pcall[i], m); return; }
    str_copy(out, i >= 0 ? g_peer[i] : "", m);
    if (str_len(out) > 12) out[12] = '\0';
}

/* ── Host UI protocol ────────────────────────────────────────────────── */
static void convo_upsert(const char *peer, const char *title, const char *sub, int bump) {
    str_copy(g_msg, "{\"type\":\"ui.convo.upsert\",\"id\":\"", sizeof(g_msg));
    json_escape_cat(g_msg, peer, sizeof(g_msg));
    str_cat(g_msg, "\",\"title\":\"", sizeof(g_msg));
    json_escape_cat(g_msg, title, sizeof(g_msg));
    str_cat(g_msg, "\",\"subtitle\":\"", sizeof(g_msg));
    json_escape_cat(g_msg, sub, sizeof(g_msg));
    str_cat(g_msg, "\",\"icon\":\"person\"", sizeof(g_msg));
    str_cat(g_msg, bump ? ",\"bump\":true}" : "}", sizeof(g_msg));
    send_msg(g_msg);
}

static void convo_msg(const char *peer, const char *dir, const char *from,
                      const char *text, const char *mid, const char *ts) {
    str_copy(g_msg, "{\"type\":\"ui.convo.msg\",\"id\":\"", sizeof(g_msg));
    json_escape_cat(g_msg, peer, sizeof(g_msg));
    str_cat(g_msg, "\",\"dir\":\"", sizeof(g_msg));
    str_cat(g_msg, dir, sizeof(g_msg));
    str_cat(g_msg, "\",\"from\":\"", sizeof(g_msg));
    json_escape_cat(g_msg, from, sizeof(g_msg));
    str_cat(g_msg, "\",\"text\":\"", sizeof(g_msg));
    json_escape_cat(g_msg, text, sizeof(g_msg));
    /* enc=1: every message here is NIP-04 encrypted and BIP-340 signed by the
     * host, so the lock badge is the truth, not decoration. */
    str_cat(g_msg, "\",\"key\":\"\",\"meta\":\"\",\"enc\":1,\"auth\":\"verified\",\"mid\":\"", sizeof(g_msg));
    json_escape_cat(g_msg, mid, sizeof(g_msg));
    str_cat(g_msg, "\",", sizeof(g_msg));
    cat_time_fields(g_msg, ts, sizeof(g_msg));
    str_cat(g_msg, "}", sizeof(g_msg));
    send_msg(g_msg);
}

/* Total unread, tagged with the `messages` intent — this is what lights the
 * launcher's Messages icon. The host's own conversation-store count is not
 * intent-keyed, so without this the header icon stays dark. */
static void unread_emit(void) {
    int total = 0;
    for (int i = 0; i < g_peer_n; i++) total += g_peer_unread[i];
    char n[16];
    u64_str((unsigned long long)total, n);
    str_copy(g_msg, "{\"type\":\"unread\",\"intent\":\"messages\",\"count\":", sizeof(g_msg));
    str_cat(g_msg, n, sizeof(g_msg));
    str_cat(g_msg, "}", sizeof(g_msg));
    send_msg(g_msg);
}

static void notify_new(const char *title, const char *body) {
    str_copy(g_msg, "{\"type\":\"notify\",\"level\":\"info\",\"intent\":\"messages\",\"title\":\"", sizeof(g_msg));
    json_escape_cat(g_msg, title, sizeof(g_msg));
    str_cat(g_msg, "\",\"body\":\"", sizeof(g_msg));
    json_escape_cat(g_msg, body, sizeof(g_msg));
    str_cat(g_msg, "\",\"tag\":\"msg\"}", sizeof(g_msg));
    send_msg(g_msg);
}

static void status_line(const char *s) {
    str_copy(g_msg, "{\"type\":\"ui.log.append\",\"field\":\"status\",\"line\":\"", sizeof(g_msg));
    json_escape_cat(g_msg, s, sizeof(g_msg));
    str_cat(g_msg, "\"}", sizeof(g_msg));
    send_msg(g_msg);
}

/* One-time purge of history written by the pre-0.1.2 builds.
 *
 * Those builds could not see the dedup envelope on the relay lane (the host
 * JSON-escapes it), so they stored bubbles with a literal "…" prefix
 * in front of the text, and stored the same message more than once. That text is
 * already persisted in the host's ConversationStore and no amount of fixing the
 * wapp will rewrite it — the only way it leaves the screen is to drop it.
 *
 * So: on first run of the fixed build, clear the store once and record that we
 * did. This throws away conversation history, which is normally a thing you must
 * never do silently — it is acceptable here ONLY because that history is at most
 * a few hours of garbled dev-build test messages that never left these devices.
 * If this wapp ever ships to real users with real history, this migration must
 * be deleted, not re-run. */
static void purge_pre_envelope_history(void) {
    /* Token "3", not "2": the first attempt recorded that it had migrated, but
     * the headless host silently ignored ui.convo.clear (it only handled it in
     * the foreground page), so nothing was actually cleared. Devices that stored
     * "2" therefore still hold the garbled history and must run it again. */
    char v[8];
    unsigned n = hal_kv_get("fmt", 3, v, sizeof(v) - 1);
    if (n > 0 && n < sizeof(v)) {
        v[n] = '\0';
        if (str_eq(v, "3")) return; /* already migrated */
    }
    send_msg("{\"type\":\"ui.convo.clear\",\"field\":\"conversations\"}");
    hal_kv_set("fmt", 3, "3", 1);
    hal_log(1, "[messages] cleared pre-envelope history (one-time)", 48);
}

/* ── The dedup envelope ──────────────────────────────────────────────────
 * plaintext := \x01 <8 hex> \x02 <text>
 * Minted once per message; the SAME id goes down both lanes so their two
 * distinct kind-4 events fold into one bubble on the far side. It sits inside
 * the ciphertext, so relays cannot use it to correlate the copies. */
#define ENV_SOH 0x01
#define ENV_STX 0x02

static void env_wrap(const char *rmid, const char *text, char *out, unsigned m) {
    unsigned o = 0;
    out[0] = '\0';
    if (m < 12) return;
    out[o++] = ENV_SOH;
    for (unsigned i = 0; rmid[i] && o < m - 2; i++) out[o++] = rmid[i];
    out[o++] = ENV_STX;
    for (unsigned i = 0; text[i] && o < m - 1; i++) out[o++] = text[i];
    out[o] = '\0';
}
/* Split an envelope. Writes the id (empty when absent) and returns the text —
 * a message from an older sender simply has no envelope and dedups by event id. */
static const char *env_split(const char *pt, char *rmid, unsigned m) {
    rmid[0] = '\0';
    if (pt[0] != ENV_SOH) return pt;
    unsigned i = 1, o = 0;
    while (pt[i] && pt[i] != ENV_STX && o < m - 1) rmid[o++] = pt[i++];
    rmid[o] = '\0';
    if (pt[i] != ENV_STX) { rmid[0] = '\0'; return pt; } /* not ours after all */
    return pt + i + 1;
}

static void rmid_new(char *out) {
    unsigned char r[4];
    hal_crypto_random((char *)r, 4);
    bytes_hex(r, 4, out); /* 8 hex chars */
}

/* ── Relay set (lane 2) ──────────────────────────────────────────────────
 * The RENDEZVOUS set for the recipient's key: sender and receiver derive the
 * same relays from the key alone, so neither has to know where the other polls.
 * Falls back to whatever Reticulum relays we can currently reach. */
static void relays_for(const char *peer_hex) {
    g_relays[0] = '\0';
    int n = hal_relay_for(peer_hex, str_len(peer_hex), g_relays, sizeof(g_relays) - 1);
    if (n > 0) { g_relays[n] = '\0'; if (!str_eq(g_relays, "[]")) return; }
    unsigned r = hal_relay_reachable(g_relays, sizeof(g_relays) - 1);
    if (r > 0 && r < sizeof(g_relays)) g_relays[r] = '\0';
    else str_copy(g_relays, "[]", sizeof(g_relays));
}

/* ── Ingest ──────────────────────────────────────────────────────────────
 * One funnel for BOTH lanes. Everything above it converts to: peer (hex),
 * plaintext, mine, lane id, ts. */
static void ingest(const char *peer_hex, const char *plaintext, int mine,
                   const char *lane_id, const char *ts) {
    char rmid[20];
    const char *text = env_split(plaintext, rmid, sizeof(rmid));

    /* Dedup on the envelope id when present (folds the two lanes together),
     * else on the lane's own event id (an older sender, one copy only). */
    const char *key = rmid[0] ? rmid : lane_id;
    int dup = seen_has(key);
    {   /* Which key a copy dedups on is the difference between one bubble and
         * four, and it is invisible from outside — so say it out loud. */
        char l[96];
        str_copy(l, dup ? "[messages] fold key=" : "[messages] recv key=", sizeof(l));
        str_cat(l, key, sizeof(l));
        str_cat(l, rmid[0] ? " (envelope)" : " (event-id)", sizeof(l));
        hal_log(1, l, str_len(l));
    }
    if (dup) { g_dupes++; return; }
    seen_add(key);

    int i = peer_add(peer_hex);
    if (i < 0) return;

    char title[40];
    peer_title(i, title, sizeof(title));
    convo_upsert(peer_hex, title, text, 1);
    convo_msg(peer_hex, mine ? "out" : "in", mine ? "me" : title, text,
              rmid[0] ? rmid : lane_id, ts);

    if (!mine) {
        g_recv++;
        if (!str_eq(g_open, peer_hex)) {
            g_peer_unread[i]++;
            unread_emit();
            notify_new(title, text);
        }
    }
}

/* A kind-4 event straight off the NOSTR lane. */
static void dm_ingest(const char *evt) {
    char sender[80] = "", content[4096] = "", ts[24] = "", id[80] = "";
    json_raw(evt, "pubkey", sender, sizeof(sender));
    json_raw(evt, "content", content, sizeof(content));
    json_raw(evt, "created_at", ts, sizeof(ts));
    json_raw(evt, "id", id, sizeof(id));
    if (!sender[0] || !content[0]) return;

    int mine = str_eq(sender, g_self);
    char peer[80];
    if (mine) { if (!find_p_tag(evt, peer, sizeof(peer))) return; }
    else str_copy(peer, sender, sizeof(peer));
    str_lower(peer);

    /* Decrypt with the OTHER party's key; the host supplies our private half. */
    int pn = hal_nostr_dm_decrypt(peer, str_len(peer), content, str_len(content),
                                  g_plain, sizeof(g_plain) - 1);
    if (pn <= 0) return;
    g_plain[pn] = '\0';
    ingest(peer, g_plain, mine, id, ts);
}

/* A store-and-forward DM off the Reticulum relay lane: already decrypted and
 * signature-checked host-side. {id, from(b64url), callsign, ts, text, mid} */
static void relay_ingest(const char *js) {
    char from[64] = "", id[80] = "", ts[24] = "", call[16] = "";
    char text[4096] = "", peer[80] = "";
    json_raw(js, "from", from, sizeof(from));
    json_raw(js, "id", id, sizeof(id));
    json_raw(js, "ts", ts, sizeof(ts));
    json_raw(js, "text", text, sizeof(text));
    json_raw(js, "callsign", call, sizeof(call));
    /* The host JSON-encoded this, so the envelope's control bytes arrive as
     * literal "" escapes. Decode before anything tries to read them. */
    json_unescape(text);
    if (!from[0] || !text[0]) return;
    if (!b64url_to_hex(from, peer, sizeof(peer))) return;

    int i = peer_add(peer);
    if (i >= 0 && call[0] && !g_pcall[i][0]) {
        str_copy(g_pcall[i], call, sizeof(g_pcall[0]));
        peers_save();
    }
    /* Relay DMs are only ever fetched for #p == us, so they are inbound. */
    ingest(peer, text, 0, id, ts);
}

/* ── Send ────────────────────────────────────────────────────────────────
 * Both lanes, one envelope id. We seed the seen-ring with our own id BEFORE
 * publishing: our kind-4 comes straight back to us on the `authors:[self]`
 * subscription (the local relay echoes instantly), and without this the echo
 * would print a second copy of our own message. */
static void do_send(const char *peer_hex, const char *text) {
    if (!is_hex64(peer_hex) || !text[0]) return;

    char rmid[20];
    rmid_new(rmid);

    static char env[4096];
    env_wrap(rmid, text, env, sizeof(env));

    seen_add(rmid); /* our own echo is a duplicate, by construction */

    int lane1 = hal_nostr_dm_send(peer_hex, str_len(peer_hex), env, str_len(env));

    char b64[64];
    hex_to_b64url(peer_hex, b64, sizeof(b64));
    int lane2 = 0;
    if (b64[0]) {
        relays_for(peer_hex);
        lane2 = hal_relay_dm_send(b64, str_len(b64), env, str_len(env),
                                  g_relays, str_len(g_relays),
                                  rmid, str_len(rmid));
    }

    int i = peer_add(peer_hex);
    char title[40];
    peer_title(i, title, sizeof(title));
    convo_upsert(peer_hex, title, text, 1);

    char ts[24];
    u64_str(hal_time_epoch(), ts);
    convo_msg(peer_hex, "out", "me", text, rmid, ts);
    g_sent++;

    if (lane1 <= 0 && lane2 <= 0) {
        status_line("send FAILED: no relay reachable on either lane");
        notify_new("Not sent", "No relay reachable — the message was not sent.");
    }
}

/* ── Commands ────────────────────────────────────────────────────────── */
static void prompt_new_message(void) {
    str_copy(g_msg,
        "{\"type\":\"ui.prompt\",\"id\":\"newmsg\",\"title\":\"New message\","
        "\"fullscreen\":true,\"fields\":[{\"name\":\"to\",\"type\":\"text\","
        "\"label\":\"Person\",\"hint\":\"npub1… , hex key, or a callsign\","
        "\"max\":80}],\"chips\":[", sizeof(g_msg));
    /* Known people as one-tap chips — the common case is writing to someone you
     * already know, and nobody wants to paste a 64-char key for that. */
    for (int i = 0; i < g_peer_n && i < 12; i++) {
        char title[40];
        peer_title(i, title, sizeof(title));
        if (i) str_cat(g_msg, ",", sizeof(g_msg));
        str_cat(g_msg, "{\"label\":\"", sizeof(g_msg));
        json_escape_cat(g_msg, title, sizeof(g_msg));
        str_cat(g_msg, "\",\"value\":\"", sizeof(g_msg));
        json_escape_cat(g_msg, g_peer[i], sizeof(g_msg));
        str_cat(g_msg, "\"}", sizeof(g_msg));
    }
    str_cat(g_msg, "]}", sizeof(g_msg));
    send_msg(g_msg);
}

/* Open (or create) the thread for whatever the user typed. A key opens straight
 * away; a callsign has to be resolved to a key first, so we ask the relays and
 * tell the user we are waiting rather than failing silently. */
static void open_target(const char *raw) {
    char in[128];
    str_copy(in, raw, sizeof(in));
    char hex[80];
    if (key_to_hex(in, hex, sizeof(hex))) {
        int i = peer_add(hex);
        char title[40];
        peer_title(i, title, sizeof(title));
        str_copy(g_msg, "{\"type\":\"ui.convo.upsert\",\"id\":\"", sizeof(g_msg));
        json_escape_cat(g_msg, hex, sizeof(g_msg));
        str_cat(g_msg, "\",\"title\":\"", sizeof(g_msg));
        json_escape_cat(g_msg, title, sizeof(g_msg));
        str_cat(g_msg, "\",\"icon\":\"person\",\"select\":true}", sizeof(g_msg));
        send_msg(g_msg);
        return;
    }
    /* Not a key — treat it as a callsign and ask the relay directory. */
    char call[32];
    str_copy(call, in, sizeof(call));
    str_upper(call);
    relays_for(g_self);
    hal_relay_resolve(call, str_len(call), g_relays, str_len(g_relays));
    char line[96];
    str_copy(line, "looking up ", sizeof(line));
    str_cat(line, call, sizeof(line));
    str_cat(line, " …", sizeof(line));
    status_line(line);
}

/* A callsign→key resolution came back: file the alias and open the thread. */
static void resolve_drain(void) {
    for (int i = 0; i < 8; i++) {
        char js[512];
        unsigned n = hal_relay_resolve_recv(js, sizeof(js) - 1);
        if (n == 0) break;
        js[n] = '\0';
        char call[24] = "", npub[64] = "", hex[80] = "";
        json_raw(js, "callsign", call, sizeof(call));
        json_raw(js, "npub", npub, sizeof(npub));
        if (!npub[0] || !b64url_to_hex(npub, hex, sizeof(hex))) continue;
        int idx = peer_add(hex);
        if (idx >= 0 && call[0]) {
            str_copy(g_pcall[idx], call, sizeof(g_pcall[0]));
            peers_save();
        }
        char title[40];
        peer_title(idx, title, sizeof(title));
        str_copy(g_msg, "{\"type\":\"ui.convo.upsert\",\"id\":\"", sizeof(g_msg));
        json_escape_cat(g_msg, hex, sizeof(g_msg));
        str_cat(g_msg, "\",\"title\":\"", sizeof(g_msg));
        json_escape_cat(g_msg, title, sizeof(g_msg));
        str_cat(g_msg, "\",\"icon\":\"person\",\"select\":true}", sizeof(g_msg));
        send_msg(g_msg);
        char line[96];
        str_copy(line, "found ", sizeof(line));
        str_cat(line, call[0] ? call : "key", sizeof(line));
        status_line(line);
    }
}

/* Names come from kind-0 profiles; ask for the ones we still lack. Calling
 * hal_nostr_profile also SUBSCRIBES to the kind-0, so an empty answer now
 * usually becomes a name a few ticks later. */
static void refresh_names(void) {
    /* THROTTLED, and deliberately so. hal_nostr_profile does not just read a
     * cache: calling it also SUBSCRIBES to the peer's kind-0, so calling it every
     * tick for every peer whose name we lack asks the host to re-fetch a profile
     * that is not coming, ~40 times a minute, forever. That work lands in the
     * NOSTR engine isolate (relay + sqlite), where it is invisible to this wapp's
     * own CPU accounting — the tick looks free while the host burns.
     *
     * A name is cosmetic. Once a minute is plenty. */
    unsigned long long now = hal_time_epoch();
    if (now - g_last_names < NAME_REFRESH_SEC) return;
    g_last_names = now;

    for (int i = 0; i < g_peer_n; i++) {
        if (g_pname[i][0]) continue;
        char js[512], name[40] = "";
        unsigned n = hal_nostr_profile(g_peer[i], str_len(g_peer[i]), js, sizeof(js) - 1);
        if (n == 0 || n >= sizeof(js)) continue;
        js[n] = '\0';
        json_raw(js, "name", name, sizeof(name));
        if (!name[0]) continue;
        str_copy(g_pname[i], name, sizeof(g_pname[0]));
        char title[40];
        peer_title(i, title, sizeof(title));
        convo_upsert(g_peer[i], title, "", 0); /* retitle the row, don't bump it */
    }
}

static void status_refresh(void) {
    char line[160], n[16];
    str_copy(line, "you: ", sizeof(line));
    str_cat(line, g_self[0] ? g_self : "(no profile)", sizeof(line));
    if (str_len(line) > 22) line[22] = '\0';
    status_line(line);

    str_copy(line, "sent ", sizeof(line));
    u64_str((unsigned long long)g_sent, n); str_cat(line, n, sizeof(line));
    str_cat(line, " · received ", sizeof(line));
    u64_str((unsigned long long)g_recv, n); str_cat(line, n, sizeof(line));
    str_cat(line, " · folded duplicates ", sizeof(line));
    u64_str((unsigned long long)g_dupes, n); str_cat(line, n, sizeof(line));
    status_line(line);

    relays_for(g_self);
    str_copy(line, "reticulum relays: ", sizeof(line));
    str_cat(line, g_relays, sizeof(line));
    status_line(line);
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */
/* Bring lane 1 up, and KEEP TRYING until it is up.
 *
 * This runs as a background wapp, so module_init fires while the app is still
 * booting — before the NOSTR hub exists. Subscribing once at init therefore
 * returns 0, and a subscription id we never retry means the whole NOSTR lane is
 * silently dead for the life of the process: no inbound DMs from internet
 * relays, and not even the echo of our own sent messages. (Observed on-device
 * exactly so: the Reticulum lane delivered, lane 1 never did.)
 *
 * So both halves are established lazily and retried every tick until they take.
 * Returns 1 once the subscription is live. */
static int lane1_ready(void) {
    if (g_sub_dm[0]) return 1;

    if (!g_self[0]) {
        unsigned n = hal_nostr_self(g_self, sizeof(g_self) - 1);
        if (n == 0 || n >= sizeof(g_self)) return 0; /* no profile yet */
        g_self[n] = '\0';
        str_lower(g_self);
    }

    /* Both halves of a thread: messages TO me, and the copies of the ones I
     * sent (so a conversation rehydrates, and our own echo can be folded). */
    char f[256];
    str_copy(f, "[{\"kinds\":[4],\"#p\":[\"", sizeof(f));
    str_cat(f, g_self, sizeof(f));
    str_cat(f, "\"]},{\"kinds\":[4],\"authors\":[\"", sizeof(f));
    str_cat(f, g_self, sizeof(f));
    str_cat(f, "\"]}]", sizeof(f));
    unsigned s = hal_nostr_subscribe(f, str_len(f), g_sub_dm, sizeof(g_sub_dm) - 1);
    if (s > 0 && s < sizeof(g_sub_dm)) { g_sub_dm[s] = '\0'; return 1; }
    g_sub_dm[0] = '\0';
    return 0; /* hub not up yet — try again next tick */
}

void module_init(void) {
    hal_log(1, "[messages] init", 15);
    seen_load();
    peers_load();
    purge_pre_envelope_history();
    lane1_ready(); /* usually too early; module_tick keeps trying */
    unread_emit();
    status_refresh();
}

void module_tick(void) {
    /* Lane 1 — the NOSTR subscription (internet + rns:// + local). Established
     * lazily: the hub is not up when a background wapp first ticks. */
    lane1_ready();
    for (int i = 0; i < 20 && g_sub_dm[0]; i++) {
        int n = hal_nostr_event_recv(g_sub_dm, str_len(g_sub_dm), g_evt, sizeof(g_evt) - 1);
        if (n <= 0) break;
        g_evt[n] = '\0';
        dm_ingest(g_evt);
    }

    /* Lane 2 — Reticulum relay store-and-forward. */
    for (int i = 0; i < 20; i++) {
        char js[4600];
        unsigned n = hal_relay_dm_recv(js, sizeof(js) - 1);
        if (n == 0) break;
        js[n] = '\0';
        relay_ingest(js);
    }

    unsigned long long now = hal_time_epoch();
    if (now - g_last_fetch >= RELAY_POLL_SEC) {
        g_last_fetch = now;
        relays_for(g_self); /* our OWN rendezvous set: where peers post to us */
        hal_relay_dm_fetch(0, g_relays, str_len(g_relays));
    }

    resolve_drain();
    refresh_names();
}

void module_handle_event(void) {
    static char buf[8192];
    if (hal_msg_available() == 0) return;
    unsigned n = hal_msg_recv(buf, sizeof(buf) - 1);
    if (n == 0) return;
    buf[n] = '\0';

    char cmd[32] = "";
    json_raw(buf, "command", cmd, sizeof(cmd));

    if (str_eq(cmd, "conversations_send")) {
        char peer[80] = "";
        static char text[4096];
        text[0] = '\0';
        json_raw(buf, "conversations_convo", peer, sizeof(peer));
        json_raw(buf, "conversations_input", text, sizeof(text));
        str_lower(peer);
        if (peer[0] && text[0]) do_send(peer, text);
        return;
    }
    if (str_eq(cmd, "conversations_open")) {
        char peer[80] = "";
        json_raw(buf, "conversations_convo", peer, sizeof(peer));
        str_lower(peer);
        str_copy(g_open, peer, sizeof(g_open));
        int i = peer_idx(peer);
        if (i >= 0 && g_peer_unread[i] > 0) {
            g_peer_unread[i] = 0; /* read */
            unread_emit();
        }
        return;
    }
    if (str_eq(cmd, "new_message")) { prompt_new_message(); return; }
    if (str_eq(cmd, "refresh_status")) { status_refresh(); return; }

    /* Result of the New-message prompt. */
    char pid[24] = "";
    json_raw(buf, "id", pid, sizeof(pid));
    if (str_eq(cmd, "prompt") || str_eq(pid, "newmsg")) {
        char to[128] = "";
        if (!json_raw(buf, "to", to, sizeof(to))) json_raw(buf, "value", to, sizeof(to));
        if (to[0]) open_target(to);
        return;
    }

}

void module_destroy(void) { }

uint32_t module_tick_interval_ms(void) { return 1500; }
