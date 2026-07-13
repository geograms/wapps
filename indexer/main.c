/*
 * indexer — volunteer this device as an Indexer, and see what it does with it.
 *
 * An Indexer answers ONE question for the network: "where can I find notes from
 * npub X?". It hands out addresses — signed pointers, ~176 bytes — and it never
 * holds other people's posts. It is a phone book, not a library, and a person
 * should be told that plainly before they turn it on, because an Indexer that
 * vanishes costs the network a directory, not an archive. That is exactly what
 * stops the whole thing sliding back into a few big servers with everything on
 * them.
 *
 * Until this wapp existed the role was inferred from the charger and the WiFi: a
 * decent default and a bad only-option, because the old phone in a drawer had no
 * way to say "yes, use this", and the metered home line had no way to say "no,
 * don't". So: three states, and revoking is exactly as easy as granting.
 *
 * Host HAL (read-mostly, cheap — counters the node already keeps):
 *   hal_node_status   → JSON: volunteer, serving, pointers, authors, syncPeers…
 *   hal_node_peers    → the other Indexers (and the leaves we leave alone)
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
static char g_msg[24576];

static void send_msg(const char *json) { hal_msg_send(json, str_len(json)); }

/* One row for the people list. */
static void row(char *dst, unsigned cap, const char *id, const char *title,
                const char *subtitle, int online) {
    str_cat(dst, "{\"id\":\"", cap);
    str_cat(dst, id, cap);
    str_cat(dst, "\",\"title\":\"", cap);
    str_cat(dst, title, cap);
    str_cat(dst, "\",\"subtitle\":\"", cap);
    str_cat(dst, subtitle, cap);
    str_cat(dst, "\",\"online\":", cap);
    str_cat(dst, online ? "true}" : "false}", cap);
}

/* The Volunteer screen: what this device is offering, and what it costs. */
static void push_volunteer(void) {
    int n = hal_node_status(g_status, sizeof(g_status) - 1);
    if (n <= 0) return;
    g_status[n] = '\0';

    char vol[16] = "auto", serving[8] = "false", role[16] = "leaf";
    char pointers[16] = "0", authors[16] = "0", peers[16] = "0";
    char demoted[16] = "0", rejected[16] = "0", uptime[16] = "0";
    char power[24] = "", uplink[24] = "", powered[8] = "0";
    json_raw(g_status, "volunteer", vol, sizeof(vol));
    json_raw(g_status, "serving", serving, sizeof(serving));
    json_raw(g_status, "role", role, sizeof(role));
    json_raw(g_status, "pointers", pointers, sizeof(pointers));
    json_raw(g_status, "authors", authors, sizeof(authors));
    json_raw(g_status, "syncPeers", peers, sizeof(peers));
    json_raw(g_status, "demoted", demoted, sizeof(demoted));
    json_raw(g_status, "rejected", rejected, sizeof(rejected));
    json_raw(g_status, "uptimeSec", uptime, sizeof(uptime));
    json_raw(g_status, "power", power, sizeof(power));
    json_raw(g_status, "uplink", uplink, sizeof(uplink));
    json_raw(g_status, "poweredPct", powered, sizeof(powered));

    int on = str_eq(serving, "true");

    str_copy(g_msg, "{\"type\":\"ui.people.set\",\"field\":\"live\",\"sections\":[", sizeof(g_msg));

    /* 1. The offer itself, said in words a person can act on. */
    str_cat(g_msg, "{\"title\":\"This device\",\"items\":[", sizeof(g_msg));
    {
        char sub[192];
        str_copy(sub, on ? "Answering 'where can I find npub X' for the network"
                         : "Not serving. Nobody is being sent here.", sizeof(sub));
        row(g_msg, sizeof(g_msg), "role",
            on ? "Indexer - serving" : "Leaf - not serving", sub, on);
    }
    str_cat(g_msg, ",", sizeof(g_msg));
    {
        char sub[192];
        str_copy(sub, "off - hold nothing | auto - serve when plugged in | always", sizeof(sub));
        char title[64];
        str_copy(title, "Volunteer: ", sizeof(title));
        str_cat(title, vol, sizeof(title));
        row(g_msg, sizeof(g_msg), "volunteer", title, sub, !str_eq(vol, "off"));
    }
    str_cat(g_msg, "]}", sizeof(g_msg));

    /* 2. The numbers. A role nobody can inspect is a role nobody trusts. */
    str_cat(g_msg, ",{\"title\":\"What it is doing\",\"items\":[", sizeof(g_msg));
    {
        char sub[192];
        str_copy(sub, "Addresses held. An indexer stores no one else's posts.", sizeof(sub));
        char title[64];
        str_copy(title, pointers, sizeof(title));
        str_cat(title, " pointers", sizeof(title));
        row(g_msg, sizeof(g_msg), "pointers", title, sub, on);
    }
    str_cat(g_msg, ",", sizeof(g_msg));
    {
        char title[64];
        str_copy(title, authors, sizeof(title));
        str_cat(title, " authors this device is a home for", sizeof(title));
        row(g_msg, sizeof(g_msg), "authors", title,
            "Published as who-has records, so people find you, not a server.", on);
    }
    str_cat(g_msg, ",", sizeof(g_msg));
    {
        char title[64];
        str_copy(title, peers, sizeof(title));
        str_cat(title, " indexers synced with", sizeof(title));
        row(g_msg, sizeof(g_msg), "sync", title,
            "Indexers spread the map between themselves, so phones do not have to.", on);
    }
    str_cat(g_msg, ",", sizeof(g_msg));
    {
        char title[80];
        str_copy(title, demoted, sizeof(title));
        str_cat(title, " dead pointers pruned - ", sizeof(title));
        str_cat(title, rejected, sizeof(title));
        str_cat(title, " stores refused", sizeof(title));
        row(g_msg, sizeof(g_msg), "hygiene", title,
            "A provider that will not serve stops being handed out.", 0);
    }
    str_cat(g_msg, "]}", sizeof(g_msg));

    /* 3. The hardware, read from Settings -> Hardware. Stated once, for the
     *    device, never asked for twice. */
    str_cat(g_msg, ",{\"title\":\"Hardware (Settings)\",\"items\":[", sizeof(g_msg));
    {
        char title[96];
        str_copy(title, power[0] ? power : "power not stated", sizeof(title));
        str_cat(title, " - ", sizeof(title));
        str_cat(title, uplink[0] ? uplink : "uplink unknown", sizeof(title));
        char sub[96];
        str_copy(sub, "Powered ", sizeof(sub));
        str_cat(sub, powered, sizeof(sub));
        str_cat(sub, "% of the last week (measured here)", sizeof(sub));
        row(g_msg, sizeof(g_msg), "hw", title, sub, 1);
    }
    str_cat(g_msg, "]}]}", sizeof(g_msg));
    send_msg(g_msg);
}

/* The network: who else is indexing, and the leaves we deliberately leave alone. */
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
    push_volunteer();
    push_peers();
}

/* Cycle the volunteer state: off -> auto -> always -> off. Revoking has to be
 * exactly as easy as granting, or it was never really granted. */
static void cycle_volunteer(void) {
    int n = hal_node_status(g_status, sizeof(g_status) - 1);
    if (n <= 0) return;
    g_status[n] = '\0';
    char vol[16] = "auto";
    json_raw(g_status, "volunteer", vol, sizeof(vol));

    const char *next = "auto";
    if (str_eq(vol, "off")) next = "auto";
    else if (str_eq(vol, "auto")) next = "always";
    else next = "off";

    char kv[32];
    str_copy(kv, "volunteer=", sizeof(kv));
    str_cat(kv, next, sizeof(kv));
    hal_node_set_pref(kv, str_len(kv));
    refresh();
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
    } else if (str_eq(cmd, "live_tap")) {
        char id[32] = "";
        if (json_raw(buf, "live_id", id, sizeof(id)) && str_eq(id, "volunteer")) {
            cycle_volunteer();
        }
    } else if (str_eq(cmd, "volunteer")) {
        cycle_volunteer();
    }
    return 0;
}

/* How often the host ticks us. Five seconds: these are counters, not a feed. */
int32_t module_tick_interval_ms(void) { return 5000; }

int32_t module_destroy(void) { return 0; }
