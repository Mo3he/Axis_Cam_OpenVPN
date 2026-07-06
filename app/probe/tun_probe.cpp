// Copyright (C) 2026  Mo3he
// SPDX-License-Identifier: AGPL-3.0-or-later

/*
 * OpenVPN userspace client for Axis cameras (ACAP), no root.
 *
 * Runs the OpenVPN3 core entirely in userspace: instead of a kernel
 * /dev/net/tun, tun_builder_establish() returns one end of a SOCK_DGRAM
 * socketpair and spawns the Go netstack sidecar (netstack_proxy), handing it
 * the other end plus the pushed VPN address/MTU. The sidecar attaches a gVisor
 * netstack to the fd and runs the proxy/forwarder layer that makes the camera
 * reachable from, and able to route through, the VPN.
 *
 * Build with -DUSE_TUN_BUILDER so the core routes packets to these callbacks.
 *
 * Usage: tun_probe <ovpn_path> <creds_path> <ports_path> <status_path>
 */

#include <client/ovpncli.cpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

using namespace openvpn;

namespace {

const char *g_status_path = nullptr;
const char *SIDECAR = "/usr/local/packages/OpenVPN/lib/netstack_proxy";

std::string g_http_port = "8080";
std::string g_socks_port = "1080";

std::mutex g_status_mtx;
std::string g_state = "starting", g_vpn_ip4, g_last_event, g_last_error;

void write_status() {
    if (!g_status_path) return;
    std::lock_guard<std::mutex> lk(g_status_mtx);
    std::string tmp = std::string(g_status_path) + ".tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f) return;
    fprintf(f,
            "{\n  \"state\": \"%s\",\n  \"vpn_ip4\": \"%s\",\n"
            "  \"http_port\": \"%s\",\n  \"socks_port\": \"%s\",\n"
            "  \"last_event\": \"%s\",\n  \"last_error\": \"%s\",\n  \"ts\": %ld\n}\n",
            g_state.c_str(), g_vpn_ip4.c_str(), g_http_port.c_str(),
            g_socks_port.c_str(), g_last_event.c_str(), g_last_error.c_str(),
            (long)time(nullptr));
    fclose(f);
    rename(tmp.c_str(), g_status_path);
    chmod(g_status_path, 0644);
}

class VPNClient : public ClientAPI::OpenVPNClient {
    int core_fd_ = -1;          // fd handed to the OpenVPN3 core (its "tun")
    pid_t sidecar_pid_ = -1;    // the Go netstack proxy
    std::string vpn_cidr_ = "10.8.0.2/24";
    int mtu_ = 1500;

    void stop_sidecar() {
        if (sidecar_pid_ > 0) {
            kill(sidecar_pid_, SIGTERM);
            waitpid(sidecar_pid_, nullptr, 0);
            sidecar_pid_ = -1;
        }
        if (core_fd_ >= 0) { close(core_fd_); core_fd_ = -1; }
    }

  public:
    ~VPNClient() override { stop_sidecar(); }

    // --- TunBuilderBase: capture pushed config, spawn userspace data plane ---
    bool tun_builder_new() override { return true; }
    bool tun_builder_set_layer(int) override { return true; }
    bool tun_builder_set_remote_address(const std::string &, bool) override { return true; }
    bool tun_builder_add_address(const std::string &address, int prefix,
                                 const std::string &, bool ipv6, bool) override {
        if (!ipv6) {
            vpn_cidr_ = address + "/" + std::to_string(prefix);
            std::lock_guard<std::mutex> lk(g_status_mtx);
            g_vpn_ip4 = address;
        }
        return true;
    }
    bool tun_builder_reroute_gw(bool, bool, unsigned int) override { return true; }
    bool tun_builder_add_route(const std::string &, int, int, bool) override { return true; }
    bool tun_builder_set_mtu(int mtu) override { if (mtu > 0) mtu_ = mtu; return true; }
    bool tun_builder_set_session_name(const std::string &) override { return true; }
    bool tun_builder_set_dns_options(const DnsOptions &) override { return true; }

    int tun_builder_establish() override {
        stop_sidecar();
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) != 0) {
            syslog(LOG_ERR, "socketpair failed: %s", strerror(errno));
            return -1;
        }
        int bufsz = 1 << 20;
        for (int i = 0; i < 2; i++) {
            setsockopt(fds[i], SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
            setsockopt(fds[i], SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
        }
        pid_t pid = fork();
        if (pid < 0) {
            syslog(LOG_ERR, "fork sidecar failed: %s", strerror(errno));
            close(fds[0]); close(fds[1]);
            return -1;
        }
        if (pid == 0) {
            // Child: hand our end of the socketpair to the sidecar as fd 3.
            dup2(fds[1], 3);
            if (fds[0] > 3) close(fds[0]);
            if (fds[1] > 3) close(fds[1]);
            execl(SIDECAR, "netstack_proxy", "3", vpn_cidr_.c_str(),
                  std::to_string(mtu_).c_str(), g_http_port.c_str(),
                  g_socks_port.c_str(), (char *)nullptr);
            _exit(127);
        }
        sidecar_pid_ = pid;
        close(fds[1]);
        core_fd_ = fds[0];
        syslog(LOG_INFO, "userspace data plane up: sidecar pid %d, vpn=%s mtu=%d http=%s socks=%s",
               sidecar_pid_, vpn_cidr_.c_str(), mtu_, g_http_port.c_str(), g_socks_port.c_str());
        {
            std::lock_guard<std::mutex> lk(g_status_mtx);
            g_state = "tun_established";
        }
        write_status();
        return core_fd_;
    }
    void tun_builder_teardown(bool) override { stop_sidecar(); }

    // --- OpenVPNClient callbacks ---
    bool socket_protect(openvpn_io::detail::socket_type, std::string, bool) override { return true; }
    bool pause_on_connection_timeout() override { return false; }

    void event(const ClientAPI::Event &ev) override {
        syslog(LOG_INFO, "event: %s%s%s", ev.name.c_str(),
               ev.info.empty() ? "" : " - ", ev.info.c_str());
        {
            std::lock_guard<std::mutex> lk(g_status_mtx);
            g_last_event = ev.name;
            if (ev.error) g_last_error = ev.name + ": " + ev.info;
            if (ev.name == "CONNECTED") g_state = "connected";
            else if (ev.name == "DISCONNECTED") g_state = "disconnected";
            else if (ev.name == "RECONNECTING") g_state = "connecting";
            else if (ev.name == "AUTH_FAILED") g_state = "auth_failed";
        }
        write_status();
    }
    void acc_event(const ClientAPI::AppCustomControlMessageEvent &) override {}
    void log(const ClientAPI::LogInfo &li) override {
        std::string t = li.text;
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
        if (!t.empty()) syslog(LOG_INFO, "ovpn: %s", t.c_str());
    }
    void external_pki_cert_request(ClientAPI::ExternalPKICertRequest &req) override {
        req.error = true; req.errorText = "external PKI not supported";
    }
    void external_pki_sign_request(ClientAPI::ExternalPKISignRequest &req) override {
        req.error = true; req.errorText = "external PKI not supported";
    }
};

std::string read_file(const char *path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Parse KEY=VALUE lines from ports.conf into the proxy port globals.
void load_ports(const char *path) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
        if (v.empty()) continue;
        if (k == "HTTP_PORT") g_http_port = v;
        else if (k == "SOCKS_PORT") g_socks_port = v;
    }
}

} // namespace

int main(int argc, char **argv) {
    openlog("OpenVPN", LOG_PID, LOG_USER);
    signal(SIGPIPE, SIG_IGN);
    // Become a process-group leader so the bridge can kill this client AND its
    // netstack sidecar together (kill(-pgid)), preventing orphaned sidecars
    // that would otherwise keep host proxy ports bound after a restart.
    setpgid(0, 0);

    if (argc < 5) {
        syslog(LOG_ERR, "usage: tun_probe <ovpn> <creds> <ports> <status>");
        return 2;
    }
    const char *ovpn_path = argv[1];
    const char *creds_path = argv[2];
    const char *ports_path = argv[3];
    g_status_path = argv[4];

    load_ports(ports_path);

    std::string profile = read_file(ovpn_path);
    if (profile.empty()) {
        syslog(LOG_ERR, "no OpenVPN profile configured yet (upload one in the app settings)");
        { std::lock_guard<std::mutex> lk(g_status_mtx); g_state = "waiting_config"; }
        write_status();
        return 1;
    }

    VPNClient client;

    ClientAPI::Config config;
    config.content = profile;
    config.guiVersion = "AxisOpenVPN 0.1.0";
    config.tunPersist = false;
    config.connTimeout = 0;
    config.dco = false;

    ClientAPI::EvalConfig ec = client.eval_config(config);
    if (ec.error) {
        syslog(LOG_ERR, "profile error: %s", ec.message.c_str());
        { std::lock_guard<std::mutex> lk(g_status_mtx); g_state = "config_error"; g_last_error = ec.message; }
        write_status();
        return 1;
    }

    if (!ec.autologin) {
        std::string creds = read_file(creds_path);
        std::string user, pass;
        size_t nl = creds.find('\n');
        if (nl != std::string::npos) {
            user = creds.substr(0, nl);
            pass = creds.substr(nl + 1);
            while (!pass.empty() && (pass.back() == '\n' || pass.back() == '\r')) pass.pop_back();
        }
        ClientAPI::ProvideCreds pc;
        pc.username = user;
        pc.password = pass;
        client.provide_creds(pc);
    }

    { std::lock_guard<std::mutex> lk(g_status_mtx); g_state = "connecting"; }
    write_status();

    syslog(LOG_INFO, "connecting (userspace, no root)...");
    ClientAPI::Status st = client.connect(); // blocks until disconnect
    syslog(LOG_INFO, "connect() returned: error=%d message=%s", st.error, st.message.c_str());

    { std::lock_guard<std::mutex> lk(g_status_mtx); g_state = "disconnected"; if (st.error) g_last_error = st.message; }
    write_status();
    return st.error ? 1 : 0;
}
