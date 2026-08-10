/*
 * xprs — what this device can hear on the air.
 *
 * Two read-only host calls, nothing else:
 *   hal_xprs_stations → stations heard, pre-shaped as people-widget sections
 *   hal_xprs_traffic  → the recent packet ring, oldest first
 *
 * It transmits nothing. It is a window onto the radio, and deliberately shows
 * traffic addressed to OTHER people too — on a mesh that is most of what goes
 * past, and being able to see it is the difference between believing the
 * network works and knowing it does.
 *
 * Nothing here arrived over the internet. The host records a packet's bearer
 * where it lands and only collects radio and local ones, so this view cannot
 * show an internet peer even by mistake.
 *
 * Build: cd wapps/xprs && make
 */

#include "../hal/geogram_wasm_hal.h"

/* ── String helpers (no libc under wasm32-wasi -nostartfiles) ─────────── */
static unsigned str_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int str_eq(const char *a, const char *b) { while (*a && *b && *a == *b) { a++; b++; } return *a == *b; }
static void str_copy(char *d, const char *s, unsigned m) { unsigned i = 0; while (i < m - 1 && s[i]) { d[i] = s[i]; i++; } d[i] = '\0'; }
static void str_cat(char *d, const char *s, unsigned m) { unsigned l = str_len(d); unsigned i = 0; while (l + i < m - 1 && s[i]) { d[l + i] = s[i]; i++; } d[l + i] = '\0'; }

/* Find "key":<value> in flat JSON starting at `json`, copy the raw value.
 * Same scanner the bluetooth wapp uses — the host emits flat objects. */
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
            if (instr) { if (*p == '"') break; }
            else if (*p == ',' || *p == '}') break;
            out[o++] = *p++;
        }
        out[o] = '\0';
        return 1;
    }
    return 0;
}

/* Escape a string into a JSON context. */
static void json_esc(char *d, unsigned m, const char *s) {
    unsigned l = str_len(d);
    for (unsigned i = 0; s[i] && l < m - 2; i++) {
        char c = s[i];
        if (c == '"' || c == '\\') { d[l++] = '\\'; d[l++] = c; }
        else if (c == '\n' || c == '\r' || c == '\t') { d[l++] = ' '; }
        else { d[l++] = c; }
    }
    d[l] = '\0';
}

/* Right-pad so the log lines form columns a person can scan. */
static void pad_to(char *d, unsigned m, unsigned width) {
    unsigned l = str_len(d);
    /* Count from the last newline, not the start: d is one line here. */
    while (l < width && l < m - 1) { d[l++] = ' '; }
    d[l] = '\0';
}

/* ── Buffers ─────────────────────────────────────────────────────────── */
static char g_stations[32768];
static char g_traffic[65536];
static char g_msg[40960];
static char g_line[512];
static char g_last_id[8] = "";   /* newest packet already rendered */
static int  g_have_last = 0;

static void send_msg(const char *json) { hal_msg_send(json, str_len(json)); }

static void log_line(const char *text) {
    char m[700];
    str_copy(m, "{\"type\":\"ui.log.append\",\"field\":\"traffic\",\"line\":\"", sizeof(m));
    json_esc(m, sizeof(m), text);
    str_cat(m, "\"}", sizeof(m));
    send_msg(m);
}

static void log_clear(void) {
    send_msg("{\"type\":\"ui.log.clear\",\"field\":\"traffic\"}");
}

/* ── Stations ────────────────────────────────────────────────────────── */
static void push_stations(void) {
    int n = hal_xprs_stations(g_stations, sizeof(g_stations) - 1);
    if (n <= 0) return;                       /* 0 = nothing, <0 = too small */
    g_stations[n] = '\0';
    str_copy(g_msg, "{\"type\":\"ui.people.set\",\"field\":\"stations\",\"sections\":",
             sizeof(g_msg));
    str_cat(g_msg, g_stations, sizeof(g_msg));
    str_cat(g_msg, "}", sizeof(g_msg));
    send_msg(g_msg);
}

/* ── Traffic ─────────────────────────────────────────────────────────── */

/* One rendered line:
 *   BLE   -37  X1RD89  observation  peers 12, mail 3
 *   BLE   -42  X1A67X  message      -> X32DVA (passing)
 */
static void render_packet(const char *obj) {
    char bearer[16] = "", rssi[12] = "", from[16] = "", to[16] = "";
    char type[16] = "", mine[8] = "", wire[300] = "";
    json_raw(obj, "bearer", bearer, sizeof(bearer));
    json_raw(obj, "rssi", rssi, sizeof(rssi));
    json_raw(obj, "from", from, sizeof(from));
    json_raw(obj, "to", to, sizeof(to));
    json_raw(obj, "type", type, sizeof(type));
    json_raw(obj, "mine", mine, sizeof(mine));
    json_raw(obj, "wire", wire, sizeof(wire));

    /* Bearer, upper-cased, padded into a column. */
    str_copy(g_line, "", sizeof(g_line));
    for (unsigned i = 0; bearer[i] && i < 8; i++) {
        char c = bearer[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        unsigned l = str_len(g_line);
        g_line[l] = c; g_line[l + 1] = '\0';
    }
    pad_to(g_line, sizeof(g_line), 6);

    if (rssi[0] && !str_eq(rssi, "0")) {
        str_cat(g_line, rssi, sizeof(g_line));
        str_cat(g_line, " dBm", sizeof(g_line));
    }
    pad_to(g_line, sizeof(g_line), 15);

    str_cat(g_line, from, sizeof(g_line));
    pad_to(g_line, sizeof(g_line), 26);

    str_cat(g_line, type, sizeof(g_line));
    pad_to(g_line, sizeof(g_line), 40);

    /* Who it was for. Traffic that is merely passing is the interesting part
     * of a mesh, so it is labelled rather than hidden. */
    if (str_eq(mine, "true")) {
        str_cat(g_line, "for us", sizeof(g_line));
    } else if (to[0]) {
        str_cat(g_line, "-> ", sizeof(g_line));
        str_cat(g_line, to, sizeof(g_line));
        str_cat(g_line, " (passing)", sizeof(g_line));
    } else {
        str_cat(g_line, "broadcast", sizeof(g_line));
    }

    log_line(g_line);
    /* The packet itself, indented, so the trace is the actual wire text. */
    str_copy(g_line, "        ", sizeof(g_line));
    str_cat(g_line, wire, sizeof(g_line));
    log_line(g_line);
}

/* Walk the traffic array and render everything newer than the last one shown.
 * The host returns a ring, so a poll usually repeats what we already have. */
static void push_traffic(int force) {
    int n = hal_xprs_traffic(g_traffic, sizeof(g_traffic) - 1);
    if (n <= 0) return;
    g_traffic[n] = '\0';

    if (force) { log_clear(); g_have_last = 0; g_last_id[0] = '\0'; }

    /* Find where to resume: everything after the object carrying g_last_id. */
    char *start = g_traffic;
    if (g_have_last) {
        for (char *p = g_traffic; *p; p++) {
            if (*p != '{') continue;
            char id[8] = "";
            json_raw(p, "id", id, sizeof(id));
            if (str_eq(id, g_last_id)) {
                while (*p && *p != '}') p++;
                start = *p ? p + 1 : p;
            }
        }
        if (start == g_traffic) {
            /* The one we last showed has aged out of the ring: everything
             * here is new. Better to repeat than to silently skip. */
            start = g_traffic;
        }
    }

    for (char *p = start; *p; p++) {
        if (*p != '{') continue;
        render_packet(p);
        char id[8] = "";
        if (json_raw(p, "id", id, sizeof(id))) {
            str_copy(g_last_id, id, sizeof(g_last_id));
            g_have_last = 1;
        }
        while (*p && *p != '}') p++;
        if (!*p) break;
    }
}

/* ── Module entry points ─────────────────────────────────────────────── */
int32_t module_init(void) {
    hal_log(6, "[xprs] listening to the air", 27);
    push_stations();
    push_traffic(1);
    return 0;
}

int32_t module_tick(void) {
    push_stations();
    push_traffic(0);
    return 0;
}

int32_t module_handle_event(void) {
    static char buf[2048];
    uint32_t n = hal_msg_recv(buf, sizeof(buf) - 1);
    if (n == 0) return 0;
    buf[n] = '\0';
    char cmd[64] = "";
    if (!json_raw(buf, "command", cmd, sizeof(cmd))) return 0;

    if (str_eq(cmd, "ready") || str_eq(cmd, "refresh")) {
        push_stations();
        push_traffic(1);
    } else if (str_eq(cmd, "traffic_clear")) {
        log_clear();
        g_have_last = 0;
        g_last_id[0] = '\0';
    }
    return 0;
}

int32_t module_tick_interval_ms(void) { return 3000; }

void module_destroy(void) {}
