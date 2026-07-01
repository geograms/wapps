/*
 * bluetooth — the BLE street mesh's face (doc/mesh.md §12, milestone M1).
 *
 * A thin driver around two read-only host HAL calls:
 *   hal_mesh_status  → node status JSON (callsign, advertising, counters)
 *   hal_mesh_devices → devices in reach, pre-shaped as people-widget sections
 *
 * Each tick it fetches the device sections and forwards them verbatim to the
 * native `$type:"people"` list via ui.people.set — refreshing only when the
 * host's revision counter moved, so an idle street costs no UI churn. Row
 * actions come back as devices_* commands (M1: logged; messaging lands in M2).
 *
 * Build: cd wapps/bluetooth && make
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
static char g_data[32768];   /* hal_mesh_devices output (sections JSON)     */
static char g_msg[33280];    /* outbound ui.people.set wrapper              */
static char g_status[1024];  /* hal_mesh_status output                      */
static char g_last_rev[16] = "";

static void send_msg(const char *json) { hal_msg_send(json, str_len(json)); }

static void push_devices(void) {
    int n = hal_mesh_devices(g_data, sizeof(g_data));
    if (n <= 0) return;
    g_data[n] = '\0';
    str_copy(g_msg, "{\"type\":\"ui.people.set\",\"field\":\"devices\",\"sections\":",
             sizeof(g_msg));
    str_cat(g_msg, g_data, sizeof(g_msg));
    str_cat(g_msg, "}", sizeof(g_msg));
    send_msg(g_msg);
}

/* Refresh the list only when the host mesh registry actually changed. */
static void tick_refresh(int force) {
    int n = hal_mesh_status(g_status, sizeof(g_status));
    if (n <= 0) return;
    g_status[n] = '\0';
    char rev[16] = "";
    json_raw(g_status, "revision", rev, sizeof(rev));
    if (!force && str_eq(rev, g_last_rev)) return;
    str_copy(g_last_rev, rev, sizeof(g_last_rev));
    push_devices();
}

/* ── Module entry points ─────────────────────────────────────────────── */
int32_t module_init(void) {
    hal_log(6, "[bluetooth] mesh view up", 24);
    tick_refresh(1);
    return 0;
}

int32_t module_tick(void) {
    tick_refresh(0);
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
        tick_refresh(1);
    } else if (str_eq(cmd, "devices_tap") || str_eq(cmd, "devices_message")) {
        /* M2: open a 1:1 conversation with the tapped device. For M1 the tap
         * just forces a refresh so the row's freshness updates. */
        tick_refresh(1);
    }
    return 0;
}

int32_t module_tick_interval_ms(void) { return 2000; }

void module_destroy(void) {}
