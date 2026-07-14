/*
 * archiver — donate storage on purpose, with a number you choose, and see
 * exactly what is on your disk and why.
 *
 * Pointers are worthless if every copy they point at is asleep or gone. Somebody
 * has to be willing to hold OTHER PEOPLE'S bytes — and that is a separate,
 * explicit, quota-bound offer, never an accident of having volunteered to index.
 *
 * Two rules are the whole contract, and this wapp exists to make both visible:
 *
 *   - A device that never volunteered holds nothing for anybody. Silence is not
 *     consent, so the quota starts at zero and the person has to move it.
 *   - A quota is a CEILING, not a target. Full is full.
 *
 * And the part a user must never be denied: knowing what is on their disk and
 * being able to get the space back. NOT as a list of files — an archive holds
 * hundreds of thousands of them, and scrolling that teaches a person nothing and
 * lets them do nothing. As STATISTICS (where the space went, whose it is, how
 * much of it has ever been asked for) plus CLEANUPS that say what they will free
 * before they free it. A cleanup tool that cannot tell you what it is about to
 * delete is not a tool, it is a gamble.
 *
 * Host HAL:
 *   hal_archive_status   → quota, used, items, policy switches
 *   hal_archive_items    → where the space went + previewed cleanups
 *   hal_archive_drop     → run a previewed cleanup, or evict one depositor
 *   hal_archive_set_pref → quotaGb, followed, fromLan/Bluetooth/Radio/WifiDirect
 *
 * Build: cd wapps/archiver && make
 */

#include "../hal/geogram_wasm_hal.h"

/* ── String helpers ──────────────────────────────────────────────────── */
static unsigned str_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int str_eq(const char *a, const char *b) { while (*a && *b && *a == *b) { a++; b++; } return *a == *b; }
static void str_copy(char *d, const char *s, unsigned m) { unsigned i = 0; while (i < m - 1 && s[i]) { d[i] = s[i]; i++; } d[i] = '\0'; }
static void str_cat(char *d, const char *s, unsigned m) { unsigned l = str_len(d); unsigned i = 0; while (l + i < m - 1 && s[i]) { d[l + i] = s[i]; i++; } d[l + i] = '\0'; }

static int str_to_int(const char *s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

static void int_to_str(int v, char *out, unsigned m) {
    char tmp[16];
    unsigned n = 0;
    if (v <= 0) { str_copy(out, "0", m); return; }
    while (v > 0 && n < sizeof(tmp)) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    unsigned o = 0;
    while (n > 0 && o < m - 1) out[o++] = tmp[--n];
    out[o] = '\0';
}

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
            else if (*p == ',' || *p == '}' || *p == ']') break;
            out[o++] = *p++;
        }
        out[o] = '\0';
        return 1;
    }
    return 0;
}

/* ── Buffers ─────────────────────────────────────────────────────────── */
static char g_status[2048];
static char g_items[24576];
static char g_msg[32768];

static void send_msg(const char *json) { hal_msg_send(json, str_len(json)); }

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

/* Quota + policy: the contract, in the numbers the owner chose. */
static void push_quota(void) {
    int n = hal_archive_status(g_status, sizeof(g_status) - 1);
    if (n <= 0) return;
    g_status[n] = '\0';

    char quota[16] = "0", used[24] = "0", items[16] = "0";
    char followed[8] = "true", lan[8] = "true", ble[8] = "true";
    char radio[8] = "true", wfd[8] = "true", direct[8] = "false";
    json_raw(g_status, "quotaGb", quota, sizeof(quota));
    json_raw(g_status, "usedBytes", used, sizeof(used));
    json_raw(g_status, "items", items, sizeof(items));
    json_raw(g_status, "followed", followed, sizeof(followed));
    json_raw(g_status, "fromLan", lan, sizeof(lan));
    json_raw(g_status, "fromBluetooth", ble, sizeof(ble));
    json_raw(g_status, "fromRadio", radio, sizeof(radio));
    json_raw(g_status, "fromWifiDirect", wfd, sizeof(wfd));
    json_raw(g_status, "directLinksActive", direct, sizeof(direct));

    int gb = str_to_int(quota);
    int on = gb > 0;
    int used_mb = str_to_int(used) / (1024 * 1024);

    str_copy(g_msg, "{\"type\":\"ui.people.set\",\"field\":\"quota\",\"sections\":[", sizeof(g_msg));

    /* The whole contract: one number. */
    str_cat(g_msg, "{\"title\":\"The offer\",\"items\":[", sizeof(g_msg));
    {
        char title[80], sub[192], mb[16];
        if (on) {
            str_copy(title, "Holding up to ", sizeof(title));
            str_cat(title, quota, sizeof(title));
            str_cat(title, " GB for other people", sizeof(title));
            int_to_str(used_mb, mb, sizeof(mb));
            str_copy(sub, "Using ", sizeof(sub));
            str_cat(sub, mb, sizeof(sub));
            str_cat(sub, " MB across ", sizeof(sub));
            str_cat(sub, items, sizeof(sub));
            str_cat(sub, " items. Full is full - nothing of yours is ever evicted for this.", sizeof(sub));
        } else {
            str_copy(title, "Not archiving - 0 GB", sizeof(title));
            str_copy(sub, "This device holds nothing for anybody. Tap to offer 5 GB.", sizeof(sub));
        }
        row(g_msg, sizeof(g_msg), "quota", title, sub, on);
    }
    str_cat(g_msg, ",", sizeof(g_msg));
    {
        char title[64];
        str_copy(title, "+5 GB / reset to 0", sizeof(title));
        row(g_msg, sizeof(g_msg), "quota_more", title,
            "Tap to raise the ceiling. Tap past 50 GB to go back to nothing.", 0);
    }
    str_cat(g_msg, "]}", sizeof(g_msg));

    /* What it takes. */
    str_cat(g_msg, ",{\"title\":\"What this device holds\",\"items\":[", sizeof(g_msg));
    {
        char title[80];
        str_copy(title, str_eq(followed, "true")
                            ? "Authors I follow: yes"
                            : "Authors I follow: no", sizeof(title));
        row(g_msg, sizeof(g_msg), "followed", title,
            "Redundancy for the people you already care about.",
            str_eq(followed, "true"));
    }
    str_cat(g_msg, "]}", sizeof(g_msg));

    /* The direct links: the peers with nowhere else to go. */
    str_cat(g_msg, ",{\"title\":\"Peers with nowhere else to go\",\"items\":[", sizeof(g_msg));
    {
        char sub[192];
        str_copy(sub, "Their data dies if you refuse it. Accepted on the strength of the link alone.", sizeof(sub));
        char title[96];
        str_copy(title, "LAN ", sizeof(title));
        str_cat(title, str_eq(lan, "true") ? "on" : "off", sizeof(title));
        str_cat(title, " - Bluetooth ", sizeof(title));
        str_cat(title, str_eq(ble, "true") ? "on" : "off", sizeof(title));
        str_cat(title, " - LoRa ", sizeof(title));
        str_cat(title, str_eq(radio, "true") ? "on" : "off", sizeof(title));
        str_cat(title, " - WiFi Direct ", sizeof(title));
        str_cat(title, str_eq(wfd, "true") ? "on" : "off", sizeof(title));
        row(g_msg, sizeof(g_msg), "links", title, sub, 1);
    }
    if (!str_eq(direct, "true")) {
        /* Said plainly rather than glossed: the switches are stored and honoured
         * by the policy, but the host cannot yet tell which interface a deposit
         * arrived on, so nothing can trigger them. A wapp that pretended
         * otherwise would be lying to the person who volunteered the disk. */
        str_cat(g_msg, ",", sizeof(g_msg));
        row(g_msg, sizeof(g_msg), "links_note",
            "Not active yet",
            "The host cannot yet see which link a deposit came in on, so these "
            "switches are saved but do not fire.", 0);
    }
    str_cat(g_msg, "]}]}", sizeof(g_msg));
    send_msg(g_msg);
}

/* Where the space went, and how to get it back.
 *
 * NOT a list of files. An archive holds hundreds of thousands of them; scrolling
 * that teaches a person nothing and lets them do nothing. What they need is the
 * breakdown (whose is it, is any of it even being used) and a cleanup that says
 * what it will free BEFORE it frees it. The host builds both. */
static void push_items(void) {
    int n = hal_archive_items(g_items, sizeof(g_items) - 1);
    if (n <= 0) return;
    g_items[n] = '\0';
    str_copy(g_msg, "{\"type\":\"ui.people.set\",\"field\":\"space\",\"sections\":", sizeof(g_msg));
    str_cat(g_msg, g_items, sizeof(g_msg));
    str_cat(g_msg, "}", sizeof(g_msg));
    send_msg(g_msg);
}

static void refresh(void) {
    push_quota();
    push_items();
}

static void set_pref(const char *kv) {
    hal_archive_set_pref(kv, str_len(kv));
    refresh();
}

/* Raise the ceiling in 5 GB steps; past 50 GB, go back to holding nothing —
 * because revoking must be at least as easy as granting. */
static void bump_quota(void) {
    int n = hal_archive_status(g_status, sizeof(g_status) - 1);
    if (n <= 0) return;
    g_status[n] = '\0';
    char q[16] = "0";
    json_raw(g_status, "quotaGb", q, sizeof(q));
    int gb = str_to_int(q) + 5;
    if (gb > 50) gb = 0;

    char kv[32], num[16];
    int_to_str(gb, num, sizeof(num));
    str_copy(kv, "quotaGb=", sizeof(kv));
    str_cat(kv, num, sizeof(kv));
    set_pref(kv);
}

static void toggle(const char *key) {
    int n = hal_archive_status(g_status, sizeof(g_status) - 1);
    if (n <= 0) return;
    g_status[n] = '\0';
    char cur[8] = "true";
    json_raw(g_status, key, cur, sizeof(cur));

    char kv[48];
    str_copy(kv, key, sizeof(kv));
    str_cat(kv, "=", sizeof(kv));
    str_cat(kv, str_eq(cur, "true") ? "0" : "1", sizeof(kv));
    set_pref(kv);
}

/* ── Module entry points ─────────────────────────────────────────────── */
int32_t module_init(void) {
    hal_log(6, "[archiver] up", 13);
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
    } else if (str_eq(cmd, "quota_tap")) {
        char id[32] = "";
        if (!json_raw(buf, "quota_id", id, sizeof(id))) return 0;
        if (str_eq(id, "quota") || str_eq(id, "quota_more")) bump_quota();
        else if (str_eq(id, "followed")) toggle("followed");
        else if (str_eq(id, "links")) toggle("fromLan");
    } else if (str_eq(cmd, "space_tap")) {
        /* A cleanup row, or one depositor. The row already said what it would
         * free, so tapping it does exactly that and nothing more. Rows starting
         * with '#' are statistics — they are information, not buttons. */
        char id[80] = "";
        if (json_raw(buf, "space_id", id, sizeof(id)) && id[0] && id[0] != '#') {
            hal_archive_drop(id, str_len(id));
            refresh();
        }
    }
    return 0;
}

int32_t module_tick_interval_ms(void) { return 5000; }

int32_t module_destroy(void) { return 0; }
