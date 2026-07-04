/*
 * nostr — a NOSTR client wapp, laid out like the Chat wapp.
 *
 *   Activity  ($type:"chat")          kind-1 notes from the accounts you follow
 *   Messages  ($type:"conversations") kind-4 encrypted DMs, per-peer threads
 *   Follows   ($type:"people")        who you follow (+ add / tap-to-unfollow)
 *   NOSTR servers (menu panel)        relay list + reachability + add / remove
 *
 * All relay/crypto/signing/decryption is host-side via hal.nostr; the transport
 * of each relay (wss:// internet, rns:// Reticulum, local device) is invisible
 * here. This module just drives the UI.
 *
 * Build: cd wapps/nostr && WASI_SDK_PATH=~/wasi-sdk make
 */
#include "../hal/geogram_wasm_hal.h"

/* ── String helpers ──────────────────────────────────────────────────── */
static unsigned str_len(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int str_eq(const char *a, const char *b) { while (*a && *b && *a == *b) { a++; b++; } return *a == *b; }
static void str_copy(char *d, const char *s, unsigned m) { unsigned i = 0; while (i < m - 1 && s[i]) { d[i] = s[i]; i++; } d[i] = '\0'; }
static void str_cat(char *d, const char *s, unsigned m) { unsigned l = str_len(d); unsigned i = 0; while (l + i < m - 1 && s[i]) { d[l + i] = s[i]; i++; } d[l + i] = '\0'; }

/* Find "key":<value> in flat JSON; copy the raw value. Escape-aware inside
 * strings so a note with a quote is not truncated. */
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

/* Grab the value of the first ["p","<value>"] tag in an event's tags array. */
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

/* Append raw string as JSON string body (escaped). */
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
static void short12(const char *hex, char *out) { str_copy(out, hex, 13); }

/* "HH:MM" (UTC) from a unix-seconds string. */
static void fmt_hhmm(const char *unix_s, char *out) {
    long v = 0;
    for (const char *p = unix_s; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
    int hh = (int)((v / 3600) % 24), mm = (int)((v / 60) % 60);
    out[0] = '0' + hh / 10; out[1] = '0' + hh % 10; out[2] = ':';
    out[3] = '0' + mm / 10; out[4] = '0' + mm % 10; out[5] = '\0';
}

/* Append the display-time fields the host feed wants: "time" (HH:MM clock) +
 * "t" (absolute epoch, MILLISECONDS) so older posts get a date prefix. */
static void cat_time_fields(char *dst, const char *ts, unsigned m) {
    str_cat(dst, "\"time\":\"", m);
    if (ts[0]) { char hm[8]; fmt_hhmm(ts, hm); str_cat(dst, hm, m); }
    str_cat(dst, "\",\"t\":", m);
    str_cat(dst, ts[0] ? ts : "0", m);
    str_cat(dst, "000", m); /* seconds → ms */
}

/* ── State ───────────────────────────────────────────────────────────── */
static char g_self[80] = "";       /* our x-only pubkey (hex)              */
static char g_sub_feed[64] = "";   /* kind-1 subscription id               */
static char g_sub_dm[64] = "";     /* kind-4 subscription id               */
static char g_evt[8192];           /* one drained event JSON               */
static char g_relays[8192];        /* hal_nostr_relays output              */
static char g_msg[16384];          /* outbound UI message                  */
static char g_follows[4096];       /* followed pubkeys JSON array          */
static char g_wot[48128];          /* web-of-trust author set JSON         */
static char g_feedfilter[52224];   /* built kind-1 WoT filter              */
static char g_plain[6000];         /* decrypted DM plaintext               */
static char g_pids[96][66];        /* recent post ids (ring) for stats     */
static int  g_npids = 0;
static char g_track[7168];         /* built ids JSON array for tracking    */
static char g_stat[128];           /* one hal_nostr_stats result           */
static int  g_ticks = 0;

/* ── Subscriptions ───────────────────────────────────────────────────── */
static void subscribe_all(void) {
    if (!g_self[0]) {
        int sn = hal_nostr_self(g_self, sizeof(g_self) - 1);
        if (sn > 0) g_self[sn] = '\0';
    }
    if (!g_sub_feed[0]) {
        // Web of trust, not the firehose: kind-1 from follows + followers +
        // follows-of-follows. Only fall back to a small global sample when the
        // trust set is still empty (brand-new user with no follows).
        int wn = hal_nostr_wot(g_wot, sizeof(g_wot) - 1);
        if (wn > 0) g_wot[wn] = '\0'; else str_copy(g_wot, "[]", sizeof(g_wot));
        if (str_len(g_wot) > 2) {
            /* Have a trust graph: subscribe kind-1 from it (spam-free). */
            str_copy(g_feedfilter, "{\"kinds\":[1],\"authors\":", sizeof(g_feedfilter));
            str_cat(g_feedfilter, g_wot, sizeof(g_feedfilter));
            str_cat(g_feedfilter, ",\"limit\":200}", sizeof(g_feedfilter));
            int n = hal_nostr_subscribe(g_feedfilter, str_len(g_feedfilter),
                                        g_sub_feed, sizeof(g_sub_feed) - 1);
            if (n > 0) g_sub_feed[n] = '\0';
        } else {
            /* Follow nobody yet: discovery feed — only posts with >2 likes,
             * so a new user sees quality, not the raw firehose of spam. */
            int n = hal_nostr_discovery(g_sub_feed, sizeof(g_sub_feed) - 1);
            if (n > 0) g_sub_feed[n] = '\0';
        }
    }
    if (!g_sub_dm[0] && g_self[0]) {
        /* DMs to us (#p=self) AND our own sent DMs (authors=self). */
        char filter[256];
        str_copy(filter, "[{\"kinds\":[4],\"#p\":[\"", sizeof(filter));
        str_cat(filter, g_self, sizeof(filter));
        str_cat(filter, "\"]},{\"kinds\":[4],\"authors\":[\"", sizeof(filter));
        str_cat(filter, g_self, sizeof(filter));
        str_cat(filter, "\"]}]", sizeof(filter));
        int n = hal_nostr_subscribe(filter, str_len(filter), g_sub_dm, sizeof(g_sub_dm) - 1);
        if (n > 0) g_sub_dm[n] = '\0';
    }
}

/* ── Activity feed ───────────────────────────────────────────────────── */
static void feed_append(const char *evt) {
    char pubkey[80] = "", content[6000] = "", ts[24] = "", id[80] = "";
    json_raw(evt, "pubkey", pubkey, sizeof(pubkey));
    json_raw(evt, "content", content, sizeof(content)); /* still escaped */
    json_raw(evt, "created_at", ts, sizeof(ts));
    json_raw(evt, "id", id, sizeof(id));
    if (!content[0]) return;
    if (id[0]) { str_copy(g_pids[g_npids % 96], id, 66); g_npids++; } /* track */
    char from[16] = ""; short12(pubkey, from);
    str_copy(g_msg, "{\"type\":\"ui.chat.append\",\"field\":\"activity\",\"message\":{\"dir\":\"in\",\"from\":\"", sizeof(g_msg));
    str_cat(g_msg, from, sizeof(g_msg));
    str_cat(g_msg, "\",\"text\":\"", sizeof(g_msg));
    str_cat(g_msg, content, sizeof(g_msg));      /* already-escaped body */
    /* The event id becomes the post's mid so the host can count likes/replies
     * and attach a reaction to it. */
    str_cat(g_msg, "\",\"mid\":\"", sizeof(g_msg));
    str_cat(g_msg, id, sizeof(g_msg));
    str_cat(g_msg, "\",", sizeof(g_msg));
    cat_time_fields(g_msg, ts, sizeof(g_msg));
    str_cat(g_msg, "}}", sizeof(g_msg));
    send_msg(g_msg);
}

/* ── Direct messages (kind-4) ────────────────────────────────────────── */
static void convo_upsert(const char *peer, const char *title, const char *preview) {
    str_copy(g_msg, "{\"type\":\"ui.convo.upsert\",\"id\":\"", sizeof(g_msg));
    json_escape_cat(g_msg, peer, sizeof(g_msg));
    str_cat(g_msg, "\",\"title\":\"", sizeof(g_msg));
    json_escape_cat(g_msg, title, sizeof(g_msg));
    str_cat(g_msg, "\",\"subtitle\":\"", sizeof(g_msg));
    json_escape_cat(g_msg, preview, sizeof(g_msg));
    str_cat(g_msg, "\",\"icon\":\"person\",\"bump\":true}", sizeof(g_msg));
    send_msg(g_msg);
}

static void convo_msg(const char *peer, const char *dir, const char *from,
                      const char *text, const char *mid, const char *ts) {
    str_copy(g_msg, "{\"type\":\"ui.convo.msg\",\"id\":\"", sizeof(g_msg));
    json_escape_cat(g_msg, peer, sizeof(g_msg));
    str_cat(g_msg, "\",\"dir\":\"", sizeof(g_msg)); str_cat(g_msg, dir, sizeof(g_msg));
    str_cat(g_msg, "\",\"from\":\"", sizeof(g_msg)); json_escape_cat(g_msg, from, sizeof(g_msg));
    str_cat(g_msg, "\",\"text\":\"", sizeof(g_msg)); json_escape_cat(g_msg, text, sizeof(g_msg));
    str_cat(g_msg, "\",\"key\":\"\",\"meta\":\"\",\"mid\":\"", sizeof(g_msg));
    str_cat(g_msg, mid, sizeof(g_msg));
    str_cat(g_msg, "\",", sizeof(g_msg));
    cat_time_fields(g_msg, ts, sizeof(g_msg));
    str_cat(g_msg, "}", sizeof(g_msg));
    send_msg(g_msg);
}

static void dm_ingest(const char *evt) {
    char sender[80] = "", content[6000] = "", ts[24] = "", id[80] = "";
    json_raw(evt, "pubkey", sender, sizeof(sender));
    json_raw(evt, "content", content, sizeof(content)); /* NOTE: escaped */
    json_raw(evt, "created_at", ts, sizeof(ts));
    json_raw(evt, "id", id, sizeof(id));
    if (!sender[0] || !content[0]) return;

    int mine = str_eq(sender, g_self);
    char peer[80];
    if (mine) { if (!find_p_tag(evt, peer, sizeof(peer))) return; } /* recipient */
    else str_copy(peer, sender, sizeof(peer));

    /* Decrypt with the OTHER party's pubkey (host uses our profile key). */
    int pn = hal_nostr_dm_decrypt(peer, str_len(peer), content, str_len(content),
                                  g_plain, sizeof(g_plain) - 1);
    if (pn <= 0) return;
    g_plain[pn] = '\0';

    char title[16]; short12(peer, title);
    convo_upsert(peer, title, g_plain);
    convo_msg(peer, mine ? "out" : "in", mine ? "me" : title, g_plain, id, ts);
}

static void drain(void) {
    for (int i = 0; i < 20 && g_sub_feed[0]; i++) {
        int n = hal_nostr_event_recv(g_sub_feed, str_len(g_sub_feed), g_evt, sizeof(g_evt) - 1);
        if (n <= 0) break;
        g_evt[n] = '\0'; feed_append(g_evt);
    }
    for (int i = 0; i < 20 && g_sub_dm[0]; i++) {
        int n = hal_nostr_event_recv(g_sub_dm, str_len(g_sub_dm), g_evt, sizeof(g_evt) - 1);
        if (n <= 0) break;
        g_evt[n] = '\0'; dm_ingest(g_evt);
    }
}

/* ── Follows list ────────────────────────────────────────────────────── */
static void push_follows(void) {
    int fn = hal_nostr_follows(g_follows, sizeof(g_follows) - 1);
    if (fn > 0) g_follows[fn] = '\0'; else str_copy(g_follows, "[]", sizeof(g_follows));
    str_copy(g_msg, "{\"type\":\"ui.people.set\",\"field\":\"follows_list\",\"sections\":[{\"title\":\"Following\",\"items\":[", sizeof(g_msg));
    int first = 1;
    for (char *p = g_follows; *p; p++) {
        if (*p != '"') continue;
        char hex[80] = ""; unsigned o = 0; p++;
        while (*p && *p != '"' && o < sizeof(hex) - 1) hex[o++] = *p++;
        hex[o] = '\0';
        if (o < 32) continue;   /* skip non-key tokens */
        char title[16]; short12(hex, title);
        if (!first) str_cat(g_msg, ",", sizeof(g_msg));
        first = 0;
        str_cat(g_msg, "{\"id\":\"", sizeof(g_msg)); str_cat(g_msg, hex, sizeof(g_msg));
        str_cat(g_msg, "\",\"title\":\"", sizeof(g_msg)); str_cat(g_msg, title, sizeof(g_msg));
        str_cat(g_msg, "…\",\"subtitle\":\"tap to unfollow\"}", sizeof(g_msg));
    }
    str_cat(g_msg, "]}]}", sizeof(g_msg));
    send_msg(g_msg);
}

/* ── Relay panel ─────────────────────────────────────────────────────── */
static void push_relays(void) {
    int n = hal_nostr_relays(g_relays, sizeof(g_relays) - 1);
    if (n <= 0) g_relays[0] = '\0'; else g_relays[n] = '\0';
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
        str_cat(g_msg, "{\"id\":\"", sizeof(g_msg)); json_escape_cat(g_msg, uri, sizeof(g_msg));
        str_cat(g_msg, "\",\"title\":\"", sizeof(g_msg)); json_escape_cat(g_msg, uri, sizeof(g_msg));
        str_cat(g_msg, "\",\"subtitle\":\"", sizeof(g_msg)); str_cat(g_msg, scheme, sizeof(g_msg));
        str_cat(g_msg, "\",\"tags\":[\"", sizeof(g_msg)); str_cat(g_msg, status[0] ? status : "?", sizeof(g_msg));
        str_cat(g_msg, "\"]}", sizeof(g_msg));
        while (*p && *p != '}') p++;
        if (!*p) break;
    }
    str_cat(g_msg, "]}]}", sizeof(g_msg));
    send_msg(g_msg);
}

/* ── Engagement (likes/replies) ──────────────────────────────────────── */
/* Ask the host to count reactions/replies for the posts on screen, then push
 * the counts back as generic ui.activity.stats messages the feed renders. */
static void push_stats(void) {
    int valid = g_npids < 96 ? g_npids : 96;
    if (valid == 0) return;
    str_copy(g_track, "[", sizeof(g_track));
    for (int i = 0; i < valid; i++) {
        if (i) str_cat(g_track, ",", sizeof(g_track));
        str_cat(g_track, "\"", sizeof(g_track));
        str_cat(g_track, g_pids[i], sizeof(g_track));
        str_cat(g_track, "\"", sizeof(g_track));
    }
    str_cat(g_track, "]", sizeof(g_track));
    hal_nostr_track(g_track, str_len(g_track));

    for (int i = 0; i < valid; i++) {
        int n = hal_nostr_stats(g_pids[i], str_len(g_pids[i]), g_stat, sizeof(g_stat) - 1);
        if (n <= 0) continue;
        g_stat[n] = '\0';
        char likes[12] = "", replies[12] = "", mine[8] = "";
        json_raw(g_stat, "likes", likes, sizeof(likes));
        json_raw(g_stat, "replies", replies, sizeof(replies));
        json_raw(g_stat, "mine", mine, sizeof(mine));
        int noLikes = (!likes[0] || (likes[0] == '0' && !likes[1]));
        int noReplies = (!replies[0] || (replies[0] == '0' && !replies[1]));
        if (noLikes && noReplies) continue; /* skip zero-engagement (default) */
        str_copy(g_msg, "{\"type\":\"ui.activity.stats\",\"mid\":\"", sizeof(g_msg));
        str_cat(g_msg, g_pids[i], sizeof(g_msg));
        str_cat(g_msg, "\",\"likes\":", sizeof(g_msg));
        str_cat(g_msg, likes[0] ? likes : "0", sizeof(g_msg));
        str_cat(g_msg, ",\"replies\":", sizeof(g_msg));
        str_cat(g_msg, replies[0] ? replies : "0", sizeof(g_msg));
        str_cat(g_msg, ",\"mine\":", sizeof(g_msg));
        str_cat(g_msg, (mine[0] == 't') ? "true" : "false", sizeof(g_msg));
        str_cat(g_msg, "}", sizeof(g_msg));
        send_msg(g_msg);
    }
}

/* ── Module entry points ─────────────────────────────────────────────── */
int32_t module_init(void) {
    hal_log(6, "[nostr] up", 10);
    subscribe_all();
    push_relays();
    push_follows();
    return 0;
}

int32_t module_tick(void) {
    subscribe_all();
    drain();
    g_ticks++;
    // The web-of-trust set grows as kind-3 contact lists arrive, so re-subscribe
    // the feed a couple of times early to pick up follows-of-follows.
    if (g_ticks == 10 || g_ticks == 30) { g_sub_feed[0] = '\0'; subscribe_all(); }
    if (g_ticks % 8 == 0) push_relays();
    if (g_ticks % 5 == 0) push_stats();   /* refresh like/reply counts */
    return 0;
}

int32_t module_handle_event(void) {
    static char buf[6144];
    uint32_t n = hal_msg_recv(buf, sizeof(buf) - 1);
    if (n == 0) return 0;
    buf[n] = '\0';
    char cmd[64] = "";
    if (!json_raw(buf, "command", cmd, sizeof(cmd))) return 0;

    if (str_eq(cmd, "ready") || str_eq(cmd, "refresh")) {
        subscribe_all(); push_relays(); push_follows();
    } else if (str_eq(cmd, "activity_send")) {
        char text[6000] = "";
        if (json_raw(buf, "activity_input", text, sizeof(text)) && text[0])
            hal_nostr_post(1, text, str_len(text), "[]", 2);
    } else if (str_eq(cmd, "clear_feed")) {
        send_msg("{\"type\":\"ui.chat.clear\",\"field\":\"activity\"}");
    } else if (str_eq(cmd, "activity_like")) {
        char mid[80] = "";
        if (json_raw(buf, "activity_mid", mid, sizeof(mid)) && mid[0]) {
            hal_nostr_react(mid, str_len(mid));
            push_stats(); /* reflect the new like immediately */
        }
    } else if (str_eq(cmd, "conversations_send")) {
        char peer[80] = "", text[6000] = "";
        json_raw(buf, "conversations_convo", peer, sizeof(peer));
        json_raw(buf, "conversations_input", text, sizeof(text));
        if (peer[0] && text[0]) {
            hal_nostr_dm_send(peer, str_len(peer), text, str_len(text));
            char title[16]; short12(peer, title);
            convo_msg(peer, "out", "me", text, "", "0"); /* local echo */
        }
    } else if (str_eq(cmd, "follow_add")) {
        char key[128] = "";
        if (json_raw(buf, "follow_input", key, sizeof(key)) && key[0]) {
            hal_nostr_follow(key, str_len(key));
            g_sub_feed[0] = '\0';  /* re-subscribe with the new author set */
            subscribe_all(); push_follows();
        }
    } else if (str_eq(cmd, "follows_list_tap") || str_eq(cmd, "follows_list")) {
        char key[128] = "";
        if (json_raw(buf, "follows_list_id", key, sizeof(key)) && key[0]) {
            hal_nostr_unfollow(key, str_len(key));
            g_sub_feed[0] = '\0';
            subscribe_all(); push_follows();
        }
    } else if (str_eq(cmd, "dm_send")) {
        char to[128] = "", text[6000] = "";
        json_raw(buf, "dm_to", to, sizeof(to));
        json_raw(buf, "dm_text", text, sizeof(text));
        if (to[0] && text[0]) {
            hal_nostr_dm_send(to, str_len(to), text, str_len(text));
            char title[16]; short12(to, title);   /* local echo → Messages */
            convo_upsert(to, title, text);
            convo_msg(to, "out", "me", text, "", "0");
        }
    } else if (str_eq(cmd, "relay_add")) {
        char uri[256] = "";
        if (json_raw(buf, "new_relay", uri, sizeof(uri)) && uri[0]) {
            hal_nostr_relay_add(uri, str_len(uri)); push_relays();
        }
    } else if (str_eq(cmd, "relays_tap") || str_eq(cmd, "relays")) {
        char uri[256] = "";
        if (json_raw(buf, "relays_id", uri, sizeof(uri)) && uri[0]) {
            hal_nostr_relay_remove(uri, str_len(uri)); push_relays();
        }
    }
    return 0;
}

int32_t module_tick_interval_ms(void) { return 1500; }

void module_destroy(void) {}
