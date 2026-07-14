/*
 * indexer — volunteer this device as an Indexer, and see what it does with it.
 *
 * An Indexer answers ONE question for the network: "where can I find notes from
 * npub X?". It hands out addresses — signed pointers, ~176 bytes — and it never
 * holds other people's posts. It is a phone book, not a library, and a person
 * should be told that plainly before they turn it on: an Indexer that vanishes
 * costs the network a directory, not an archive. That is exactly what stops the
 * whole thing sliding back into a few big servers with everything on them.
 *
 * The role used to be inferred from the charger and the WiFi — a decent default
 * and a bad only-option, because the old phone in a drawer had no way to say
 * "yes, use this" and the metered home line had no way to say "no, don't". So it
 * is an OPTION with three states, and revoking is exactly as easy as granting.
 *
 * UI note, learned the hard way: this is a settings screen, so it is built out
 * of settings — a picker and a handful of read-only values. It was briefly built
 * out of the people widget, whose sections render as TABS, which turned a page
 * of options into a page of tabs nobody asked for. Use the widget that means
 * what you mean.
 *
 * Host HAL:
 *   hal_node_status   → JSON: volunteer, serving, pointers, authors, syncPeers…
 *   hal_node_peers    → the other indexers (and the leaves we leave alone)
 *   hal_node_set_pref → volunteer=off|auto|always
 *
 * Build: cd wapps/indexer && make
 */

#include "../hal/geogram_wasm_hal.h"

/* ── String helpers ──────────────────────────────────────────────────── */
static unsigned str_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int str_eq(const char *a, const char *b) { while (*a && *b && *a == *b) { a++; b++; } return *a == *b; }
static void str_copy(char *d, const char *s, unsigned m) { unsigned i = 0; while (i < m - 1 && s[i]) { d[i] = s[i]; i++; } d[i] = '\0'; }
static void str_cat(char *d, const char *s, unsigned m) { unsigned l = str_len(d); unsigned i = 0; while (l + i < m - 1 && s[i]) { d[l + i] = s[i]; i++; } d[l + i] = '\0'; }

/* Minimal scanner: find "key":<value> in flat JSON, copy the raw value. */
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

/* ── Buffers ─────────────────────────────────────────────────────────── */
static char g_status[2048];
static char g_peers[16384];
static char g_msg[20480];

static void send_msg(const char *json) { hal_msg_send(json, str_len(json)); }

/* Set one field's value on the screen. */
static void set_field(const char *name, const char *value) {
    str_copy(g_msg, "{\"type\":\"ui.set_field\",\"name\":\"", sizeof(g_msg));
    str_cat(g_msg, name, sizeof(g_msg));
    str_cat(g_msg, "\",\"value\":\"", sizeof(g_msg));
    str_cat(g_msg, value, sizeof(g_msg));
    str_cat(g_msg, "\"}", sizeof(g_msg));
    send_msg(g_msg);
}

static void push_status(void) {
    int n = hal_node_status(g_status, sizeof(g_status) - 1);
    if (n <= 0) return;
    g_status[n] = '\0';

    char vol[16] = "auto", serving[8] = "false";
    char pointers[16] = "0", authors[16] = "0", peers[16] = "0";
    char demoted[16] = "0", rejected[16] = "0";
    char power[24] = "", uplink[24] = "", powered[8] = "0";
    json_raw(g_status, "volunteer", vol, sizeof(vol));
    json_raw(g_status, "serving", serving, sizeof(serving));
    json_raw(g_status, "pointers", pointers, sizeof(pointers));
    json_raw(g_status, "authors", authors, sizeof(authors));
    json_raw(g_status, "syncPeers", peers, sizeof(peers));
    json_raw(g_status, "demoted", demoted, sizeof(demoted));
    json_raw(g_status, "rejected", rejected, sizeof(rejected));
    json_raw(g_status, "power", power, sizeof(power));
    json_raw(g_status, "uplink", uplink, sizeof(uplink));
    json_raw(g_status, "poweredPct", powered, sizeof(powered));

    int on = str_eq(serving, "true");

    set_field("status", on ? "Serving - answering 'where can I find npub X'"
                           : "Not serving. Nobody is being sent here.");
    set_field("volunteer", vol);

    {
        char v[64];
        str_copy(v, pointers, sizeof(v));
        str_cat(v, " (addresses, not posts)", sizeof(v));
        set_field("pointers", v);
    }
    set_field("authors", authors);
    set_field("peers", peers);
    {
        char v[64];
        str_copy(v, demoted, sizeof(v));
        str_cat(v, " pruned - ", sizeof(v));
        str_cat(v, rejected, sizeof(v));
        str_cat(v, " refused", sizeof(v));
        set_field("hygiene", v);
    }
    {
        char v[96];
        str_copy(v, power[0] ? power : "power not stated", sizeof(v));
        str_cat(v, " - ", sizeof(v));
        str_cat(v, uplink[0] ? uplink : "uplink unknown", sizeof(v));
        str_cat(v, " - powered ", sizeof(v));
        str_cat(v, powered, sizeof(v));
        str_cat(v, "% of the last week", sizeof(v));
        set_field("hardware", v);
    }
}

/* The other indexers — and the leaves. This one really IS a list. */
static void push_peers(void) {
    int n = hal_node_peers(g_peers, sizeof(g_peers) - 1);
    if (n <= 0) return;
    g_peers[n] = '\0';
    str_copy(g_msg, "{\"type\":\"ui.people.set\",\"field\":\"network\",\"sections\":", sizeof(g_msg));
    str_cat(g_msg, g_peers, sizeof(g_msg));
    str_cat(g_msg, "}", sizeof(g_msg));
    send_msg(g_msg);
}

static void refresh(void) {
    push_status();
    push_peers();
}

/* ── Module entry points ─────────────────────────────────────────────── */
int32_t module_init(void) {
    hal_log(6, "[indexer] up", 12);
    refresh();
    return 0;
}

int32_t module_tick(void) {
    refresh();
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
        refresh();
    } else if (str_eq(cmd, "volunteer_changed")) {
        /* The person picked a state; set exactly that. No cycling: a control you
         * cannot predict before you touch it is a trap, and a mis-tap here
         * quietly stops this device serving the network. */
        char vol[16] = "";
        if (json_raw(buf, "volunteer", vol, sizeof(vol)) && vol[0]) {
            char kv[32];
            str_copy(kv, "volunteer=", sizeof(kv));
            str_cat(kv, vol, sizeof(kv));
            hal_node_set_pref(kv, str_len(kv));
            refresh();
        }
    }
    return 0;
}

/* Counters, not a feed. Five seconds is plenty. */
int32_t module_tick_interval_ms(void) { return 5000; }

int32_t module_destroy(void) { return 0; }
