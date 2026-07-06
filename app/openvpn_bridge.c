// Copyright (C) 2026  Mo3he
// SPDX-License-Identifier: AGPL-3.0-or-later

/*
 * ACAP parameter bridge for the OpenVPN userspace VPN.
 *
 * The OpenVPN .ovpn profile (with inline certs, several KB) is too large and
 * multi-line for the ACAP parameter store, so it is uploaded through a tiny
 * embedded HTTP server (127.0.0.1:2202) exposed via the manifest reverseProxy
 * at /local/OpenVPN/api/. The web UI POSTs the profile there; the bridge
 * writes it to client.ovpn (persisted in localdata) and restarts the client.
 *
 * Small settings come through axparameter:
 *   Username/Password  - creds for non-autologin profiles
 *   HttpProxyPort      - outbound HTTP CONNECT proxy port (default 8080)
 *   Socks5Port         - outbound SOCKS5 proxy port (default 1080)
 *
 * The bridge launches the OpenVPN3 client (lib/tun_probe), which terminates the
 * tunnel in userspace and spawns the netstack sidecar. Runs as the unprivileged
 * 'sdk' ACAP user, no root.
 */

#include <axsdk/axparameter.h>
#include <errno.h>
#include <gio/gio.h>
#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#define APP_NAME    "OpenVPN"
#define APP_DIR     "/usr/local/packages/OpenVPN"
#define STATE_DIR   APP_DIR "/localdata"
#define OVPN_FILE   STATE_DIR "/client.ovpn"
#define CREDS_FILE  STATE_DIR "/creds.txt"
#define PORTS_FILE  STATE_DIR "/ports.conf"
#define STATUS_FILE APP_DIR "/html/status.json"
#define CLIENT_BIN  APP_DIR "/lib/tun_probe"
#define HTTP_PORT   2202

static pid_t client_pid = -1;
static guint reload_timer_id = 0;
static AXParameter *g_ax_handle = NULL;

static char *cfg_username   = NULL;
static char *cfg_password   = NULL;
static char *cfg_http_port  = NULL;
static char *cfg_socks_port = NULL;

static void cache_set(char **field, const char *value) {
    if (!value) return;
    free(*field);
    *field = strdup(value);
}

static const char *cache_get(char **field, const char *fallback) {
    return (*field && **field) ? *field : fallback;
}

/* ── child process management ────────────────────────────────────────────── */

static void stop_client(void) {
    if (client_pid <= 0) return;
    /* The client is a process-group leader; signal the whole group so its
     * netstack sidecar is killed too and never orphans (holding proxy ports). */
    kill(-client_pid, SIGTERM);
    kill(client_pid, SIGTERM);
    for (int i = 0; i < 30; i++) {
        int status;
        if (waitpid(client_pid, &status, WNOHANG) == client_pid) {
            kill(-client_pid, SIGKILL);
            client_pid = -1;
            return;
        }
        usleep(100000);
    }
    syslog(LOG_WARNING, "client did not exit in 3 s, sending SIGKILL");
    kill(-client_pid, SIGKILL);
    kill(client_pid, SIGKILL);
    waitpid(client_pid, NULL, 0);
    client_pid = -1;
}

static void start_client(void) {
    stop_client();
    pid_t pid = fork();
    if (pid < 0) {
        syslog(LOG_ERR, "fork failed: %s", strerror(errno));
        return;
    }
    if (pid == 0) {
        execl(CLIENT_BIN, "tun_probe", OVPN_FILE, CREDS_FILE, PORTS_FILE,
              STATUS_FILE, (char *)NULL);
        syslog(LOG_ERR, "execl %s failed: %s", CLIENT_BIN, strerror(errno));
        _exit(1);
    }
    client_pid = pid;
    syslog(LOG_INFO, "openvpn client started (pid %d)", client_pid);
}

static gboolean watchdog_cb(gpointer G_GNUC_UNUSED data) {
    if (client_pid > 0) {
        int status;
        if (waitpid(client_pid, &status, WNOHANG) == client_pid) {
            syslog(LOG_WARNING, "openvpn client exited (status %d), restarting",
                   WEXITSTATUS(status));
            client_pid = -1;
            start_client();
        }
    }
    return G_SOURCE_CONTINUE;
}

/* ── config ──────────────────────────────────────────────────────────────── */

static void load_config_cache(AXParameter *handle) {
    GError *error = NULL;
    gchar *val = NULL;

#define LOAD(name, field)                                                    \
    val = NULL; error = NULL;                                                \
    if (ax_parameter_get(handle, name, &val, &error)) {                      \
        free(field); field = val ? strdup(val) : strdup("");                 \
        g_free(val); val = NULL;                                             \
    } else {                                                                 \
        if (error) { g_error_free(error); error = NULL; }                    \
    }

    LOAD("Username",      cfg_username)
    LOAD("Password",      cfg_password)
    LOAD("HttpProxyPort", cfg_http_port)
    LOAD("Socks5Port",    cfg_socks_port)
#undef LOAD
}

/* Write the (small) creds + ports files. The profile lives in client.ovpn,
 * written by the HTTP upload endpoint and persisted in localdata; we never
 * overwrite it here so an uploaded profile survives restarts. */
static void write_side_config(void) {
    mkdir(STATE_DIR, 0755);
    FILE *f = fopen(CREDS_FILE, "w");
    if (f) {
        fprintf(f, "%s\n%s\n", cache_get(&cfg_username, ""),
                cache_get(&cfg_password, ""));
        fclose(f);
        chmod(CREDS_FILE, 0600);
    }
    f = fopen(PORTS_FILE, "w");
    if (f) {
        fprintf(f, "HTTP_PORT=%s\nSOCKS_PORT=%s\n",
                cache_get(&cfg_http_port, "8080"),
                cache_get(&cfg_socks_port, "1080"));
        fclose(f);
        chmod(PORTS_FILE, 0644);
    }
}

static gboolean debounced_restart(gpointer G_GNUC_UNUSED data) {
    reload_timer_id = 0;
    if (g_ax_handle) load_config_cache(g_ax_handle);
    write_side_config();
    syslog(LOG_INFO, "restarting openvpn client with new config");
    stop_client();
    start_client();
    return G_SOURCE_REMOVE;
}

static void schedule_restart(void) {
    if (reload_timer_id) g_source_remove(reload_timer_id);
    reload_timer_id = g_timeout_add(300, debounced_restart, NULL);
}

static void parameter_changed(const gchar *name, const gchar *value,
                              gpointer G_GNUC_UNUSED user_data) {
    const char *dot = strrchr(name, '.');
    const char *short_name = dot ? dot + 1 : name;
    syslog(LOG_INFO, "parameter changed: %s", short_name);
    if      (strcmp(short_name, "Username")      == 0) cache_set(&cfg_username,   value);
    else if (strcmp(short_name, "Password")      == 0) cache_set(&cfg_password,   value);
    else if (strcmp(short_name, "HttpProxyPort") == 0) cache_set(&cfg_http_port,  value);
    else if (strcmp(short_name, "Socks5Port")    == 0) cache_set(&cfg_socks_port, value);
    schedule_restart();
}

/* ── embedded HTTP upload server (127.0.0.1:HTTP_PORT) ────────────────────── */
/* POST .../profile  body = raw .ovpn text  → write client.ovpn, restart client.
 * GET  .../profile  → returns the current profile length (for the UI).       */

static size_t http_content_length(const char *hdr, size_t hlen) {
    const char *key = "content-length:";
    size_t klen = strlen(key);
    for (size_t i = 0; i + klen <= hlen; i++) {
        if (g_ascii_strncasecmp(hdr + i, key, klen) == 0) {
            i += klen;
            while (i < hlen && (hdr[i] == ' ' || hdr[i] == '\t')) i++;
            return (size_t)strtoul(hdr + i, NULL, 10);
        }
    }
    return 0;
}

static void http_send(GOutputStream *out, const char *status,
                      const char *ctype, const char *body) {
    gchar *resp = g_strdup_printf(
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Connection: close\r\n\r\n%s",
        status, ctype, strlen(body), body);
    g_output_stream_write_all(out, resp, strlen(resp), NULL, NULL, NULL);
    g_free(resp);
}

static void write_profile_file(const char *body, size_t len) {
    mkdir(STATE_DIR, 0755);
    FILE *f = fopen(OVPN_FILE, "w");
    if (!f) {
        syslog(LOG_ERR, "cannot write %s: %s", OVPN_FILE, strerror(errno));
        return;
    }
    fwrite(body, 1, len, f);
    fclose(f);
    chmod(OVPN_FILE, 0600);
    syslog(LOG_INFO, "profile uploaded (%zu bytes) via http", len);
}

static gboolean http_on_incoming(GSocketService *service G_GNUC_UNUSED,
                                 GSocketConnection *connection,
                                 GObject *source G_GNUC_UNUSED,
                                 gpointer user_data G_GNUC_UNUSED) {
    GInputStream  *in  = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));

    GString *req = g_string_new(NULL);
    char buf[4096];
    int have_headers = 0;
    size_t header_end = 0, content_length = 0;

    while (1) {
        gssize n = g_input_stream_read(in, buf, sizeof(buf), NULL, NULL);
        if (n <= 0) break;
        g_string_append_len(req, buf, n);
        if (!have_headers) {
            char *p = g_strstr_len(req->str, req->len, "\r\n\r\n");
            if (p) {
                have_headers = 1;
                header_end = (size_t)(p - req->str) + 4;
                content_length = http_content_length(req->str, header_end);
            }
        }
        if (have_headers && req->len - header_end >= content_length) break;
        if (req->len > 1048576) break; /* 1 MB cap */
    }

    int is_get = g_str_has_prefix(req->str, "GET ");
    int is_post = g_str_has_prefix(req->str, "POST ");
    int is_profile = 0;
    const char *sp1 = strchr(req->str, ' ');
    if (sp1) {
        const char *path = sp1 + 1;
        const char *sp2 = strchr(path, ' ');
        size_t plen = sp2 ? (size_t)(sp2 - path) : strlen(path);
        const char *q = memchr(path, '?', plen);
        size_t mlen = q ? (size_t)(q - path) : plen;
        if (mlen >= 7 && g_ascii_strncasecmp(path + mlen - 7, "profile", 7) == 0)
            is_profile = 1;
    }

    if (is_profile && is_post) {
        const char *body = req->str + header_end;
        size_t body_len = req->len - header_end;
        if (body_len > content_length) body_len = content_length;
        write_profile_file(body, body_len);
        schedule_restart();
        http_send(out, "200 OK", "text/plain", "OK");
    } else if (is_profile && is_get) {
        struct stat st;
        char msg[64];
        long sz = (stat(OVPN_FILE, &st) == 0) ? (long)st.st_size : 0;
        snprintf(msg, sizeof(msg), "{\"profile_bytes\":%ld}", sz);
        http_send(out, "200 OK", "application/json", msg);
    } else {
        http_send(out, "404 Not Found", "text/plain", "Not found");
    }

    g_string_free(req, TRUE);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    return TRUE;
}

static void http_server_start(void) {
    GError *err = NULL;
    GSocketService *service = g_socket_service_new();
    GInetAddress   *addr    = g_inet_address_new_from_string("127.0.0.1");
    GSocketAddress *saddr   = g_inet_socket_address_new(addr, HTTP_PORT);
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(service), saddr,
                                       G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP,
                                       NULL, NULL, &err)) {
        syslog(LOG_WARNING, "http bind 127.0.0.1:%d failed: %s", HTTP_PORT,
               err ? err->message : "unknown");
        if (err) g_error_free(err);
        g_object_unref(service);
    } else {
        g_signal_connect(service, "incoming", G_CALLBACK(http_on_incoming), NULL);
        g_socket_service_start(service);
        syslog(LOG_INFO, "upload http server on 127.0.0.1:%d", HTTP_PORT);
    }
    g_object_unref(addr);
    g_object_unref(saddr);
}

/* ── main ────────────────────────────────────────────────────────────────── */

static gboolean signal_handler(gpointer loop) {
    syslog(LOG_INFO, "stopping");
    stop_client();
    g_main_loop_quit((GMainLoop *)loop);
    return G_SOURCE_REMOVE;
}

int main(void) {
    GError *error = NULL;

    openlog(APP_NAME, LOG_PID, LOG_USER);
    syslog(LOG_INFO, "starting");

    mkdir(STATE_DIR, 0755);

    AXParameter *handle = ax_parameter_new(APP_NAME, &error);
    if (!handle) {
        syslog(LOG_ERR, "ax_parameter_new: %s", error ? error->message : "unknown");
        if (error) g_error_free(error);
        return 1;
    }
    g_ax_handle = handle;

    load_config_cache(handle);
    write_side_config();
    /* client.ovpn is NOT rewritten here — it persists from a prior HTTP upload. */
    start_client();

    const char *params[] = {"Username", "Password", "HttpProxyPort", "Socks5Port"};
    for (size_t i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
        if (!ax_parameter_register_callback(handle, params[i], parameter_changed,
                                            handle, &error)) {
            if (error) { g_error_free(error); error = NULL; }
        }
    }

    http_server_start();

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGTERM, signal_handler, loop);
    g_unix_signal_add(SIGINT, signal_handler, loop);
    g_timeout_add_seconds(30, watchdog_cb, NULL);

    syslog(LOG_INFO, "running");
    g_main_loop_run(loop);

    g_main_loop_unref(loop);
    ax_parameter_free(handle);
    return 0;
}
