// bs-sync — BridgeSessions sync spike (2026-08-13)
//
// Standalone proof of the design in docs/bs-sync-design.md:
//   * bidirectional folder sync over direct TLS with certificate pinning
//   * inotify-triggered reconcile (300 ms debounce) + 2 s interval fallback
//   * symmetric index → diff → apply protocol with per-file SHA-256
//   * conflict copies (".conflict-<remote-node>") instead of clobbering
//     files that changed locally after our index was advertised
//   * atomic writes (.tmp-bs + rename)
//
// Spike scope: Linux only. Daemon integration (wire types 0x2C+) is v2.

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <nlohmann/json.hpp>

#include <sys/inotify.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

// wire helpers: 64-bit big-endian (htonl is 32-bit only)
static inline uint64_t htonll(uint64_t v) {
    return ((uint64_t)htonl((uint32_t)(v & 0xFFFFFFFFULL)) << 32) |
           htonl((uint32_t)(v >> 32));
}
static inline uint64_t ntohll(uint64_t v) { return htonll(v); }

// ─────────────────────────── hashing ───────────────────────────

static std::string sha256_hex_bytes(const unsigned char* data, size_t n) {
    unsigned char out[32]; unsigned int olen = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data, n);
    EVP_DigestFinal_ex(ctx, out, &olen);
    EVP_MD_CTX_free(ctx);
    std::ostringstream s; s << std::hex << std::setfill('0');
    for (unsigned i = 0; i < olen; i++) s << std::setw(2) << (unsigned)out[i];
    return s.str();
}

static std::string sha256_hex_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    char buf[1 << 16];
    while (f) {
        f.read(buf, sizeof buf);
        size_t n = (size_t)f.gcount();
        if (n) EVP_DigestUpdate(ctx, buf, n);
    }
    unsigned char out[32]; unsigned int olen = 0;
    EVP_DigestFinal_ex(ctx, out, &olen);
    EVP_MD_CTX_free(ctx);
    std::ostringstream s; s << std::hex << std::setfill('0');
    for (unsigned i = 0; i < olen; i++) s << std::setw(2) << (unsigned)out[i];
    return s.str();
}

// ─────────────────────────── index / walk ───────────────────────────

struct Ent {
    std::string p;
    long long m = 0;             // mtime ns since epoch
    unsigned long long s = 0;    // size
    std::string h;               // sha256 hex
};

static bool ignored_path(const std::string& p) {
    return p.find(".bs-sync") != std::string::npos ||
           p.find(".conflict-") != std::string::npos ||
           p.find(".tmp-bs") != std::string::npos;
}

static std::map<std::string, Ent> walk_dir(const fs::path& root) {
    std::map<std::string, Ent> out;
    fs::directory_options opts = fs::directory_options::skip_permission_denied;
    for (auto it = fs::recursive_directory_iterator(root, opts);
         it != fs::recursive_directory_iterator(); ++it) {
        if (it->is_directory()) {
            if (ignored_path(it->path().filename().string()))
                it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file()) continue;   // spike: skip symlinks/devices
        std::string rel = fs::relative(it->path(), root).generic_string();
        if (ignored_path(rel)) continue;
        Ent e;
        e.p = rel;
        e.m = std::chrono::duration_cast<std::chrono::nanoseconds>(
                 it->last_write_time().time_since_epoch()).count();
        e.s = it->file_size();
        e.h = sha256_hex_file(it->path());
        out[rel] = e;
    }
    return out;
}

// ─────────────────────────── framing ───────────────────────────

enum class FT : uint8_t {
    HELLO = 1, INDEX = 2, OPS = 3, FILEF = 4, DONE = 5, NUDGE = 6, PING = 7, PONG = 8,
};

static bool send_all(SSL* ssl, const void* p, size_t n) {
    const char* c = (const char*)p;
    while (n) {
        int r = SSL_write(ssl, c, (int)std::min<size_t>(n, 1 << 20));
        if (r <= 0) return false;
        c += r; n -= (size_t)r;
    }
    return true;
}

static bool recv_all(SSL* ssl, void* p, size_t n) {
    char* c = (char*)p;
    while (n) {
        int r = SSL_read(ssl, c, (int)std::min<size_t>(n, 1 << 20));
        if (r <= 0) return false;
        c += r; n -= (size_t)r;
    }
    return true;
}

static bool send_frame(SSL* ssl, FT t, const std::string& payload) {
    uint32_t nlen = htonl((uint32_t)payload.size());
    uint8_t tb = (uint8_t)t;
    return send_all(ssl, &nlen, 4) && send_all(ssl, &tb, 1) &&
           send_all(ssl, payload.data(), payload.size());
}

static bool recv_frame(SSL* ssl, FT& t, std::string& payload) {
    uint32_t nlen = 0; uint8_t tb = 0;
    if (!recv_all(ssl, &nlen, 4)) return false;
    nlen = ntohl(nlen);
    if (nlen > (256u << 20)) return false;
    if (!recv_all(ssl, &tb, 1)) return false;
    t = (FT)tb;
    payload.resize(nlen);
    if (nlen && !recv_all(ssl, payload.data(), nlen)) return false;
    return true;
}

// ─────────────────────────── TLS ───────────────────────────

static std::string cert_pin(X509* cert) {
    unsigned char buf[8192];
    unsigned char* p = buf;
    int n = i2d_X509(cert, &p);
    return sha256_hex_bytes(buf, (size_t)n);
}

struct TlsCreds {
    EVP_PKEY* key = nullptr;
    X509* cert = nullptr;
    std::string pin;
};

static bool load_or_make_creds(const fs::path& state, TlsCreds& c) {
    fs::create_directories(state);
    fs::path cp = state / "cert.pem", kp = state / "key.pem", pp = state / "pin";
    if (fs::exists(cp) && fs::exists(kp)) {
        FILE* cf = fopen(cp.c_str(), "r");
        FILE* kf = fopen(kp.c_str(), "r");
        if (cf && kf) {
            c.cert = PEM_read_X509(cf, nullptr, nullptr, nullptr);
            c.key = PEM_read_PrivateKey(kf, nullptr, nullptr, nullptr);
            fclose(cf); fclose(kf);
        }
    }
    if (!c.cert || !c.key) {
        c.key = EVP_EC_gen("prime256v1");
        if (!c.key) return false;
        c.cert = X509_new();
        X509_set_version(c.cert, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(c.cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(c.cert), 0);
        X509_gmtime_adj(X509_getm_notAfter(c.cert), 60LL * 60 * 24 * 3650);
        X509_set_pubkey(c.cert, c.key);
        X509_NAME* name = X509_get_subject_name(c.cert);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   (const unsigned char*)"bs-sync-spike", -1, -1, 0);
        X509_set_issuer_name(c.cert, name);
        if (X509_sign(c.cert, c.key, EVP_sha256()) <= 0) return false;
        FILE* cf = fopen(cp.c_str(), "w");
        FILE* kf = fopen(kp.c_str(), "w");
        if (!cf || !kf) return false;
        PEM_write_X509(cf, c.cert);
        PEM_write_PrivateKey(kf, c.key, nullptr, nullptr, 0, nullptr, nullptr);
        fclose(cf); fclose(kf);
    }
    c.pin = cert_pin(c.cert);
    {
        std::ofstream pf(pp);
        pf << c.pin << "\n";
    }
    return true;
}

// ─────────────────────────── sync engine ───────────────────────────

struct Engine {
    SSL* ssl = nullptr;
    fs::path root;
    fs::path state_dir;
    std::string node, remote;
    std::map<std::string, Ent> index;     // what we advertised last round
    std::set<std::string> seen;           // paths ever observed (tombstone base)
    bool connector = true;                // connector drives rounds
    bool once = false;

    std::mutex m;                          // protects trigger
    std::condition_variable cv;
    bool trigger = false;
    std::atomic<bool> stop{false};

    std::mutex send_m;                     // serializes SSL writes (watcher NUDGE)

    void load_seen() {
        std::ifstream f(state_dir / "seen.json");
        if (!f) return;
        try {
            for (auto& j : json::parse(f)) seen.insert(j.get<std::string>());
        } catch (...) {}
    }

    void save_seen() {
        fs::create_directories(state_dir);
        json arr = json::array();
        for (auto& p : seen) arr.push_back(p);
        std::ofstream f(state_dir / "seen.json");
        f << arr.dump();
    }

    void merge_seen(const std::map<std::string, Ent>& mine) {
        bool changed = false;
        for (auto& [p, e] : mine)
            if (seen.insert(p).second) changed = true;
        if (changed) save_seen();
    }

    void poke() {
        { std::lock_guard<std::mutex> g(m); trigger = true; }
        cv.notify_one();
    }

    bool sframe(FT t, const std::string& payload) {
        std::lock_guard<std::mutex> g(send_m);
        return send_frame(ssl, t, payload);
    }

    bool rframe(FT& t, std::string& payload) { return recv_frame(ssl, t, payload); }

    bool hello() {
        load_seen();
        char host[256] = {0};
        gethostname(host, sizeof host);
        std::string id = std::string(host) + ":" + std::to_string(getpid());
        std::string hello = "1|" + id + "|" + root.filename().string();
        if (!sframe(FT::HELLO, hello)) return false;
        FT t; std::string pl;
        if (!rframe(t, pl) || t != FT::HELLO) return false;
        auto a = pl.find('|'), b = pl.rfind('|');
        if (a == std::string::npos || b == std::string::npos || a == b) return false;
        remote = pl.substr(a + 1, b - a - 1);
        node = id;
        std::cout << "[" << (connector ? "connect" : "listen") << "] peer: " << remote << "\n";
        return true;
    }

    bool read_ent(const json& j, Ent& e) {
        if (!j.contains("p") || !j.contains("m") || !j.contains("s") || !j.contains("h"))
            return false;
        e.p = j["p"].get<std::string>();
        e.m = j["m"].get<long long>();
        e.s = j["s"].get<unsigned long long>();
        e.h = j["h"].get<std::string>();
        return e.p.find("..") == std::string::npos && e.p.find('/') != 0 && !ignored_path(e.p);
    }

    bool apply_put(const Ent& e, const std::string& content) {
        fs::path target = root / e.p;
        std::string want = sha256_hex_bytes((const unsigned char*)content.data(),
                                             content.size());
        if (want != e.h)
            std::cout << "  !! hash mismatch: advertised " << e.h.substr(0, 12)
                      << " body " << want.substr(0, 12) << " for " << e.p << "\n";
        if (fs::exists(target)) {
            // last-writer-wins: reject puts older than our local copy
            long long local_m = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    fs::last_write_time(target).time_since_epoch()).count();
            if (local_m > e.m) {
                std::cout << "  STALE-REJECT " << e.p << " (local newer)\n";
                return true;
            }
            std::string cur = sha256_hex_file(target);
            auto it = index.find(e.p);
            // extra staleness guard: we advertised a newer copy than this put
            if (it != index.end() && it->second.m > e.m) {
                std::cout << "  STALE-REJECT " << e.p << " (advertised newer)\n";
                return true;
            }
            if (it != index.end() && it->second.h != cur) {
                // changed locally since we advertised -> keep local, save theirs
                fs::path cp = root / (e.p + ".conflict-" + remote);
                std::ofstream f(cp, std::ios::binary);
                if (!f) return false;
                f.write(content.data(), (std::streamsize)content.size());
                std::cout << "  CONFLICT " << e.p << " -> " << cp.filename().string() << "\n";
                return true;
            }
        }
        fs::create_directories(target.parent_path());
        fs::path tmp = root / (e.p + ".tmp-bs");
        {
            std::ofstream f(tmp, std::ios::binary);
            if (!f) return false;
            f.write(content.data(), (std::streamsize)content.size());
        }
        fs::rename(tmp, target);
        std::cout << "  PUT " << e.p << " (" << e.s << " B)\n";
        return true;
    }

    bool apply_del(const std::string& p) {
        fs::path target = root / p;
        if (!fs::exists(target)) return true;
        std::string cur = sha256_hex_file(target);
        auto it = index.find(p);
        if (it != index.end() && it->second.h != cur) {
            std::cout << "  KEEP " << p << " (modified locally)\n";
            return true;
        }
        fs::remove(target);
        std::cout << "  DEL " << p << "\n";
        return true;
    }

    // apply remote OPS then FILE bodies until DONE; returns applied/conflicts
    bool apply_remote(std::pair<int,int>& counts) {
        FT t; std::string pl;
        if (!rframe(t, pl) || t != FT::OPS) return false;
        json ops = json::parse(pl);
        std::map<std::string, std::string> bodies;
        size_t puts = 0;
        for (auto& op : ops) {
            Ent e;
            if (op["op"] == "put" && read_ent(op, e)) puts++;
        }
        // collect put bodies
        std::map<std::string, Ent> putmeta;
        for (auto& op : ops) {
            Ent e;
            if (op["op"] == "put" && read_ent(op, e)) putmeta[e.p] = e;
        }
        for (size_t i = 0; i < putmeta.size(); i++) {
            if (!rframe(t, pl) || t != FT::FILEF) return false;
            uint32_t plen = 0;
            if (pl.size() < 4 + 8) return false;
            memcpy(&plen, pl.data(), 4);
            plen = ntohl(plen);
            if (4 + plen + 8 > pl.size()) return false;
            std::string p = pl.substr(4, plen);
            unsigned long long sz = 0;
            memcpy(&sz, pl.data() + 4 + plen, 8);
            sz = ntohll(sz);
            std::string content = pl.substr(4 + plen + 8);
            if (content.size() != sz) return false;
            bodies[p] = content;
        }
        if (!rframe(t, pl) || t != FT::DONE) return false;
        int applied = 0, conflicts = 0;
        for (auto& op : ops) {
            std::string kind = op["op"];
            if (kind == "put") {
                Ent e;
                if (!read_ent(op, e) || !bodies.count(e.p)) return false;
                if (!apply_put(e, bodies[e.p])) return false;
                applied++;
            } else if (kind == "del") {
                std::string p = op["p"];
                if (p.find("..") != std::string::npos) return false;
                if (!apply_del(p)) return false;
                applied++;
            }
        }
        counts = {applied, conflicts};
        return true;
    }

    // send our side of a round: OPS + FILE bodies + DONE; returns put count
    int send_my_ops(const std::map<std::string, Ent>& mine,
                    const std::map<std::string, Ent>& theirs) {
        json ops = json::array();
        std::vector<Ent> puts;
        for (auto& [p, e] : mine) {
            auto it = theirs.find(p);
            bool push = false;
            if (it == theirs.end()) {
                push = true;                       // peer never had it
            } else if (e.h != it->second.h) {
                // last-writer-wins: push only when our copy is newer
                if (e.m > it->second.m) push = true;
                else if (e.m == it->second.m && connector) push = true;  // tiebreak
                if (!push)
                    std::cout << "  << wait " << p << " (their copy newer)\n";
            }
            if (push) {
                if (it != theirs.end())
                    std::cout << "  >> push " << p << " mine=" << e.h.substr(0, 12)
                              << " theirs=" << it->second.h.substr(0, 12) << "\n";
                ops.push_back({{"op", "put"}, {"p", p}, {"m", e.m}, {"s", e.s}, {"h", e.h}});
                puts.push_back(e);
            }
        }
        // tombstone-aware dels: only paths we've actually seen locally vanish,
        // and only while the peer still reports having the file (their next
        // index is the ack — once they've applied the DEL we stop re-sending).
        for (auto& p : seen)
            if (!mine.count(p) && theirs.count(p) && !ignored_path(p))
                ops.push_back({{"op", "del"}, {"p", p}});
        if (!sframe(FT::OPS, ops.dump())) return -1;
        for (auto& e : puts) {
            std::ifstream f(root / e.p, std::ios::binary);
            std::string content((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
            uint32_t plen = htonl((uint32_t)e.p.size());
            uint64_t sz = htonll(e.s);
            std::string body((const char*)&plen, 4);
            body += e.p;
            body.append((const char*)&sz, 8);
            body += content;
            if (!sframe(FT::FILEF, body)) return -1;
        }
        if (!sframe(FT::DONE, "{}")) return -1;
        return (int)puts.size();
    }

    // connector side: one full round
    bool round() {
        auto mine = walk_dir(root);
        merge_seen(mine);
        index = mine;
        json idx = json::array();
        for (auto& [p, e] : mine)
            idx.push_back({{"p", p}, {"m", e.m}, {"s", e.s}, {"h", e.h}});
        if (!sframe(FT::INDEX, idx.dump())) return false;

        FT t; std::string pl;
        // consume NUDGE/PING until the peer INDEX arrives
        for (;;) {
            if (!rframe(t, pl)) return false;
            if (t == FT::INDEX) break;
            if (t == FT::PING) { sframe(FT::PONG, ""); }
        }
        std::map<std::string, Ent> theirs;
        for (auto& j : json::parse(pl)) {
            Ent e;
            if (read_ent(j, e)) theirs[e.p] = e;
        }

        int puts = send_my_ops(mine, theirs);
        if (puts < 0) return false;
        std::pair<int,int> counts;
        if (!apply_remote(counts)) return false;
        std::cout << "[connect] round: sent " << puts << " puts; applied "
                  << counts.first << ", conflicts " << counts.second << "\n";
        return true;
    }

    // listener side: respond to a peer INDEX with a full exchange
    bool respond_to_index(const std::map<std::string, Ent>& theirs) {
        auto mine = walk_dir(root);
        merge_seen(mine);
        index = mine;
        json idx = json::array();
        for (auto& [p, e] : mine)
            idx.push_back({{"p", p}, {"m", e.m}, {"s", e.s}, {"h", e.h}});
        if (!sframe(FT::INDEX, idx.dump())) return false;

        std::pair<int,int> counts;
        if (!apply_remote(counts)) return false;

        // re-walk post-apply and push anything still differing from the peer
        // index (diffing against `theirs` prevents echo of what we just applied)
        auto now = walk_dir(root);
        index = now;
        int puts = send_my_ops(now, theirs);
        if (puts < 0) return false;
        std::cout << "[listen] round: sent " << puts << " puts; applied "
                  << counts.first << ", conflicts " << counts.second << "\n";
        return true;
    }

    void listen_loop() {
        FT t; std::string pl;
        while (!stop) {
            if (!rframe(t, pl)) {
                std::cout << "[listen] peer disconnected (" << ERR_get_error() << ")\n"
                          << std::flush;
                return;
            }
            if (t == FT::INDEX) {
                std::map<std::string, Ent> theirs;
                for (auto& j : json::parse(pl)) {
                    Ent e;
                    if (read_ent(j, e)) theirs[e.p] = e;
                }
                if (!respond_to_index(theirs)) return;
            } else if (t == FT::PING) {
                sframe(FT::PONG, "");
            }
            // NUDGE: no-op here; connector reconciles within its 2s interval
        }
    }

    void connect_loop() {
        while (!stop) {
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait_for(lk, std::chrono::seconds(2), [&] { return trigger || stop.load(); });
                trigger = false;
            }
            if (stop) break;
            if (!round()) { std::cout << "[connect] peer disconnected\n"; return; }
            if (once) break;
        }
    }
};

// ─────────────────────────── watcher (inotify) ───────────────────────────

static void watch_thread(const fs::path& root, Engine* eng, bool nudger) {
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) return;   // interval fallback still works
    std::map<int, std::string> dirs;
    std::function<void(const fs::path&)> add =
        [&](const fs::path& d) {
            if (ignored_path(d.filename().string())) return;
            int wd = inotify_add_watch(fd, d.c_str(),
                IN_CREATE | IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE |
                IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB);
            if (wd >= 0) dirs[wd] = d.string();
        };
    add(root);
    for (auto it = fs::recursive_directory_iterator(root,
             fs::directory_options::skip_permission_denied);
         it != fs::recursive_directory_iterator(); ++it)
        if (it->is_directory() && !ignored_path(it->path().filename().string()))
            add(it->path());

    char buf[64 * 1024];
    while (!eng->stop) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
            bool dirty = false;
            for (char* p = buf; p < buf + n;) {
                auto* ev = (struct inotify_event*)p;
                if (ev->mask & (IN_CREATE | IN_MOVED_TO) && (ev->mask & IN_ISDIR)) {
                    auto it = dirs.find(ev->wd);
                    if (it != dirs.end()) add(fs::path(it->second) / ev->name);
                }
                std::string name = ev->len ? ev->name : "";
                if (!ignored_path(name)) dirty = true;
                p += sizeof(struct inotify_event) + ev->len;
            }
            if (dirty) {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                if (nudger) {
                    std::lock_guard<std::mutex> g(eng->send_m);
                    send_frame(eng->ssl, FT::NUDGE, "");
                } else {
                    eng->poke();
                }
            }
        } else if (n < 0 && errno != EAGAIN) {
            break;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    close(fd);
}

// ─────────────────────────── main ───────────────────────────

static void usage(const char* a0) {
    std::cerr << "usage:\n"
              << "  " << a0 << " listen <dir> --bind <ip> --port <p> [--state <dir>]\n"
              << "  " << a0 << " connect <host:port> <dir> [--state <dir>] [--pin <hex>] [--once]\n";
}

int main(int argc, char** argv) {
    if (argc < 3) { usage(argv[0]); return 2; }
    std::string mode = argv[1];
    fs::path dir;
    std::string addr, bind_ip = "0.0.0.0";
    int port = 0;
    fs::path state;
    std::string pin_arg;
    bool once = false;

    if (mode == "listen") {
        dir = argv[2];
    } else if (mode == "connect") {
        if (argc < 4) { usage(argv[0]); return 2; }
        addr = argv[2];
        dir = argv[3];
    } else {
        usage(argv[0]);
        return 2;
    }

    int flag_start = (mode == "listen") ? 3 : 4;
    for (int i = flag_start; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--bind") bind_ip = next();
        else if (a == "--port") port = std::stoi(next());
        else if (a == "--state") state = next();
        else if (a == "--pin") pin_arg = next();
        else if (a == "--once") once = true;
        else { std::cerr << "unknown arg: " << a << "\n"; return 2; }
    }
    if (state.empty()) state = dir / ".bs-sync-state";

    SSL_library_init();
    SSL_load_error_strings();

    TlsCreds creds;
    if (!load_or_make_creds(state, creds)) {
        std::cerr << "failed to load/make TLS creds\n"; return 1;
    }
    std::cout << "bs-sync pin: " << creds.pin << "\n";

    if (mode == "listen") {
        if (port == 0) { std::cerr << "--port required\n"; return 2; }
        int sfd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)port);
        inet_pton(AF_INET, bind_ip.c_str(), &sa.sin_addr);
        if (bind(sfd, (sockaddr*)&sa, sizeof sa) || listen(sfd, 4)) {
            perror("listen"); return 1;
        }
        std::cout << "[listen] " << bind_ip << ":" << port << " serving " << dir << "\n";
        for (;;) {
            int cfd = accept(sfd, nullptr, nullptr);
            if (cfd < 0) continue;
            SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
            SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
            SSL_CTX_use_certificate(ctx, creds.cert);
            SSL_CTX_use_PrivateKey(ctx, creds.key);
            SSL* ssl = SSL_new(ctx);
            SSL_set_fd(ssl, cfd);
            if (SSL_accept(ssl) != 1) { SSL_free(ssl); SSL_CTX_free(ctx); close(cfd); continue; }
            auto* eng = new Engine;
            eng->ssl = ssl;
            eng->root = dir;
            eng->state_dir = dir / ".bs-sync-state";
            eng->connector = false;
            std::thread([eng, ssl, ctx, cfd] {
                if (eng->hello()) {
                    std::thread watcher([eng] { watch_thread(eng->root, eng, true); });
                    eng->listen_loop();
                    eng->stop = true;
                    watcher.join();
                }
                SSL_free(ssl);
                SSL_CTX_free(ctx);
                close(cfd);
                delete eng;
            }).detach();
        }
    } else if (mode == "connect") {
        if (addr.empty()) { std::cerr << "peer host:port required\n"; return 2; }
        auto colon = addr.rfind(':');
        std::string host = addr.substr(0, colon), port_s = addr.substr(colon + 1);
        addrinfo hints{}, *res = nullptr;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res) || !res) {
            std::cerr << "resolve failed: " << host << "\n"; return 1;
        }
        int sfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (connect(sfd, res->ai_addr, res->ai_addrlen)) { perror("connect"); return 1; }
        freeaddrinfo(res);

        SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
        SSL* ssl = SSL_new(ctx);
        SSL_set_fd(ssl, sfd);
        SSL_set_tlsext_host_name(ssl, host.c_str());
        if (SSL_connect(ssl) != 1) {
            std::cerr << "tls handshake failed\n";
            ERR_print_errors_fp(stderr);
            return 1;
        }
        X509* peer = SSL_get1_peer_certificate(ssl);
        std::string got = peer ? cert_pin(peer) : "";
        X509_free(peer);
        fs::path pinfile = state / "peer-pin";
        std::string saved;
        {
            std::ifstream f(pinfile);
            if (f) std::getline(f, saved);
        }
        if (!pin_arg.empty()) {
            if (pin_arg != got) { std::cerr << "PIN MISMATCH: expected " << pin_arg << " got " << got << "\n"; return 1; }
        } else if (!saved.empty()) {
            if (saved != got) { std::cerr << "PIN MISMATCH (peer key changed!): " << got << "\n"; return 1; }
        } else {
            std::ofstream f(pinfile);
            f << got << "\n";
            std::cout << "TOFU: pinned new peer " << got << "\n";
        }

        Engine eng;
        eng.ssl = ssl;
        eng.root = dir;
        eng.state_dir = dir / ".bs-sync-state";
        eng.connector = true;
        eng.once = once;
        if (!eng.hello()) { std::cerr << "hello failed\n"; return 1; }
        std::thread watcher([&eng] { watch_thread(eng.root, &eng, false); });
        eng.connect_loop();
        eng.stop = true;
        watcher.join();
        std::cout << "[connect] done\n";
    } else {
        usage(argv[0]);
        return 2;
    }
    return 0;
}
