/*
 * nostr — a NOSTR client wapp.
 *
 * Reads notes (kind-1) from the accounts you follow and lets you post your own,
 * over a TRANSPORT-ABSTRACT relay list: internet (wss://), Reticulum (rns://) or
 * this device itself (local). All relay/crypto/signing work is host-side via the
 * hal.nostr family — this module just drives the UI:
 *   - Feed screen ($type:"chat"): drains events into the feed, composes posts.
 *   - "NOSTR servers" menu panel ($type:"people"): relay list + status + add/
 *     remove.
 *
 * Build: cd wapps/nostr && WASI_SDK_PATH=~/wasi-sdk make
 */
#include "../hal/geogram_wasm_hal.h"

/* ── String helpers ──────────────────────────────────────────────────── */
static unsigned str_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int str_eq(const char *a, const char *b) { while (*a && *b && *a == *b) { a++; b++; } return *a == *b; }
static void str_copy(char *d, const char *s, unsigned m) { unsigned i = 0; while (i < m - 1 && s[i]) { d[i] = s[i]; i++; } d[i] = '\0'; }
static void str_cat(char *d, const char *s, unsigned m) { unsigned l = str_len(d); unsigned i = 0; while (l + i < m - 1 && s[i]) { d[l + i] = s[i]; i++; } d[l + i] = '\0'; }

/* Find "key":<value> in flat JSON, copy the raw value. Skips \" inside strings
 * so a note containing a quote is not truncated. */
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

/* Escape a raw string into a JSON string body (append to dst). */
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

static void send_msg(const char *json) { hal_msg_send(json, str_len(json)); }

/* ── State ───────────────────────────────────────────────────────────── */
static char g_sub[64] = "";        /* feed subscription id                 */
static char g_evt[8192];           /* one drained event JSON               */
static char g_relays[8192];        /* hal_nostr_relays output              */
static char g_msg[16384];          /* outbound UI message                  */
static char g_follows[4096];       /* followed pubkeys JSON array          */
static int  g_relay_ticks = 0;

/* ── Feed subscription ───────────────────────────────────────────────── */
static void subscribe_feed(void) {
    if (g_sub[0]) return;                 /* already subscribed */
    int fn = hal_nostr_follows(g_follows, sizeof(g_follows) - 1);
    if (fn > 0) g_follows[fn] = '\0'; else str_copy(g_follows, "[]", sizeof(g_follows));

    char filter[4352];
    if (str_len(g_follows) > 2) {         /* have follows -> their notes */
        str_copy(filter, "{\"kinds\":[1],\"authors\":", sizeof(filter));
        str_cat(filter, g_follows, sizeof(filter));
        str_cat(filter, ",\"limit\":100}", sizeof(filter));
    } else {                              /* no follows -> a global sample */
        str_copy(filter, "{\"kinds\":[1],\"limit\":50}", sizeof(filter));
    }
    int n = hal_nostr_subscribe(filter, str_len(filter), g_sub, sizeof(g_sub) - 1);
    if (n > 0) g_sub[n] = '\0'; else g_sub[0] = '\0';
}

/* Append one event to the feed as a chat message. */
static void feed_append(const char *evt) {
    char pubkey[80] = "", content[6000] = "", ts[24] = "";
    json_raw(evt, "pubkey", pubkey, sizeof(pubkey));
    json_raw(evt, "content", content, sizeof(content)); /* still JSON-escaped */
    json_raw(evt, "created_at", ts, sizeof(ts));
    if (!content[0]) return;
    char from[16] = "";
    str_copy(from, pubkey, sizeof(from));               /* short pubkey label */

    str_copy(g_msg, "{\"type\":\"ui.chat.append\",\"field\":\"feed\",\"message\":{\"dir\":\"in\",\"from\":\"", sizeof(g_msg));
    str_cat(g_msg, from, sizeof(g_msg));
    /* content came out of json_raw already escaped, so embed it verbatim. */
    str_cat(g_msg, "\",\"text\":\"", sizeof(g_msg));
    str_cat(g_msg, content, sizeof(g_msg));
    str_cat(g_msg, "\",\"time\":", sizeof(g_msg));
    str_cat(g_msg, ts[0] ? ts : "0", sizeof(g_msg));
    str_cat(g_msg, "}}", sizeof(g_msg));
    send_msg(g_msg);
}

static void drain_feed(void) {
    if (!g_sub[0]) return;
    for (int i = 0; i < 20; i++) {        /* bounded per tick */
        int n = hal_nostr_event_recv(g_sub, str_len(g_sub), g_evt, sizeof(g_evt) - 1);
        if (n <= 0) break;
        g_evt[n] = '\0';
        feed_append(g_evt);
    }
}

/* ── Relay panel ─────────────────────────────────────────────────────── */
static void push_relays(void) {
    int n = hal_nostr_relays(g_relays, sizeof(g_relays) - 1);
    if (n <= 0) { g_relays[0] = '\0'; }
    else g_relays[n] = '\0';

    str_copy(g_msg, "{\"type\":\"ui.people.set\",\"field\":\"relays\",\"sections\":[{\"title\":\"Relays\",\"items\":[", sizeof(g_msg));
    int first = 1;
    for (char *p = g_relays; *p; p++) {
        if (*p != '{') continue;
        char uri[256] = "", scheme[24] = "", status[24] = "";
        json_raw(p, "uri", uri, sizeof(uri));
        json_raw(p, "scheme", scheme, sizeof(scheme));
        json_raw(p, "status", status, sizeof(status));
        if (!uri[0]) continue;
        if (!first) str_cat(g_msg, ",", sizeof(g_msg));
        first = 0;
        str_cat(g_msg, "{\"id\":\"", sizeof(g_msg));
        json_escape_cat(g_msg, uri, sizeof(g_msg));
        str_cat(g_msg, "\",\"title\":\"", sizeof(g_msg));
        json_escape_cat(g_msg, uri, sizeof(g_msg));
        str_cat(g_msg, "\",\"subtitle\":\"", sizeof(g_msg));
        str_cat(g_msg, scheme, sizeof(g_msg));
        str_cat(g_msg, "\",\"tags\":[\"", sizeof(g_msg));
        str_cat(g_msg, status[0] ? status : "?", sizeof(g_msg));
        str_cat(g_msg, "\"]}", sizeof(g_msg));
        /* advance past this object */
        while (*p && *p != '}') p++;
        if (!*p) break;
    }
    str_cat(g_msg, "]}]}", sizeof(g_msg));
    send_msg(g_msg);
}

/* ── Module entry points ─────────────────────────────────────────────── */
int32_t module_init(void) {
    hal_log(6, "[nostr] up", 10);
    subscribe_feed();
    push_relays();
    return 0;
}

int32_t module_tick(void) {
    subscribe_feed();        /* re-arm if a profile/follow change reset it */
    drain_feed();
    if (++g_relay_ticks % 8 == 0) push_relays();  /* refresh status ~12s */
    return 0;
}

int32_t module_handle_event(void) {
    static char buf[4096];
    uint32_t n = hal_msg_recv(buf, sizeof(buf) - 1);
    if (n == 0) return 0;
    buf[n] = '\0';
    char cmd[64] = "";
    if (!json_raw(buf, "command", cmd, sizeof(cmd))) return 0;

    if (str_eq(cmd, "ready") || str_eq(cmd, "refresh")) {
        subscribe_feed();
        push_relays();
    } else if (str_eq(cmd, "feed_send")) {
        char text[6000] = "";
        if (json_raw(buf, "feed_input", text, sizeof(text)) && text[0]) {
            hal_nostr_post(1, text, str_len(text), "[]", 2);
        }
    } else if (str_eq(cmd, "relay_add")) {
        char uri[256] = "";
        if (json_raw(buf, "new_relay", uri, sizeof(uri)) && uri[0]) {
            hal_nostr_relay_add(uri, str_len(uri));
            push_relays();
        }
    } else if (str_eq(cmd, "relays_tap") || str_eq(cmd, "relays")) {
        char uri[256] = "";
        if (json_raw(buf, "relays_id", uri, sizeof(uri)) && uri[0]) {
            hal_nostr_relay_remove(uri, str_len(uri));
            push_relays();
        }
    }
    return 0;
}

int32_t module_tick_interval_ms(void) { return 1500; }

void module_destroy(void) {}
