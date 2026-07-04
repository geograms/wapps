/*
 * Geogram WASM HAL — Hardware Abstraction Layer
 *
 * Modules #include this header and call hal_* functions normally.
 * Each host platform (ESP32/Wasm3, Flutter/Wasmer, CLI/Wasmtime) provides
 * its own implementation of these imports.
 *
 * Buffer exchanges use (ptr, len) pairs into WASM linear memory.
 * Functions that need hardware return sentinel values when the capability
 * is absent — modules should check *_available_hw() before using.
 *
 * Copyright (c) geogram — Apache-2.0
 */

#ifndef GEOGRAM_WASM_HAL_H
#define GEOGRAM_WASM_HAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── System ─────────────────────────────────────────────────────────── */

/* Monotonic milliseconds since host start */
__attribute__((import_module("hal"), import_name("time_ms")))
uint64_t hal_time_ms(void);

/* Unix epoch seconds (0 if no RTC) */
__attribute__((import_module("hal"), import_name("time_epoch")))
uint64_t hal_time_epoch(void);

/* Log a message. level: 0=debug, 1=info, 2=warn, 3=error */
__attribute__((import_module("hal"), import_name("log")))
void hal_log(int32_t level, const char *msg, uint32_t msg_len);

/* Yield to host scheduler (cooperative multitasking on ESP32) */
__attribute__((import_module("hal"), import_name("yield")))
void hal_yield(void);

/* Platform identifier written into buf. Returns bytes written. */
/* Platforms: "esp32", "android", "linux-desktop", "linux-cli" */
__attribute__((import_module("hal"), import_name("platform")))
uint32_t hal_platform(char *buf, uint32_t buf_len);

/* This device's identity (the active profile's callsign) written into buf.
 * Returns bytes written (0 if none). Use it as the default callsign instead
 * of hardcoding one, so each device transmits as itself. */
__attribute__((import_module("hal"), import_name("identity")))
uint32_t hal_identity(char *buf, uint32_t buf_len);

/* This device's public key — the active profile's Nostr public key in bech32
 * form ("npub1..."), written into buf. Returns bytes written (0 if none).
 * Lets a wapp publish its identity key so peers can map callsign -> pubkey
 * (e.g. to later send it encrypted messages). */
__attribute__((import_module("hal"), import_name("identity_pubkey")))
uint32_t hal_identity_pubkey(char *buf, uint32_t buf_len);

/* Sign [msg_len] bytes with THIS device's private key and write the signature,
 * as a compact ASCII string, into out_buf. The private key never leaves the
 * host. Returns bytes written (0 if no key / no room). The scheme is APRX
 * short-Schnorr over secp256k1: a 48-byte signature, base85-encoded (60 chars).
 * Verify with hal_verify against the signer's hal_identity_pubkey. */
__attribute__((import_module("hal"), import_name("identity_sign")))
uint32_t hal_identity_sign(const char *msg, uint32_t msg_len,
                           char *out_buf, uint32_t out_len);

/* Verify a signature produced by hal_identity_sign. [pubkey] is the signer's
 * public key string exactly as hal_identity_pubkey returns it (base64url of the
 * 32-byte key); [msg] is the signed bytes; [sig] is the base85 signature string.
 * Returns 1 if valid, 0 otherwise. */
__attribute__((import_module("hal"), import_name("verify")))
uint32_t hal_verify(const char *pubkey, uint32_t pubkey_len,
                    const char *msg, uint32_t msg_len,
                    const char *sig, uint32_t sig_len);

/* Encode a public key (base64url of the 32 raw bytes, as hal_identity_pubkey
 * emits) to its npub bech32 string ("npub1..."), written into out_buf. Returns
 * bytes written (0 on error). Lets a wapp display the familiar npub form. */
__attribute__((import_module("hal"), import_name("npub")))
uint32_t hal_npub(const char *pubkey, uint32_t pubkey_len,
                  char *out_buf, uint32_t out_len);

/* Encrypt [msg] for [pubkey] (base64url of the 32 raw bytes) with THIS device's
 * private key (ECDH + AES-256-CBC). Writes base64url(iv||ciphertext) into
 * out_buf. Returns bytes written (0 on error). Decrypt with hal_decrypt on the
 * peer using the sender's pubkey. The private key never leaves the host. */
__attribute__((import_module("hal"), import_name("encrypt")))
uint32_t hal_encrypt(const char *pubkey, uint32_t pubkey_len,
                     const char *msg, uint32_t msg_len,
                     char *out_buf, uint32_t out_len);

/* Decrypt a base64url blob (from hal_encrypt) sent by [pubkey] (base64url),
 * using this device's private key. Writes the plaintext into out_buf. Returns
 * bytes written (0 on failure / not for us). */
__attribute__((import_module("hal"), import_name("decrypt")))
uint32_t hal_decrypt(const char *pubkey, uint32_t pubkey_len,
                     const char *blob, uint32_t blob_len,
                     char *out_buf, uint32_t out_len);

/* ── Media archive + decentralized sharing (APRX §16 / Files wapp) ─────
 * The host keeps a device-wide content-addressed archive (sha256 → bytes,
 * media.sqlite3) plus a Blossom-compatible HTTP provider endpoint and a
 * BitTorrent seeder. Hashes are accepted as a full "file:<sha256>.<ext>"
 * token, a bare 43-char base64url digest, or 64-char hex. All JSON I/O. */

/* Page of archive metadata (newest first) as a JSON array. Returns bytes
 * written (0 = none/unavailable). */
__attribute__((import_module("hal"), import_name("media_list")))
uint32_t hal_media_list(int32_t offset, int32_t limit,
                        char *out_buf, uint32_t out_len);

/* One entry's metadata as JSON. 0 = unknown hash. */
__attribute__((import_module("hal"), import_name("media_meta")))
uint32_t hal_media_meta(const char *hash, uint32_t hash_len,
                        char *out_buf, uint32_t out_len);

/* Import a host file (absolute path, e.g. from a file.pick result) into the
 * archive. Writes the wire token "file:<sha256>.<ext>". 0 = failure. */
__attribute__((import_module("hal"), import_name("media_put_file")))
uint32_t hal_media_put_file(const char *path, uint32_t path_len,
                            char *out_buf, uint32_t out_len);

/* Update {"name","description","tags":[..]} (absent keys unchanged). */
__attribute__((import_module("hal"), import_name("media_set_meta")))
uint32_t hal_media_set_meta(const char *hash, uint32_t hash_len,
                            const char *json, uint32_t json_len);

__attribute__((import_module("hal"), import_name("media_delete")))
uint32_t hal_media_delete(const char *hash, uint32_t hash_len);

/* {"count":n,"bytes":n,"screenshots":n} */
__attribute__((import_module("hal"), import_name("media_stats")))
uint32_t hal_media_stats(char *out_buf, uint32_t out_len);

/* Search the archive. An exact sha256 (token / 43-char base64url / 64-hex)
 * returns that single entry; any other text is a full-text query over
 * name/description/tags/folder/parent. → JSON array of metadata objects. */
__attribute__((import_module("hal"), import_name("media_search")))
uint32_t hal_media_search(const char *query, uint32_t query_len,
                          char *out_buf, uint32_t out_len);

/* Virtual-folder tree: JSON array of {"parent","folder","count"}. */
__attribute__((import_module("hal"), import_name("media_folders")))
uint32_t hal_media_folders(char *out_buf, uint32_t out_len);

/* Files inside one virtual folder. Input JSON {"parent":..,"folder":..}
 * (empty strings = uncategorized). → JSON array of metadata objects. */
__attribute__((import_module("hal"), import_name("media_list_folder")))
uint32_t hal_media_list_folder(const char *json, uint32_t json_len,
                               char *out_buf, uint32_t out_len);

/* ── Mutable folders (IPNS-like; folder = secp256k1 identity on the relay) ──
 * Create a folder: input {"name":..,"desc":..} → folderId hex written to out. */
__attribute__((import_module("hal"), import_name("folder_create")))
uint32_t hal_folder_create(const char *json, uint32_t json_len,
                           char *out_buf, uint32_t out_len);

/* Owned folders → JSON array of {"folderId","npub","name"}. */
__attribute__((import_module("hal"), import_name("folder_list")))
uint32_t hal_folder_list(char *out_buf, uint32_t out_len);

/* Apply an edit to a folder. op JSON is one of:
 *   {"op":"addFile","x":<sha256hex>,"name":..,"desc":..,"mime":..,"size":..}
 *   {"op":"rmFile","x":<sha256hex>} / {"op":"setMeta","name":..,"desc":..}
 *   {"op":"link","f":<folderId>,"name":..} / {"op":"unlink","f":<folderId>}
 *   {"op":"grant","p":<npub-hex>,"role":..} / {"op":"revoke","p":<npub-hex>}
 * Asynchronous: returns 1 when the edit was accepted for publishing. */
__attribute__((import_module("hal"), import_name("folder_edit")))
uint32_t hal_folder_edit(const char *folder_id, uint32_t id_len,
                         const char *json, uint32_t json_len);

/* Browse a folder by id → its cached FolderState JSON {folderId,name,desc,
 * files[],links[],admins[]}. Triggers a background refresh; poll again for
 * fresh data (empty/partial on the first call). */
__attribute__((import_module("hal"), import_name("folder_browse")))
uint32_t hal_folder_browse(const char *folder_id, uint32_t id_len,
                           char *out_buf, uint32_t out_len);

/* Browse ONE directory level: pass "folder_id\tsubpath" as folder_id (subpath
 * "" = root, else ends with '/'). Returns JSON {name, npub, path, dirs:[{name}],
 * files:[{x,name,base,size,ts,dl}], links}. Keeps payload + work flat for huge
 * folders (only the immediate children of that path). */

/* Folder info + serve statistics for the info panel → JSON
 * {npub, name, owned, fileCount, totalBytes, serves, last24h, last7d, last30d,
 *  activeDays, top:[{name,serves}]}. */
__attribute__((import_module("hal"), import_name("folder_stats")))
uint32_t hal_folder_stats(const char *folder_id, uint32_t id_len,
                          char *out_buf, uint32_t out_len);

/* Stop sharing an owned disk folder (its on-disk files are left untouched).
 * Returns 1 when accepted. */
__attribute__((import_module("hal"), import_name("folder_remove")))
uint32_t hal_folder_remove(const char *folder_id, uint32_t id_len);

/* Open an owned disk folder's directory in the OS file manager so the user can
 * edit its files directly. Returns 1 if it's a known disk folder. */
__attribute__((import_module("hal"), import_name("folder_opendir")))
uint32_t hal_folder_opendir(const char *folder_id, uint32_t id_len);

/* ── Disk-backed owner folders + consumer downloads ──
 * Register an on-disk directory as an owned folder (files served from disk, not
 * copied to the archive). Asynchronous: returns 1 when started; poll
 * hal_folder_owned for the resulting folderId. */
__attribute__((import_module("hal"), import_name("folder_add_disk")))
uint32_t hal_folder_add_disk(const char *path, uint32_t path_len);

/* Re-scan owned disk folders and sync changes (one if folder_id given, else
 * all when id_len==0). Asynchronous; returns 1 when started. */
__attribute__((import_module("hal"), import_name("folder_rescan")))
uint32_t hal_folder_rescan(const char *folder_id, uint32_t id_len);

/* Download from a folder. json is {"sha":..,"name":..} for one file, or
 * {"all":true} for the whole folder. Asynchronous; returns 1 when started. */
__attribute__((import_module("hal"), import_name("folder_download")))
uint32_t hal_folder_download(const char *folder_id, uint32_t id_len,
                            const char *json, uint32_t json_len);

/* Turn auto-sync on/off for a folder (on != 0). */
__attribute__((import_module("hal"), import_name("folder_autosync")))
uint32_t hal_folder_autosync(const char *folder_id, uint32_t id_len, int32_t on);

/* Owned disk folders → JSON [{"folderId","dir","files"}]. */
__attribute__((import_module("hal"), import_name("folder_owned")))
uint32_t hal_folder_owned(char *out_buf, uint32_t out_len);

/* Folder subscriptions → JSON [{"folderId","autoSync","downloaded"}]. */
__attribute__((import_module("hal"), import_name("folder_subs")))
uint32_t hal_folder_subs(char *out_buf, uint32_t out_len);

/* List a real directory for an in-app folder browser → JSON array of
 * {"name","path","dir"} (directories first). Empty/0 if not accessible. */
__attribute__((import_module("hal"), import_name("fs_listdir")))
uint32_t hal_fs_listdir(const char *path, uint32_t path_len,
                        char *out_buf, uint32_t out_len);

/* Request broad file access (Android "all files"). Returns 1 (the system access
 * screen opens on Android); poll hal_fs_listdir afterwards. */
__attribute__((import_module("hal"), import_name("storage_request")))
uint32_t hal_storage_request(int32_t unused);

/* A good starting directory for the in-app browser (Android primary storage,
 * else the desktop home dir) → its path. */
__attribute__((import_module("hal"), import_name("fs_home")))
uint32_t hal_fs_home(char *out_buf, uint32_t out_len);

/* Try to obtain the bytes for a token from known sources (Blossom servers,
 * then the torrent swarm). Asynchronous: returns 1 when the lookup started
 * (or the file is already local); poll hal_media_meta to see it arrive. */
/* Obtain the bytes for a token: scan the LAN for a Blossom peer that has it,
 * then the BitTorrent swarm via any recorded infohash. Asynchronous: 1 = the
 * lookup started (or already local); poll hal_media_meta for arrival. */
__attribute__((import_module("hal"), import_name("media_fetch")))
uint32_t hal_media_fetch(const char *token, uint32_t token_len);

/* Fetch from a magnet: link (the cross-internet path). [expected] is an
 * optional file:token to verify the downloaded content against. */
__attribute__((import_module("hal"), import_name("media_fetch_magnet")))
uint32_t hal_media_fetch_magnet(const char *magnet, uint32_t magnet_len,
                                const char *expected, uint32_t expected_len);

/* Record a source for a hash. kind = "blossom" (base URL), "infohash"
 * (40-hex) or "callsign". */
__attribute__((import_module("hal"), import_name("media_add_source")))
uint32_t hal_media_add_source(const char *token, uint32_t token_len,
                              const char *kind, uint32_t kind_len,
                              const char *value, uint32_t value_len);

/* A shareable magnet: link for an archived token (the reference handed to a
 * peer on another network to fetch over BitTorrent). Built in the background:
 * returns 0 until ready, then the magnet on a later call. */
__attribute__((import_module("hal"), import_name("media_magnet")))
uint32_t hal_media_magnet(const char *token, uint32_t token_len,
                          char *out_buf, uint32_t out_len);

/* The deterministic torrent infohash (40-hex) for an archived token. The
 * sender appends "ih:<hex>" to a share message so the receiver can join the
 * swarm. Built in the background: 0 until ready, then the hex on a later
 * call. */
__attribute__((import_module("hal"), import_name("media_infohash")))
uint32_t hal_media_infohash(const char *token, uint32_t token_len,
                            char *out_buf, uint32_t out_len);

/* Apply sharing controls: {"server":bool,"port":n,"uploads":bool,
 * "seed":bool}. server = Blossom HTTP endpoint; seed = BitTorrent. */
__attribute__((import_module("hal"), import_name("share_ctl")))
uint32_t hal_share_ctl(const char *json, uint32_t json_len);

/* {"server":{"running":b,"port":n,"uploads":b,"requests":n,"bytes":n},
 *  "torrents":[{"infohash","token","seeding","progress","peers"},..]} */
__attribute__((import_module("hal"), import_name("share_status")))
uint32_t hal_share_status(char *out_buf, uint32_t out_len);

/* Routine LAN scan for Blossom servers: probes the local network and refreshes
 * the host's cached directory of reachable Aurora Blossom servers. Call it
 * periodically (NOT per message). Writes a JSON array of base URLs to out_buf;
 * returns the count of reachable servers. */
__attribute__((import_module("hal"), import_name("lan_scan")))
uint32_t hal_lan_scan(char *out_buf, uint32_t out_len);

/* Free heap bytes available to this module */
__attribute__((import_module("hal"), import_name("heap_free")))
uint32_t hal_heap_free(void);

/* ── Storage (key-value, scoped per module ID) ──────────────────────── */

/* Get value for key. Returns bytes written to val_buf, 0 if not found. */
__attribute__((import_module("hal"), import_name("kv_get")))
uint32_t hal_kv_get(const char *key, uint32_t key_len,
                    char *val_buf, uint32_t val_buf_len);

/* Set key=value. Returns 0 on success, -1 on error. */
__attribute__((import_module("hal"), import_name("kv_set")))
int32_t hal_kv_set(const char *key, uint32_t key_len,
                   const char *val, uint32_t val_len);

/* Delete key. Returns 0 on success, -1 if not found. */
__attribute__((import_module("hal"), import_name("kv_delete")))
int32_t hal_kv_delete(const char *key, uint32_t key_len);

/* List keys matching prefix. Writes null-separated key names into buf.
 * Returns number of keys found (0 if none). */
__attribute__((import_module("hal"), import_name("kv_list")))
uint32_t hal_kv_list(const char *prefix, uint32_t prefix_len,
                     char *buf, uint32_t buf_len);

/* Check if key exists. Returns 1 if exists, 0 if not. */
__attribute__((import_module("hal"), import_name("kv_exists")))
int32_t hal_kv_exists(const char *key, uint32_t key_len);

/* Get size of value for key. Returns size in bytes, 0 if not found. */
__attribute__((import_module("hal"), import_name("kv_size")))
uint32_t hal_kv_size(const char *key, uint32_t key_len);

/* ── Internationalisation ───────────────────────────────────────────── */

/* Look up a translation key against the wapp's lang/<locale>.json
 * (merged with the English fallback). Writes up to out_cap bytes of
 * the localised string into out (NOT null-terminated). Returns the
 * number of bytes written. When the key is missing from both the
 * primary and fallback maps, returns 0 and out is untouched — the
 * caller should fall back to its hard-coded literal. */
__attribute__((import_module("hal"), import_name("i18n_get")))
uint32_t hal_i18n_get(const char *key, uint32_t key_len,
                      char *out, uint32_t out_cap);

/* ── Storage (host filesystem, no sandbox) ──────────────────────────
 *
 * Absolute paths only. Same trust model as hal_process_exec — wapps
 * have full filesystem access at the privilege level of the geogram
 * process.
 *
 * Reads slurp the whole file at open and serve from a buffer; writes
 * accumulate in memory and flush at close. The host creates parent
 * directories automatically on write/append open as a convenience.
 */

/* Open file. mode: 0=read, 1=write, 2=append. Returns handle or -1.
 * Path must be absolute. */
__attribute__((import_module("hal"), import_name("file_open")))
int32_t hal_file_open(const char *path, uint32_t path_len, int32_t mode);

/* Read up to buf_len bytes. Returns bytes read, 0 on EOF, -1 on error. */
__attribute__((import_module("hal"), import_name("file_read")))
int32_t hal_file_read(int32_t handle, char *buf, uint32_t buf_len);

/* Write buf_len bytes. Returns bytes written or -1 on error. */
__attribute__((import_module("hal"), import_name("file_write")))
int32_t hal_file_write(int32_t handle, const char *buf, uint32_t buf_len);

/* Close file handle. */
__attribute__((import_module("hal"), import_name("file_close")))
void hal_file_close(int32_t handle);

/* ── Network (async polling) ────────────────────────────────────────── */

/* Start HTTP request. Returns request_id or -1 on error. */
/* method: 0=GET, 1=POST, 2=PUT, 3=DELETE */
__attribute__((import_module("hal"), import_name("http_request")))
int32_t hal_http_request(int32_t method,
                         const char *url, uint32_t url_len,
                         const char *body, uint32_t body_len);

/* Poll request status. Returns: 0=pending, 1=complete, -1=error */
__attribute__((import_module("hal"), import_name("http_poll")))
int32_t hal_http_poll(int32_t request_id);

/* Read response body. Returns bytes written to buf. */
__attribute__((import_module("hal"), import_name("http_read_response")))
int32_t hal_http_read_response(int32_t request_id, char *buf, uint32_t buf_len);

/* HTTP status code for completed request, or -1 if still pending. */
__attribute__((import_module("hal"), import_name("http_status")))
int32_t hal_http_status(int32_t request_id);

/* Free request resources. */
__attribute__((import_module("hal"), import_name("http_free")))
void hal_http_free(int32_t request_id);

/* ── Streaming HTTP (online radio) ──────────────────────────────────────
 * A long-lived GET whose body arrives over time. The host handles TLS,
 * redirects and ICY/SHOUTcast metadata (stripped out so reads return pure
 * audio; the latest StreamTitle is available via hal_http_stream_meta). */

/* Open a streaming GET. Returns a handle (>=0) immediately; data arrives in
 * the background. Returns -1 on bad arguments. */
__attribute__((import_module("hal"), import_name("http_stream_open")))
int32_t hal_http_stream_open(const char *url, uint32_t url_len);

/* Drain available audio bytes into buf. Returns bytes read (0 = none yet,
 * -1 = stream closed/ended and drained). */
__attribute__((import_module("hal"), import_name("http_stream_read")))
int32_t hal_http_stream_read(int32_t handle, char *buf, uint32_t buf_len);

/* Latest ICY StreamTitle into buf. Returns bytes written (0 if none). */
__attribute__((import_module("hal"), import_name("http_stream_meta")))
uint32_t hal_http_stream_meta(int32_t handle, char *buf, uint32_t buf_len);

/* Close the stream and release the handle. */
__attribute__((import_module("hal"), import_name("http_stream_close")))
void hal_http_stream_close(int32_t handle);

/* ── Raw TCP socket (async, host network) ───────────────────────────── */

/* Open a TCP connection. Returns a handle (>=0) immediately; the
 * connection resolves in the background — poll hal_socket_status until it
 * reports open. Returns -1 on bad arguments. */
__attribute__((import_module("hal"), import_name("socket_open")))
int32_t hal_socket_open(const char *host, uint32_t host_len, int32_t port);

/* Connection state: 0 = connecting, 1 = open, 2 = closed or error. */
__attribute__((import_module("hal"), import_name("socket_status")))
int32_t hal_socket_status(int32_t handle);

/* Queue bytes for transmission. Returns bytes accepted, or -1 on error. */
__attribute__((import_module("hal"), import_name("socket_send")))
int32_t hal_socket_send(int32_t handle, const char *buf, uint32_t len);

/* Drain received bytes into buf. Returns bytes read (0 if none yet). */
__attribute__((import_module("hal"), import_name("socket_recv")))
uint32_t hal_socket_recv(int32_t handle, char *buf, uint32_t buf_len);

/* Close the connection and release the handle. */
__attribute__((import_module("hal"), import_name("socket_close")))
void hal_socket_close(int32_t handle);

/* ── Synchronous (blocking) socket — for the test runner only ────────── *
 * The wasm test runner (module_run_tests) is one synchronous call and
 * can't await the async hal_socket_* above. These blocking variants let
 * a test connect/write/read within that single call: connect blocks,
 * `avail` returns kernel-buffered bytes without blocking, and `read`
 * pulls only what's already buffered. Bound your read loop with
 * hal_time_ms(). Do NOT use these in normal wapp code — they block. */
__attribute__((import_module("hal"), import_name("socket_open_sync")))
int32_t hal_socket_open_sync(const char *host, uint32_t host_len, int32_t port);

__attribute__((import_module("hal"), import_name("socket_avail_sync")))
int32_t hal_socket_avail_sync(int32_t handle);

__attribute__((import_module("hal"), import_name("socket_read_sync")))
int32_t hal_socket_read_sync(int32_t handle, char *buf, uint32_t buf_len);

__attribute__((import_module("hal"), import_name("socket_write_sync")))
int32_t hal_socket_write_sync(int32_t handle, const char *buf, uint32_t len);

__attribute__((import_module("hal"), import_name("socket_close_sync")))
void hal_socket_close_sync(int32_t handle);

/* ── Process (host subprocess) ──────────────────────────────────────
 *
 * Generic host-process primitive. The wapp asks the host to run a
 * binary — the host runs it as-is, with no sandbox and no allow
 * list. argv is a JSON array of strings, e.g.
 *
 *   ["/usr/bin/clang", "-O2", "-o", "/tmp/a.wasm", "/tmp/main.c"]
 *
 * cwd is an absolute working directory or empty for the host's CWD.
 * Always use absolute paths in argv. The host's PATH is not searched
 * for argv[0].
 *
 * Async polling, mirrors hal_http_*. After hal_process_exec returns
 * a handle, the wapp polls hal_process_poll each tick, drains
 * stdout/stderr with hal_process_read_*, and finally calls
 * hal_process_free to release the handle.
 *
 * Output buffers are unbounded host-side — the wapp can drain on
 * its own cadence without losing data, but should drain regularly
 * for long-running processes to keep host memory in check.
 */

/* Spawn a subprocess. Returns handle (>=0) or -1 on error. */
__attribute__((import_module("hal"), import_name("process_exec")))
int32_t hal_process_exec(const char *argv_json, uint32_t argv_json_len,
                         const char *cwd, uint32_t cwd_len);

/* Poll process state. Returns:
 *    0 = still running
 *    1 = exited (call hal_process_exit_code for the code)
 *   -1 = error or unknown handle
 */
__attribute__((import_module("hal"), import_name("process_poll")))
int32_t hal_process_poll(int32_t handle);

/* Exit code of a finished process. Returns -1 if the process is
 * still running or the handle is unknown. */
__attribute__((import_module("hal"), import_name("process_exit_code")))
int32_t hal_process_exit_code(int32_t handle);

/* Drain buffered stdout into buf. Returns bytes written, 0 if no
 * output is currently buffered. Output is raw bytes — caller owns
 * line splitting. */
__attribute__((import_module("hal"), import_name("process_read_stdout")))
uint32_t hal_process_read_stdout(int32_t handle,
                                 char *buf, uint32_t buf_len);

/* Drain buffered stderr into buf. Same semantics as
 * hal_process_read_stdout. */
__attribute__((import_module("hal"), import_name("process_read_stderr")))
uint32_t hal_process_read_stderr(int32_t handle,
                                 char *buf, uint32_t buf_len);

/* Release the handle. Kills the process if still running, drops any
 * unread buffered output. */
__attribute__((import_module("hal"), import_name("process_free")))
void hal_process_free(int32_t handle);

/* ── LoRa ───────────────────────────────────────────────────────────── */

/* Returns 1 if LoRa hardware is present, 0 otherwise. */
__attribute__((import_module("hal"), import_name("lora_available_hw")))
int32_t hal_lora_available_hw(void);

/* Send packet. Returns 0 on success, -1 on error. */
__attribute__((import_module("hal"), import_name("lora_send")))
int32_t hal_lora_send(const char *data, uint32_t data_len);

/* Returns number of bytes available to read, 0 if none. */
__attribute__((import_module("hal"), import_name("lora_available")))
uint32_t hal_lora_available(void);

/* Read received packet into buf. Returns bytes read. */
__attribute__((import_module("hal"), import_name("lora_recv")))
uint32_t hal_lora_recv(char *buf, uint32_t buf_len);

/* ── BLE ────────────────────────────────────────────────────────────── */

/* Start BLE scan. Returns 0 on success. */
__attribute__((import_module("hal"), import_name("ble_scan_start")))
int32_t hal_ble_scan_start(void);

/* Stop BLE scan. */
__attribute__((import_module("hal"), import_name("ble_scan_stop")))
void hal_ble_scan_stop(void);

/* Read next scan result as JSON into buf. Returns bytes, 0 if none. */
__attribute__((import_module("hal"), import_name("ble_scan_read")))
uint32_t hal_ble_scan_read(char *buf, uint32_t buf_len);

/* Start BLE advertising with given payload. Returns 0 on success. */
__attribute__((import_module("hal"), import_name("ble_advertise")))
int32_t hal_ble_advertise(const char *data, uint32_t data_len);

/* Stop BLE advertising. */
__attribute__((import_module("hal"), import_name("ble_advertise_stop")))
void hal_ble_advertise_stop(void);

/* Whether the physical Bluetooth adapter is powered ON right now (the user can
 * toggle it at the OS level at any time). Returns 1 = on/usable, 0 = off. Use
 * this before claiming a BLE channel is available. */
__attribute__((import_module("hal"), import_name("ble_available")))
int32_t hal_ble_available(void);

/* ── Sensors ────────────────────────────────────────────────────────── */

/* Temperature in centidegrees C (2500 = 25.00°C). INT32_MIN if N/A. */
__attribute__((import_module("hal"), import_name("sensor_temperature")))
int32_t hal_sensor_temperature(void);

/* Relative humidity in centipercent (6500 = 65.00%). INT32_MIN if N/A. */
__attribute__((import_module("hal"), import_name("sensor_humidity")))
int32_t hal_sensor_humidity(void);

/* Battery millivolts (3700 = 3.7V). INT32_MIN if N/A. */
__attribute__((import_module("hal"), import_name("sensor_battery")))
int32_t hal_sensor_battery(void);

/* GPS latitude * 1e7 (e.g. 377749000 = 37.7749°N). INT32_MIN if N/A. */
__attribute__((import_module("hal"), import_name("sensor_gps_lat")))
int32_t hal_sensor_gps_lat(void);

/* GPS longitude * 1e7. INT32_MIN if N/A. */
__attribute__((import_module("hal"), import_name("sensor_gps_lon")))
int32_t hal_sensor_gps_lon(void);

/* ── Display ────────────────────────────────────────────────────────── */

/* Display width in pixels. 0 if no display. */
__attribute__((import_module("hal"), import_name("display_width")))
uint32_t hal_display_width(void);

/* Display height in pixels. 0 if no display. */
__attribute__((import_module("hal"), import_name("display_height")))
uint32_t hal_display_height(void);

/* Clear display buffer. */
__attribute__((import_module("hal"), import_name("display_clear")))
void hal_display_clear(void);

/* Draw text at (x,y). color: 0=black, 1=white. */
__attribute__((import_module("hal"), import_name("display_text")))
void hal_display_text(int32_t x, int32_t y, int32_t color,
                      const char *text, uint32_t text_len);

/* Set single pixel. */
__attribute__((import_module("hal"), import_name("display_pixel")))
void hal_display_pixel(int32_t x, int32_t y, int32_t color);

/* Draw filled rectangle. */
__attribute__((import_module("hal"), import_name("display_rect")))
void hal_display_rect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t color);

/* Flush buffer to physical display. */
__attribute__((import_module("hal"), import_name("display_flush")))
void hal_display_flush(void);

/* ── Codec-free A/V sink ────────────────────────────────────────────────
 * The host contains NO codec. A media wapp decodes (e.g. mp4/H.264) IN
 * wasm and pushes the decoded frames/PCM here; the host only uploads the
 * raw pixels to a texture and (later) plays the raw samples. This keeps
 * the codec/player out of the host binary and makes playback platform
 * agnostic — the decoder travels inside the .wapp. */

/* Announce the video geometry before the first frame. pixfmt: 0 = RGBA8888. */
__attribute__((import_module("hal"), import_name("video_config")))
void hal_video_config(int32_t width, int32_t height, int32_t pixfmt);

/* Submit one decoded frame. `data` points into wasm linear memory; for
 * pixfmt 0 (RGBA8888) `len` must equal width*height*4. pts_ms is the
 * presentation timestamp in milliseconds from the start of the stream. */
__attribute__((import_module("hal"), import_name("video_frame")))
void hal_video_frame(const uint8_t *data, uint32_t len,
                     int32_t width, int32_t height,
                     int32_t pixfmt, int32_t pts_ms);

/* Submit one block of decoded PCM. sampfmt: 0 = s16 interleaved,
 * 1 = f32 interleaved. (Wired end-to-end; the MVP host drops audio.) */
__attribute__((import_module("hal"), import_name("audio_pcm")))
void hal_audio_pcm(const uint8_t *data, uint32_t len,
                   int32_t sample_rate, int32_t channels,
                   int32_t sampfmt, int32_t pts_ms);

/* Signal end of stream (playback reached the last frame). */
__attribute__((import_module("hal"), import_name("video_end")))
void hal_video_end(void);

/* ── GPIO (ESP32 only, no-op elsewhere) ─────────────────────────────── */

/* Set pin mode. mode: 0=input, 1=output, 2=input_pullup. */
__attribute__((import_module("hal"), import_name("gpio_mode")))
void hal_gpio_mode(int32_t pin, int32_t mode);

/* Read digital pin value (0 or 1). */
__attribute__((import_module("hal"), import_name("gpio_read")))
int32_t hal_gpio_read(int32_t pin);

/* Write digital pin value (0 or 1). */
__attribute__((import_module("hal"), import_name("gpio_write")))
void hal_gpio_write(int32_t pin, int32_t value);

/* ── Messages (host <-> module JSON) ────────────────────────────────── */

/* Send JSON message to host. */
__attribute__((import_module("hal"), import_name("msg_send")))
void hal_msg_send(const char *json, uint32_t json_len);

/* Returns number of bytes in next pending message, 0 if none. */
__attribute__((import_module("hal"), import_name("msg_available")))
uint32_t hal_msg_available(void);

/* Read next pending message into buf. Returns bytes read. */
__attribute__((import_module("hal"), import_name("msg_recv")))
uint32_t hal_msg_recv(char *buf, uint32_t buf_len);

/* ── Library calls (cross-module RPC) ───────────────────────────────── */

/* Call a function in a loaded library module.
 * Returns bytes written to result on success, or negative error code:
 *   -1 = library not found, -2 = function not found,
 *   -3 = buffer too small, -4 = internal error. */
__attribute__((import_module("hal"), import_name("lib_call")))
int32_t hal_lib_call(const char *lib_id, uint32_t lib_id_len,
                     const char *fn_name, uint32_t fn_name_len,
                     const char *args, uint32_t args_len,
                     char *result, uint32_t result_len);

/* ── Events (topic-based pub/sub between modules) ───────────────────── */

/* Subscribe to a topic. Returns 0 on success, -1 on error. */
__attribute__((import_module("hal"), import_name("event_subscribe")))
int32_t hal_event_subscribe(const char *topic, uint32_t topic_len);

/* Unsubscribe from a topic. Returns 0 on success, -1 if not subscribed. */
__attribute__((import_module("hal"), import_name("event_unsubscribe")))
int32_t hal_event_unsubscribe(const char *topic, uint32_t topic_len);

/* Publish data to a topic. Returns number of subscribers notified. */
__attribute__((import_module("hal"), import_name("event_publish")))
int32_t hal_event_publish(const char *topic, uint32_t topic_len,
                          const char *data, uint32_t data_len);

/* Returns size of next pending event (topic_len + data_len), 0 if none. */
__attribute__((import_module("hal"), import_name("event_available")))
uint32_t hal_event_available(void);

/* Read next pending event. Writes topic into topic_buf, data into data_buf.
 * Returns bytes written to data_buf, 0 if no event available. */
__attribute__((import_module("hal"), import_name("event_recv")))
uint32_t hal_event_recv(char *topic_buf, uint32_t topic_buf_len,
                        char *data_buf, uint32_t data_buf_len);

/* ── SQLite (per-wapp relational storage) ───────────────────────────── *
 *
 * Generic database access for wapps. `path` is RELATIVE to this wapp's
 * private data directory — a leading '/' and any ".." segment are
 * rejected, so a wapp can never reach another wapp's files or escape its
 * sandbox. The database is created if missing (WAL mode).
 *
 * Binary values are NOT marshalled across the boundary: store binary as
 * base64 TEXT. Bind parameters are an optional JSON array matched to '?'
 * placeholders in order (JSON string/number/null → TEXT/INTEGER-or-REAL/
 * NULL); pass len 0 for none. Query rows come back as a JSON array of
 * objects (column name → value). */

/* Open/create a database. Returns a handle (>=0) or -1 on error. */
__attribute__((import_module("hal"), import_name("sqlite_open")))
int32_t hal_sqlite_open(const char *path, uint32_t path_len);

/* Run a non-SELECT statement (CREATE/INSERT/UPDATE/DELETE/PRAGMA).
 * Returns 0 on success, -1 on error (see hal_sqlite_error). */
__attribute__((import_module("hal"), import_name("sqlite_exec")))
int32_t hal_sqlite_exec(int32_t handle, const char *sql, uint32_t sql_len,
                        const char *params_json, uint32_t params_len);

/* Run a SELECT and write the result rows as a JSON array into out.
 * Returns bytes written (>=0), -1 on SQL error, or -2 if the buffer is
 * too small (retry with a larger one; use LIMIT to bound results). */
__attribute__((import_module("hal"), import_name("sqlite_query")))
int32_t hal_sqlite_query(int32_t handle, const char *sql, uint32_t sql_len,
                         const char *params_json, uint32_t params_len,
                         char *out, uint32_t out_cap);

/* Write the last error message for a handle into out. Returns bytes. */
__attribute__((import_module("hal"), import_name("sqlite_error")))
uint32_t hal_sqlite_error(int32_t handle, char *out, uint32_t out_cap);

/* Close a database handle and release its resources. */
__attribute__((import_module("hal"), import_name("sqlite_close")))
void hal_sqlite_close(int32_t handle);

/* ── Generic crypto (caller-supplied keys) ──────────────────────────── *
 *
 * The hal_identity_* / hal_encrypt / hal_decrypt calls all use THIS device's
 * profile key. These complementary calls operate on keys the wapp itself
 * holds — needed for things like a group/room key independent of any one
 * member. Keys and signatures are lowercase hex strings; messages and AES
 * payloads are raw bytes. Signing hashes the message with SHA-256 first
 * (BIP-340 Schnorr over secp256k1), matching hal_identity_sign. */

/* Generate a fresh secp256k1 keypair. Writes JSON {"priv":hex,"pub":hex}
 * (pub is the 32-byte x-only key). Returns bytes written, 0 on error. */
__attribute__((import_module("hal"), import_name("crypto_keygen")))
uint32_t hal_crypto_keygen(char *out, uint32_t out_cap);

/* Sign msg with priv_hex; write the signature hex. Returns bytes, 0 on error. */
__attribute__((import_module("hal"), import_name("crypto_sign")))
uint32_t hal_crypto_sign(const char *priv_hex, uint32_t priv_len,
                         const char *msg, uint32_t msg_len,
                         char *out, uint32_t out_cap);

/* Verify sig_hex on msg for pub_hex. Returns 1 if valid, 0 otherwise. */
__attribute__((import_module("hal"), import_name("crypto_verify")))
int32_t hal_crypto_verify(const char *pub_hex, uint32_t pub_len,
                          const char *sig_hex, uint32_t sig_len,
                          const char *msg, uint32_t msg_len);

/* Fill out with out_len cryptographically-random bytes. Returns bytes written. */
__attribute__((import_module("hal"), import_name("crypto_random")))
uint32_t hal_crypto_random(char *out, uint32_t out_len);

/* AES-256-CBC encrypt `in` under the 32-byte `key`. A random IV is
 * generated and prepended to the ciphertext. Returns bytes written, 0 on
 * error (e.g. key not 32 bytes or out too small). */
__attribute__((import_module("hal"), import_name("crypto_aes_encrypt")))
uint32_t hal_crypto_aes_encrypt(const char *key, uint32_t key_len,
                                const char *in, uint32_t in_len,
                                char *out, uint32_t out_cap);

/* Decrypt a blob produced by hal_crypto_aes_encrypt (IV || ciphertext)
 * under the 32-byte `key`. Returns plaintext bytes written, 0 on error. */
__attribute__((import_module("hal"), import_name("crypto_aes_decrypt")))
uint32_t hal_crypto_aes_decrypt(const char *key, uint32_t key_len,
                                const char *in, uint32_t in_len,
                                char *out, uint32_t out_cap);

/* ── Reticulum (wapp-scoped peer-to-peer datagrams) ─────────────────── *
 *
 * Exchange opaque datagrams with other devices running the SAME wapp over
 * the device's shared Reticulum node. Traffic is demultiplexed by the
 * calling wapp, so different wapps never see each other's datagrams. A
 * datagram is delivered to every reachable peer (the RNS mesh: LAN, BLE,
 * and transport peers); on a LAN/BLE link it travels device-to-device,
 * and where peers are only reachable through a transport node it transits
 * that node but the payload is whatever bytes the wapp put in — encrypt
 * end-to-end before sending. Payloads must fit one packet (a few hundred
 * bytes); chunk anything larger. */

/* This device's RNS destination hash (hex) — usable as a sender id so a
 * peer can tell datagrams apart. Returns bytes written, 0 if node down. */
__attribute__((import_module("hal"), import_name("rns_identity")))
uint32_t hal_rns_identity(char *out, uint32_t out_cap);

/* Broadcast `payload` to all reachable peers running this wapp. Returns 1
 * if queued, -1 on error (node down / payload too large). */
__attribute__((import_module("hal"), import_name("rns_broadcast")))
int32_t hal_rns_broadcast(const char *payload, uint32_t payload_len);

/* Reliably deliver `payload` ADDRESSED to one peer's RNS delivery dest (hex,
 * from hal_rns_delivery_dest). Direct if reachable, else held for the peer to
 * pull (store-and-forward) — tolerant of NAT/asymmetric inbound. The peer
 * receives it on the same queue as hal_rns_recv. Returns 1 if queued, -1 err. */
__attribute__((import_module("hal"), import_name("rns_send_to")))
int32_t hal_rns_send_to(const char *dest_hex, uint32_t dest_len,
                        const char *payload, uint32_t payload_len);

/* Pull store-and-forwarded datagrams a peer is holding for us, from its
 * propagation dest (hex, from hal_rns_prop_dest). Fire-and-forget; pulled
 * datagrams arrive via hal_rns_recv. Returns 1 if queued, -1 on error. */
__attribute__((import_module("hal"), import_name("rns_pull")))
int32_t hal_rns_pull(const char *prop_dest_hex, uint32_t dest_len);

/* This device's LXMF delivery dest hash (hex) — give to peers so they can
 * hal_rns_send_to us. Returns bytes written, 0 if node down. */
__attribute__((import_module("hal"), import_name("rns_delivery_dest")))
uint32_t hal_rns_delivery_dest(char *out, uint32_t out_cap);

/* This device's LXMF propagation (mailbox) dest hash (hex) — give to peers so
 * they can hal_rns_pull from us. Returns bytes written, 0 if node down. */
__attribute__((import_module("hal"), import_name("rns_prop_dest")))
uint32_t hal_rns_prop_dest(char *out, uint32_t out_cap);

/* Short-code rendezvous (discovery without a directory). Announce a rendezvous
 * destination derived from the public [seed] (e.g. a circle short code) carrying
 * [app_data] (our address + the full id), so a peer holding only the seed can
 * find us. Owners call this periodically. Returns 1 if queued, -1 on error. */
__attribute__((import_module("hal"), import_name("rns_rv_announce")))
int32_t hal_rns_rv_announce(const char *seed, uint32_t seed_len,
                            const char *app_data, uint32_t app_len);

/* Resolve the rendezvous for [seed]: path-requests the derived dest and writes
 * the announced app_data into [out]. Returns bytes written, or 0 while pending
 * (call again to poll — the first call kicks off the async resolve). */
__attribute__((import_module("hal"), import_name("rns_rv_resolve")))
uint32_t hal_rns_rv_resolve(const char *seed, uint32_t seed_len,
                            char *out, uint32_t out_cap);

/* Send [payload] to the rendezvous dest derived from [seed] as ONE encrypted
 * connectionless packet (the owner listens there). First-contact channel that
 * needs no link handshake, so it survives a flaky owner inbound. */
__attribute__((import_module("hal"), import_name("rns_rv_send")))
int32_t hal_rns_rv_send(const char *seed, uint32_t seed_len,
                        const char *payload, uint32_t payload_len);

/* Size of the next inbound datagram JSON envelope, 0 if none pending. */
__attribute__((import_module("hal"), import_name("rns_available")))
uint32_t hal_rns_available(void);

/* Read the next inbound datagram as JSON {"from":hex,"payload":base64,
 * "ts":ms} into out. Returns bytes written, 0 if none. */
__attribute__((import_module("hal"), import_name("rns_recv")))
uint32_t hal_rns_recv(char *out, uint32_t out_cap);

/* ── NOSTR-relay store-and-forward DM backup (kind-4 over Reticulum) ──────── *
 *
 * Back up 1:1 messages to up to 3 NOSTR relays reachable over Reticulum. The
 * host owns the active profile key and does the NOSTR work (BIP-340 signing,
 * NIP-04 encryption/decryption, publish/query/delete); the wapp supplies the
 * recipient npub (base64url, as hal_identity_pubkey returns) + plaintext and
 * orchestrates which relays to use + when to poll/clean up. */

/* Up to 3 reachable relays (their RNS identity hashes, hex) as a JSON array of
 * strings. Returns bytes written, 0 if none/too small. */
__attribute__((import_module("hal"), import_name("relay_reachable")))
uint32_t hal_relay_reachable(char *out, uint32_t out_cap);

/* Publish [text] (plaintext) as a kind-4 (NIP-04) encrypted DM to recipient
 * [npub] (base64url x-only pubkey), signed by this device's profile key, to each
 * relay in [relays_json] (a JSON array of relay hashes). [mid] is carried in a
 * `d` tag so the recipient can dedup the relay copy against the direct copy.
 * Fire-and-forget; returns 1 if queued, -1 on error. */
__attribute__((import_module("hal"), import_name("relay_dm_send")))
int32_t hal_relay_dm_send(const char *npub, uint32_t npub_len,
                          const char *text, uint32_t text_len,
                          const char *relays_json, uint32_t relays_len,
                          const char *mid, uint32_t mid_len);

/* Trigger an async fetch of kind-4 DMs addressed to us with created_at >=
 * [since_sec] from [relays_json] (JSON array of relay hashes). Decrypted results
 * arrive on the same queue hal_relay_dm_recv drains. Returns 1 if queued. */
__attribute__((import_module("hal"), import_name("relay_dm_fetch")))
int32_t hal_relay_dm_fetch(uint32_t since_sec,
                           const char *relays_json, uint32_t relays_len);

/* Rendezvous relay set for [pubkey] (hex or base64url x-only): JSON array of
 * relay identity hashes ranked by sha256(relay|pubkey). Sender (recipient's
 * key) and receiver (own key) derive the SAME set independently, so the
 * publish and poll sets meet even when the ?RLY announce was missed. */
__attribute__((import_module("hal"), import_name("relay_for")))
int32_t hal_relay_for(const char *pubkey, uint32_t pubkey_len,
                      char *out, uint32_t out_cap);

/* Pop the next fetched DM as JSON {"id":hex,"from":base64url,"ts":sec,
 * "text":plaintext,"mid":id} into out. Returns bytes written, 0 if none. */
__attribute__((import_module("hal"), import_name("relay_dm_recv")))
uint32_t hal_relay_dm_recv(char *out, uint32_t out_cap);

/* Recipient-authorized delete of received DMs [ids_json] (JSON array of event
 * ids) from [relays_json]. Fire-and-forget; returns 1 if queued. */
__attribute__((import_module("hal"), import_name("relay_dm_drop")))
int32_t hal_relay_dm_drop(const char *ids_json, uint32_t ids_len,
                          const char *relays_json, uint32_t relays_len);

/* Publish OUR identity (callsign -> npub + Reticulum delivery/propagation dests)
 * to [relays_json] as a signed, replaceable kind-30078 event, so peers can
 * resolve us by callsign. Fire-and-forget; returns 1 if queued, -1 on error. */
__attribute__((import_module("hal"), import_name("relay_identity_publish")))
int32_t hal_relay_identity_publish(const char *callsign, uint32_t callsign_len,
                                   const char *deliv, uint32_t deliv_len,
                                   const char *prop, uint32_t prop_len,
                                   const char *relays_json, uint32_t relays_len);

/* Trigger an async resolve of [callsign] -> npub by querying [relays_json] for
 * the identity event. The result (if any) lands on the queue
 * hal_relay_resolve_recv drains. Returns 1 if queued, -1 on error. */
__attribute__((import_module("hal"), import_name("relay_resolve")))
int32_t hal_relay_resolve(const char *callsign, uint32_t callsign_len,
                          const char *relays_json, uint32_t relays_len);

/* Pop the next resolution as JSON {callsign, npub(base64url), deliv, prop}.
 * Returns bytes written, 0 if none queued or the buffer is too small. */
__attribute__((import_module("hal"), import_name("relay_resolve_recv")))
uint32_t hal_relay_resolve_recv(char *out, uint32_t out_cap);

/* ── hal.nostr — transport-abstract NOSTR client ─────────────────────────
 *
 * A normal NOSTR client, but the transport is chosen by each relay URI's
 * scheme, so the wapp never cares which medium a relay is on:
 *   wss:// | ws://   internet WebSocket
 *   rns://<idhash>   a relay on the Reticulum mesh
 *   local            THIS device (also served to others over RNS + a local
 *                    wss server, so the device is itself a relay + Blossom).
 * Every inbound event merges into the one local store; subscriptions are
 * drained one event at a time (inbox-pop). The host holds the profile key and
 * signs on hal_nostr_post — the nsec never enters the wasm sandbox. */

/* Relay list + live status as JSON [{"uri","scheme","status"}]. Returns bytes
 * written, or the negated required size if [out] is too small. */
__attribute__((import_module("hal"), import_name("nostr_relays")))
uint32_t hal_nostr_relays(char *out, uint32_t out_cap);

/* Add/remove a relay by URI (wss://…, rns://<idhash>, local). 1 on success. */
__attribute__((import_module("hal"), import_name("nostr_relay_add")))
int32_t hal_nostr_relay_add(const char *uri, uint32_t uri_len);
__attribute__((import_module("hal"), import_name("nostr_relay_remove")))
int32_t hal_nostr_relay_remove(const char *uri, uint32_t uri_len);

/* Open a subscription from a NIP-01 [filter] (JSON object or array of them),
 * fanned across every enabled relay + the local store. Writes the opaque subId
 * into [out]; returns its byte length, 0 on error. */
__attribute__((import_module("hal"), import_name("nostr_subscribe")))
uint32_t hal_nostr_subscribe(const char *filter, uint32_t filter_len,
                             char *out, uint32_t out_cap);

/* Pop the next buffered event JSON (NIP-01 event object) for subscription [sub]
 * into [out]. Returns bytes written, 0 when the inbox is drained. Poll each
 * tick. */
__attribute__((import_module("hal"), import_name("nostr_event_recv")))
uint32_t hal_nostr_event_recv(const char *sub, uint32_t sub_len,
                              char *out, uint32_t out_cap);

/* Close a subscription. */
__attribute__((import_module("hal"), import_name("nostr_unsubscribe")))
int32_t hal_nostr_unsubscribe(const char *sub, uint32_t sub_len);

/* Build a NOSTR event of [kind] with [content] and [tags] (JSON array of tag
 * arrays), sign it with the active profile key (host-side), and publish to the
 * local store + every enabled relay. Fire-and-forget: the event appears on the
 * feed's next hal_nostr_event_recv drain. */
__attribute__((import_module("hal"), import_name("nostr_post")))
int32_t hal_nostr_post(int32_t kind, const char *content, uint32_t content_len,
                       const char *tags, uint32_t tags_len);

/* Followed pubkeys (hex) as a JSON array — the feed's author set. */
__attribute__((import_module("hal"), import_name("nostr_follows")))
uint32_t hal_nostr_follows(char *out, uint32_t out_cap);

/* Follow / unfollow a pubkey ([key] hex or npub). */
__attribute__((import_module("hal"), import_name("nostr_follow")))
int32_t hal_nostr_follow(const char *key, uint32_t key_len);
__attribute__((import_module("hal"), import_name("nostr_unfollow")))
int32_t hal_nostr_unfollow(const char *key, uint32_t key_len);

/* ── Reticulum visualization/management (read-only) ──────────────────── *
 *
 * The node's view of the Reticulum network, for the "reticulum" wapp to
 * render an interactive graph. This is an OBSERVED, sampled view (the nodes
 * whose announces this node has heard) — never a hub's full client roster.
 * Each returns the number of bytes written; when the JSON does not fit, the
 * NEGATED required length is returned (nothing written) so the caller can
 * retry with a larger buffer. Config (add/remove/connect hubs, passive
 * toggle) is performed by emitting host-action messages, not via the HAL. */

/* Node status JSON ({up,mode,identity,paths,passive,observed,...}). */
__attribute__((import_module("hal"), import_name("rns_status")))
int32_t hal_rns_status(char *out, uint32_t out_cap);

/* Configured bootstrap hubs as a JSON array
 * [{"endpoint":"host:port","connected":bool},...]. */
__attribute__((import_module("hal"), import_name("rns_hubs")))
int32_t hal_rns_hubs(char *out, uint32_t out_cap);

/* The observed network as a {"nodes":[...],"edges":[...],"sample":true}
 * graph. [filter] is an optional JSON object
 * {"service":str,"geogramOnly":bool,"search":str} (pass len 0 for none). */
__attribute__((import_module("hal"), import_name("rns_nodes")))
int32_t hal_rns_nodes(const char *filter, uint32_t filter_len,
                      char *out, uint32_t out_cap);

/* ── BLE street mesh (doc/mesh.md) ──────────────────────────────── *
 *
 * Read-only registry of the BLE mesh node: who is within reach (direct
 * neighbors heard over route beacons) and which destinations are reachable
 * multi-hop, plus this node's own status. Overflow protocol: when the JSON
 * doesn't fit, the NEGATED required size is returned and nothing is written. */

/* Mesh node status JSON
 * ({running,callsign,advertising,class,powered,uptime,neighbors,routes,
 *   beaconsSent,beaconsHeard,revision}). */
__attribute__((import_module("hal"), import_name("mesh_status")))
int32_t hal_mesh_status(char *out, uint32_t out_cap);

/* Devices within reach as ready-to-render people-widget sections
 * [{"title":"Nearby (n)","items":[{id,title,subtitle,tags,...}]},
 *  {"title":"Multi-hop (n)","items":[...]}]. */
__attribute__((import_module("hal"), import_name("mesh_devices")))
int32_t hal_mesh_devices(char *out, uint32_t out_cap);

/* Custody store + bulk spool counters JSON (M2/M3). */
__attribute__((import_module("hal"), import_name("mesh_scf_status")))
int32_t hal_mesh_scf_status(char *out, uint32_t out_cap);

/* Bulk transfers JSON [{sha,name,target,size,have,state,active}]. */
__attribute__((import_module("hal"), import_name("mesh_transfers")))
int32_t hal_mesh_transfers(char *out, uint32_t out_cap);

/* Set a mesh tunable "key=value" (msgQuotaMb, bulkQuotaMb). 0 ok. */
__attribute__((import_module("hal"), import_name("mesh_set_pref")))
int32_t hal_mesh_set_pref(const char *kv, uint32_t kv_len);

/* ── Contacts (people this device already knows) ─────────────────────── *
 *
 * A reusable picker source: the people the user can address — those seen on
 * APRS (where a callsign is bound to a public key) and those they follow —
 * so a wapp can offer "add from contacts" instead of pasting a raw key. The
 * result is a JSON array of objects {"npub":..,"callsign":..,"nick":..}.
 * [query] filters case-insensitively across npub, callsign and nickname (an
 * empty query returns everyone). A callsign is only present when its npub is
 * known, so a contact returned here can always be added by any of the three.
 * Returns bytes written, -1 on error, or -2 if the buffer was too small. */
__attribute__((import_module("hal"), import_name("contacts_query")))
int32_t hal_contacts_query(const char *query, uint32_t query_len,
                           char *out, uint32_t out_cap);

#ifdef __cplusplus
}
#endif

#endif /* GEOGRAM_WASM_HAL_H */
