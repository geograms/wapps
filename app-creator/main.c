/*
 * tools.geogram.app-creator — in-app wapp authoring.
 *
 * Projects tab lists every installed wapp by asking the host for the
 * shared archive contents (wapps.list_installed). Picking a card
 * populates the metadata form via ui.set_field; New project clears
 * those same fields. Compile / Install / Run tests forward the form
 * state to the host using the existing compile/install/tests.run
 * outbox messages.
 *
 * Wire protocol — wapp → host:
 *   {"type":"wapps.list_installed","req_id":1}
 *   {"type":"ui.set_field","name":"<field>","value":"<v>"}
 *   {"type":"ui.data","target":"projects","items":[...]}
 *   {"type":"compile","source":"<escaped>"}
 *   {"type":"install","id":"...","name":"...",...}
 *   {"type":"tests.run","req_id":1,"target":"<wapp_id>"}
 *
 * Wire protocol — host → wapp:
 *   {"type":"wapps.list_installed.response","items":[...]}
 *   {"type":"action","action":"<name>"}
 *   {"type":"tests.case","suite":"...","name":"...","passed":true,...}
 *   {"type":"tests.complete","error":null}
 *
 * Build: WASI_SDK_PATH=$HOME/wasi-sdk make
 */

#include "../hal/geogram_wasm_hal.h"

/* ── Minimal string helpers (no libc) ─────────────────────────────── */

static unsigned str_len(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

static int str_eq_n(const char *a, const char *b, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static int find_substr(const char *hay, unsigned hlen, const char *needle) {
    unsigned nl = str_len(needle);
    if (nl == 0 || hlen < nl) return -1;
    for (unsigned i = 0; i + nl <= hlen; i++) {
        if (str_eq_n(hay + i, needle, nl)) return (int)i;
    }
    return -1;
}

static void append_range(char *dst, unsigned max, unsigned *pos,
                         const char *src, unsigned slen) {
    for (unsigned i = 0; i < slen && *pos + 1 < max; i++) {
        dst[(*pos)++] = src[i];
    }
}

static void append_cstr(char *dst, unsigned max, unsigned *pos, const char *s) {
    append_range(dst, max, pos, s, str_len(s));
}

/*
 * Find `"<key>":"...value..."` in [hay, hay+hlen) and copy the
 * still-JSON-escaped value into out. Re-embedding it in another JSON
 * message round-trips correctly. Returns bytes written, or -1 if the
 * key was not found. Always null-terminates.
 */
static int extract_json_string_field(
    const char *hay, unsigned hlen,
    const char *key,
    char *out, unsigned outmax
) {
    char token[64];
    unsigned tp = 0;
    token[tp++] = '"';
    const unsigned kl = str_len(key);
    for (unsigned i = 0; i < kl && tp + 3 < sizeof(token); i++) {
        token[tp++] = key[i];
    }
    token[tp++] = '"';
    token[tp++] = ':';
    token[tp++] = '"';
    token[tp] = '\0';

    const int found = find_substr(hay, hlen, token);
    if (found < 0) {
        if (outmax > 0) out[0] = '\0';
        return -1;
    }

    unsigned i = (unsigned)found + tp;
    unsigned op = 0;
    while (i < hlen && op + 1 < outmax) {
        const char c = hay[i];
        if (c == '\\' && i + 1 < hlen) {
            if (op + 2 >= outmax) break;
            out[op++] = c;
            out[op++] = hay[i + 1];
            i += 2;
        } else if (c == '"') {
            break;
        } else {
            out[op++] = c;
            i++;
        }
    }
    out[op] = '\0';
    return (int)op;
}

/* Match the matching close brace for an open at start. Tracks string
 * boundaries so braces inside JSON strings don't throw off the count. */
static int find_close_brace(const char *hay, unsigned hlen, unsigned start) {
    int depth = 0;
    int in_string = 0;
    for (unsigned i = start; i < hlen; i++) {
        const char c = hay[i];
        if (in_string) {
            if (c == '\\' && i + 1 < hlen) { i++; continue; }
            if (c == '"') in_string = 0;
            continue;
        }
        if (c == '"') in_string = 1;
        else if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) return (int)i;
        }
    }
    return -1;
}

/* ── Installed-wapps cache ───────────────────────────────────────────
 * The wapps.list_installed.response is parsed once per refresh and
 * stored here so action handlers can populate form fields without
 * re-asking the host. Field strings are kept in their JSON-escaped
 * form so they can be spliced back into outgoing JSON messages
 * verbatim. */

#define MAX_WAPPS 32
#define SLUG_LEN  64
#define TEXT_LEN  256
#define LONG_LEN  512

static char wapps_slug   [MAX_WAPPS][SLUG_LEN];
static char wapps_id     [MAX_WAPPS][TEXT_LEN];
static char wapps_title  [MAX_WAPPS][TEXT_LEN];
static char wapps_version[MAX_WAPPS][SLUG_LEN];
static char wapps_desc   [MAX_WAPPS][LONG_LEN];
static char wapps_summary[MAX_WAPPS][LONG_LEN];
static char wapps_icon   [MAX_WAPPS][TEXT_LEN];
static unsigned wapp_count = 0;

static void copy_field(const char *src, int slen, char *dst, unsigned dmax) {
    unsigned n = slen > 0 ? (unsigned)slen : 0;
    if (n >= dmax) n = dmax - 1;
    for (unsigned i = 0; i < n; i++) dst[i] = src[i];
    dst[n] = '\0';
}

/* Parse a wapps.list_installed response into the cache. The items
 * array is scanned object-by-object using brace matching. Each
 * object's fields are extracted via the same JSON-string helper used
 * elsewhere in the file. */
static void parse_list_response(const char *hay, unsigned hlen) {
    wapp_count = 0;
    const int items_at = find_substr(hay, hlen, "\"items\":[");
    if (items_at < 0) return;
    unsigned i = (unsigned)items_at + 9;
    while (i < hlen && wapp_count < MAX_WAPPS) {
        while (i < hlen && hay[i] != '{' && hay[i] != ']') i++;
        if (i >= hlen || hay[i] == ']') break;
        const unsigned obj_start = i;
        const int close_at = find_close_brace(hay, hlen, obj_start);
        if (close_at < 0) break;
        const unsigned obj_len = (unsigned)close_at - obj_start + 1;
        const char *obj = hay + obj_start;

        char tmp[LONG_LEN];
        int n;
        n = extract_json_string_field(obj, obj_len, "id", tmp, sizeof(tmp));
        copy_field(tmp, n, wapps_id[wapp_count], TEXT_LEN);
        n = extract_json_string_field(obj, obj_len, "name", tmp, sizeof(tmp));
        copy_field(tmp, n, wapps_slug[wapp_count], SLUG_LEN);
        n = extract_json_string_field(obj, obj_len, "title", tmp, sizeof(tmp));
        copy_field(tmp, n, wapps_title[wapp_count], TEXT_LEN);
        n = extract_json_string_field(obj, obj_len, "version", tmp, sizeof(tmp));
        copy_field(tmp, n, wapps_version[wapp_count], SLUG_LEN);
        n = extract_json_string_field(obj, obj_len, "description",
                                      tmp, sizeof(tmp));
        copy_field(tmp, n, wapps_desc[wapp_count], LONG_LEN);
        n = extract_json_string_field(obj, obj_len, "summary",
                                      tmp, sizeof(tmp));
        copy_field(tmp, n, wapps_summary[wapp_count], LONG_LEN);
        n = extract_json_string_field(obj, obj_len, "icon", tmp, sizeof(tmp));
        copy_field(tmp, n, wapps_icon[wapp_count], TEXT_LEN);

        wapp_count++;
        i = (unsigned)close_at + 1;
    }
}

static int find_wapp_by_slug(const char *slug, unsigned slen) {
    for (unsigned k = 0; k < wapp_count; k++) {
        const unsigned ml = str_len(wapps_slug[k]);
        if (ml != slen) continue;
        if (str_eq_n(wapps_slug[k], slug, slen)) return (int)k;
    }
    return -1;
}

/* ── Outbox helpers ───────────────────────────────────────────────── */

static void send_list_installed(void) {
    const char *m = "{\"type\":\"wapps.list_installed\",\"req_id\":1}";
    hal_msg_send(m, str_len(m));
}

/* Push a value into a named form field. The value is expected to be
 * already JSON-escaped (as it is when extracted from an inbox payload
 * via extract_json_string_field). */
static void send_set_field(const char *name, const char *value) {
    static char buf[2048];
    unsigned op = 0;
    append_cstr(buf, sizeof(buf), &op,
                "{\"type\":\"ui.set_field\",\"name\":\"");
    append_cstr(buf, sizeof(buf), &op, name);
    append_cstr(buf, sizeof(buf), &op, "\",\"value\":\"");
    append_cstr(buf, sizeof(buf), &op, value);
    append_cstr(buf, sizeof(buf), &op, "\"}");
    hal_msg_send(buf, op);
}

/* Render the cached wapps as cards in the projects group. Each card
 * carries a single "Edit" action whose name is "select:<slug>" so the
 * inbox handler knows which wapp to populate the form with. */
static void render_projects(void) {
    static char buf[16 * 1024];
    unsigned op = 0;
    append_cstr(buf, sizeof(buf), &op,
        "{\"type\":\"ui.data\",\"target\":\"projects\",\"items\":[");
    for (unsigned k = 0; k < wapp_count; k++) {
        if (k) append_cstr(buf, sizeof(buf), &op, ",");
        append_cstr(buf, sizeof(buf), &op, "{\"id\":\"");
        append_cstr(buf, sizeof(buf), &op, wapps_slug[k]);
        append_cstr(buf, sizeof(buf), &op, "\",\"title\":\"");
        if (wapps_title[k][0]) {
            append_cstr(buf, sizeof(buf), &op, wapps_title[k]);
        } else {
            append_cstr(buf, sizeof(buf), &op, wapps_slug[k]);
        }
        append_cstr(buf, sizeof(buf), &op, "\",\"subtitle\":\"v");
        append_cstr(buf, sizeof(buf), &op, wapps_version[k]);
        append_cstr(buf, sizeof(buf), &op, "\",\"description\":\"");
        if (wapps_summary[k][0]) {
            append_cstr(buf, sizeof(buf), &op, wapps_summary[k]);
        } else {
            append_cstr(buf, sizeof(buf), &op, wapps_desc[k]);
        }
        append_cstr(buf, sizeof(buf), &op, "\",\"icon_path\":\"wapp:");
        append_cstr(buf, sizeof(buf), &op, wapps_slug[k]);
        append_cstr(buf, sizeof(buf), &op,
            "\",\"actions\":[{\"name\":\"select:");
        append_cstr(buf, sizeof(buf), &op, wapps_slug[k]);
        append_cstr(buf, sizeof(buf), &op,
            "\",\"label\":\"Edit\",\"icon\":\"edit\"}]}");
    }
    append_cstr(buf, sizeof(buf), &op, "]}");
    hal_msg_send(buf, op);
}

/* ── Action dispatch ──────────────────────────────────────────────── */

static void on_select(const char *slug, unsigned slen) {
    const int idx = find_wapp_by_slug(slug, slen);
    if (idx < 0) return;
    send_set_field("wapp_title",       wapps_title[idx]);
    send_set_field("wapp_name",        wapps_slug[idx]);
    send_set_field("wapp_id",          wapps_id[idx]);
    send_set_field("wapp_version",     wapps_version[idx]);
    /* Prefer summary (longer) over description for the Description
     * field; fall back to description when summary is empty. */
    send_set_field("wapp_description",
                   wapps_summary[idx][0] ? wapps_summary[idx]
                                         : wapps_desc[idx]);
}

static void on_new_project(void) {
    send_set_field("wapp_title",       "");
    send_set_field("wapp_name",        "");
    send_set_field("wapp_id",          "");
    send_set_field("wapp_version",     "0.1.0");
    send_set_field("wapp_description", "");
}

/* Read the form state from KV (the host's binding layer mirrors form
 * fields into the wapp's KV automatically) and emit the host-side
 * compile / install / tests.run messages. */
static void do_compile(void) {
    static char source_buf[24 * 1024];
    static char out_buf[32 * 1024];
    uint32_t n = hal_kv_get("source", 6, source_buf, sizeof(source_buf) - 1);
    if (n == 0) {
        const char *err = "{\"type\":\"compile\",\"error\":\"no source\"}";
        hal_msg_send(err, str_len(err));
        return;
    }
    source_buf[n] = '\0';
    /* hal_kv_get returns the raw user-typed text (unescaped). The
     * host's compile handler expects the source field to be a JSON
     * string value, so we escape on the way out. */
    unsigned op = 0;
    append_cstr(out_buf, sizeof(out_buf), &op,
                "{\"type\":\"compile\",\"source\":\"");
    for (uint32_t i = 0; i < n && op + 8 < sizeof(out_buf); i++) {
        unsigned char c = (unsigned char)source_buf[i];
        if (c == '"' || c == '\\') {
            out_buf[op++] = '\\'; out_buf[op++] = (char)c;
        } else if (c == '\n') { out_buf[op++] = '\\'; out_buf[op++] = 'n'; }
        else if (c == '\r') { out_buf[op++] = '\\'; out_buf[op++] = 'r'; }
        else if (c == '\t') { out_buf[op++] = '\\'; out_buf[op++] = 't'; }
        else if (c < 0x20) { /* drop */ }
        else { out_buf[op++] = (char)c; }
    }
    append_cstr(out_buf, sizeof(out_buf), &op, "\"}");
    hal_msg_send(out_buf, op);
}

/* Read field at key into out, return bytes written (0 if missing).
 * Always null-terminates. */
static uint32_t kv_read(const char *key, char *out, uint32_t omax) {
    uint32_t n = hal_kv_get(key, str_len(key), out, omax - 1);
    out[n] = '\0';
    return n;
}

/* JSON-escape src into dst at *op. Used by do_install for fields that
 * came from KV (where they sit in raw form). */
static void escape_into(char *dst, unsigned dmax, unsigned *op,
                        const char *src, uint32_t slen) {
    for (uint32_t i = 0; i < slen && *op + 8 < dmax; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            dst[(*op)++] = '\\'; dst[(*op)++] = (char)c;
        } else if (c == '\n') { dst[(*op)++] = '\\'; dst[(*op)++] = 'n'; }
        else if (c == '\r') { dst[(*op)++] = '\\'; dst[(*op)++] = 'r'; }
        else if (c == '\t') { dst[(*op)++] = '\\'; dst[(*op)++] = 't'; }
        else if (c < 0x20) { /* drop */ }
        else { dst[(*op)++] = (char)c; }
    }
}

static void do_install(void) {
    static char src[24 * 1024];
    static char buf[1024];
    static char out_buf[32 * 1024];

    uint32_t id_n    = kv_read("wapp_id",          buf, sizeof(buf));
    if (id_n == 0) {
        const char *e = "{\"type\":\"install\",\"error\":\"no wapp_id\"}";
        hal_msg_send(e, str_len(e));
        return;
    }
    unsigned op = 0;
    append_cstr(out_buf, sizeof(out_buf), &op,
                "{\"type\":\"install\",\"id\":\"");
    escape_into(out_buf, sizeof(out_buf), &op, buf, id_n);

    append_cstr(out_buf, sizeof(out_buf), &op, "\",\"title\":\"");
    uint32_t n = kv_read("wapp_title", buf, sizeof(buf));
    escape_into(out_buf, sizeof(out_buf), &op, buf, n);

    append_cstr(out_buf, sizeof(out_buf), &op, "\",\"name\":\"");
    n = kv_read("wapp_name", buf, sizeof(buf));
    escape_into(out_buf, sizeof(out_buf), &op, buf, n);

    append_cstr(out_buf, sizeof(out_buf), &op, "\",\"description\":\"");
    n = kv_read("wapp_description", buf, sizeof(buf));
    escape_into(out_buf, sizeof(out_buf), &op, buf, n);

    append_cstr(out_buf, sizeof(out_buf), &op, "\",\"source_ui\":\"");
    n = hal_kv_get("source_ui", 9, src, sizeof(src) - 1);
    src[n] = '\0';
    escape_into(out_buf, sizeof(out_buf), &op, src, n);

    append_cstr(out_buf, sizeof(out_buf), &op, "\"}");
    hal_msg_send(out_buf, op);
}

static void do_run_tests(void) {
    static char id_buf[256];
    uint32_t n = kv_read("wapp_id", id_buf, sizeof(id_buf));
    if (n == 0) {
        const char *m = "{\"type\":\"ui.log.append\",\"name\":\"output\","
                        "\"text\":\"[tests] no wapp_id set — pick a "
                        "project first.\\n\"}";
        hal_msg_send(m, str_len(m));
        return;
    }
    static char buf[512];
    unsigned op = 0;
    append_cstr(buf, sizeof(buf), &op,
                "{\"type\":\"tests.run\",\"req_id\":1,\"target\":\"");
    append_range(buf, sizeof(buf), &op, id_buf, n);
    append_cstr(buf, sizeof(buf), &op, "\"}");
    hal_msg_send(buf, op);

    op = 0;
    append_cstr(buf, sizeof(buf), &op,
                "{\"type\":\"ui.log.append\",\"name\":\"output\","
                "\"text\":\"[tests] running tests in ");
    append_range(buf, sizeof(buf), &op, id_buf, n);
    append_cstr(buf, sizeof(buf), &op, "...\\n\"}");
    hal_msg_send(buf, op);
}

/* ── Module lifecycle ─────────────────────────────────────────────── */

void module_init(void) {
    hal_log(1, "[app-creator] init", 18);
    send_list_installed();
}

void module_tick(void) {
    /* Idle. The projects list refreshes only on explicit Refresh
     * presses or after Install completes (the user's next tab visit
     * triggers a fresh wapp boot). */
}

void module_destroy(void) {
    hal_log(1, "[app-creator] destroy", 21);
}

uint32_t module_tick_interval_ms(void) {
    return 0;
}

void module_handle_event(void) {
    static char inbox[32 * 1024];
    static char field_buf[512];
    static char out_buf[32 * 1024];

    while (hal_msg_available() != 0) {
        uint32_t n = hal_msg_recv(inbox, sizeof(inbox) - 1);
        if (n == 0) break;
        inbox[n] = '\0';

        /* Installed-wapps response → cache + render. */
        if (find_substr(inbox, n,
                "\"type\":\"wapps.list_installed.response\"") >= 0) {
            parse_list_response(inbox, n);
            render_projects();
            continue;
        }

        /* Test runner case results. */
        if (find_substr(inbox, n, "\"type\":\"tests.case\"") >= 0) {
            const int suite_len = extract_json_string_field(
                inbox, n, "suite", field_buf, sizeof(field_buf));
            char suite_copy[80];
            int sc = suite_len;
            if (sc > 79) sc = 79;
            for (int i = 0; i < sc; i++) suite_copy[i] = field_buf[i];
            suite_copy[sc < 0 ? 0 : sc] = '\0';

            const int name_len = extract_json_string_field(
                inbox, n, "name", field_buf, sizeof(field_buf));
            const int has_pass =
                find_substr(inbox, n, "\"passed\":true") >= 0;

            unsigned op = 0;
            append_cstr(out_buf, sizeof(out_buf), &op,
                        "{\"type\":\"ui.log.append\",\"name\":\"output\","
                        "\"text\":\"[tests] ");
            append_cstr(out_buf, sizeof(out_buf), &op,
                        has_pass ? "PASS  " : "FAIL  ");
            if (sc > 0) {
                append_range(out_buf, sizeof(out_buf), &op, suite_copy,
                             (unsigned)sc);
                append_cstr(out_buf, sizeof(out_buf), &op, ".");
            }
            if (name_len > 0) {
                append_range(out_buf, sizeof(out_buf), &op, field_buf,
                             (unsigned)name_len);
            }
            if (!has_pass) {
                const int err_len = extract_json_string_field(
                    inbox, n, "error", field_buf, sizeof(field_buf));
                if (err_len > 0) {
                    append_cstr(out_buf, sizeof(out_buf), &op,
                                "\\n         ");
                    append_range(out_buf, sizeof(out_buf), &op, field_buf,
                                 (unsigned)err_len);
                }
            }
            append_cstr(out_buf, sizeof(out_buf), &op, "\\n\"}");
            hal_msg_send(out_buf, op);
            continue;
        }

        if (find_substr(inbox, n, "\"type\":\"tests.complete\"") >= 0) {
            const int err_len = extract_json_string_field(
                inbox, n, "error", field_buf, sizeof(field_buf));
            unsigned op = 0;
            append_cstr(out_buf, sizeof(out_buf), &op,
                        "{\"type\":\"ui.log.append\",\"name\":\"output\","
                        "\"text\":\"[tests] complete");
            if (err_len > 0) {
                append_cstr(out_buf, sizeof(out_buf), &op, " — ");
                append_range(out_buf, sizeof(out_buf), &op, field_buf,
                             (unsigned)err_len);
            }
            append_cstr(out_buf, sizeof(out_buf), &op, "\\n\"}");
            hal_msg_send(out_buf, op);
            continue;
        }

        /* Action messages — extract action name, then dispatch. */
        const int act_idx = find_substr(inbox, n, "\"action\":\"");
        if (act_idx < 0) continue;
        const unsigned vstart = (unsigned)act_idx + 10;
        unsigned vend = vstart;
        while (vend < n && inbox[vend] != '"') vend++;
        if (vend >= n) continue;
        const char *a = inbox + vstart;
        const unsigned al = vend - vstart;

        /* Card-emitted "select:<slug>" actions — populate the form
         * from the cached wapp metadata. */
        if (al > 7 && str_eq_n(a, "select:", 7)) {
            on_select(a + 7, al - 7);
            continue;
        }

        if (al == 11 && str_eq_n(a, "new-project", 11)) {
            on_new_project();
            continue;
        }
        if (al == 16 && str_eq_n(a, "refresh-projects", 16)) {
            send_list_installed();
            continue;
        }
        if (al == 7 && str_eq_n(a, "compile", 7)) {
            do_compile();
            continue;
        }
        if (al == 7 && str_eq_n(a, "install", 7)) {
            do_install();
            continue;
        }
        if (al == 9 && str_eq_n(a, "run-tests", 9)) {
            do_run_tests();
            continue;
        }
    }
}
