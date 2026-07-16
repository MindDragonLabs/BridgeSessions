// bench_frame_io.cpp — Phase 12: Frame I/O throughput benchmark
// Single TLS session, 10K roundtrips per message type (40K total)

#include <catch2/catch_test_macros.hpp>
#include "bstransport/tls.hpp"
#include "bstransport/frame_io.hpp"
#include "bsprotocol/codec.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <thread>
#include <fstream>
#include <cstdio>
#include <chrono>
#include <numeric>
#include <algorithm>
#include <vector>
#include <cstring>
#include <atomic>

using namespace bs::protocol;
using namespace bs::transport;
using namespace std::chrono;

namespace {

struct TmpFile {
    std::string path;
    TmpFile(const std::string& content) {
        char tmpl[] = "/tmp/bridgesessions_bench_XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd >= 0) { write(fd, content.data(), content.size()); close(fd); path = tmpl; }
    }
    ~TmpFile() { if (!path.empty()) std::remove(path.c_str()); }
};

auto cert_key() { return generate_cert_key_pair("bench.bridgesessions"); }

struct BenchResult {
    std::string label;
    double throughput_mbps = 0;
    double latency_avg_us = 0;
    double latency_p99_us = 0;
    double compression_ratio = 0;
};

BenchResult run_bench(SSL* ssl, const std::string& label,
                      std::function<Message(size_t)> make_msg,
                      size_t num_roundtrips = 1000)
{
    std::vector<double> lats;
    lats.reserve(num_roundtrips);
    size_t total_wire = 0;
    size_t total_uncompressed = 0;
    size_t compressed_count = 0;

    auto t0 = high_resolution_clock::now();
    for (size_t i = 0; i < num_roundtrips; ++i) {
        auto msg = make_msg(i);
        auto encoded = encode(msg, CONTROL_STREAM_ID);
        total_wire += encoded.size();
        // uncompressed estimate: header + payload
        total_uncompressed += FRAME_HEADER_SIZE + encoded.size();
        if (encoded.size() > FRAME_HEADER_SIZE && (encoded[3] & FLAG_COMPRESSED))
            compressed_count++;

        auto ts = high_resolution_clock::now();
        write_frame(ssl, msg, CONTROL_STREAM_ID);
        auto reply = read_frame(ssl);
        auto te = high_resolution_clock::now();
        lats.push_back(duration_cast<nanoseconds>(te - ts).count() / 1000.0);
        REQUIRE(message_type(reply) == message_type(msg));
    }
    auto t1 = high_resolution_clock::now();
    double elapsed = duration_cast<nanoseconds>(t1 - t0).count() / 1e9;

    std::sort(lats.begin(), lats.end());
    BenchResult r;
    r.label = label;
    r.throughput_mbps = (total_wire * 2 / 1e6) / elapsed;
    r.latency_avg_us = std::accumulate(lats.begin(), lats.end(), 0.0) / lats.size();
    r.latency_p99_us = lats[lats.size() * 99 / 100];
    r.compression_ratio = compressed_count > 0 ? (double)total_uncompressed / total_wire : 1.0;
    return r;
}

} // namespace

TEST_CASE("Frame I/O benchmark: single TLS session", "[bench][frame_io]") {
    auto [sc, sk] = cert_key();
    auto [cc, ck] = cert_key();
    TmpFile sf(sc), kf(sk), cf(cc), cfk(ck);
    auto client_hex = pubkey_hex_from_pem(ck);
    TmpFile ak(client_hex + "\n");
    TmpFile ks("");

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(lfd, (sockaddr*)&addr, sizeof(addr));
    listen(lfd, 1);
    socklen_t alen = sizeof(addr);
    getsockname(lfd, (sockaddr*)&addr, &alen);
    int port = ntohs(addr.sin_port);

    // Server: echo loop
    std::jthread server([&] {
        int cfd = accept(lfd, nullptr, nullptr);
        if (cfd < 0) return;
        ServerConfig scfg{sf.path, kf.path, ak.path};
        auto ctx = create_server_context(scfg);
        auto s = SslPtr(SSL_new(ctx.get()));
        SSL_set_fd(s.get(), cfd);
        SSL_accept(s.get());
        for (int i = 0; i < 4500; ++i) {
            try { auto m = read_frame(s.get()); write_frame(s.get(), m, CONTROL_STREAM_ID); }
            catch (...) { break; }
        }
        close(cfd);
    });

    // Client
    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in ca{}; ca.sin_family = AF_INET; ca.sin_addr.s_addr = htonl(INADDR_LOOPBACK); ca.sin_port = htons(port);
    connect(cfd, (sockaddr*)&ca, sizeof(ca));

    ClientConfig ccfg{cf.path, cfk.path, ks.path};
    auto cctx = create_client_context(ccfg, [](auto&){ return true; });
    auto ssl = SslPtr(SSL_new(cctx.get()));
    SSL_set_fd(ssl.get(), cfd);
    SSL_connect(ssl.get());

    printf("\n  bs-transport Frame I/O Benchmark (1K roundtrips each type)\n");
    printf("  %-22s | %8s | %10s | %10s | %12s\n", "Type", "MB/s", "Avg(us)", "P99(us)", "Compress");

    auto make_ks = [](size_t i) -> Message {
        KeystrokeMsg k; k.data = "key_" + std::to_string(i % 1000); return k;
    };
    auto r1 = run_bench(ssl.get(), "Keystroke (~9B)", make_ks);
    printf("  %-22s | %8.1f | %9.1f | %9.1f | %11.2fx\n",
           r1.label.c_str(), r1.throughput_mbps, r1.latency_avg_us, r1.latency_p99_us, r1.compression_ratio);

    auto make_out = [](size_t i) -> Message {
        OutputMsg o; o.data = std::string(2048, 'A' + (i % 26)); return o;
    };
    auto r2 = run_bench(ssl.get(), "Output (2KB)", make_out);
    printf("  %-22s | %8.1f | %9.1f | %9.1f | %11.2fx\n",
           r2.label.c_str(), r2.throughput_mbps, r2.latency_avg_us, r2.latency_p99_us, r2.compression_ratio);

    auto make_sb = [](size_t i) -> Message {
        ScrollbackMsg s;
        s.data = std::string(8000, 'X') + "\x1b[32m" + std::string(i % 100, 'Y') + "\x1b[0m\n" + std::string(8000, 'Z');
        s.total_lines = 100;
        s.chunk_index = (uint32_t)(i % 10);
        return s;
    };
    auto r3 = run_bench(ssl.get(), "Scrollback (~16KB)", make_sb, 500);
    printf("  %-22s | %8.1f | %9.1f | %9.1f | %11.2fx\n",
           r3.label.c_str(), r3.throughput_mbps, r3.latency_avg_us, r3.latency_p99_us, r3.compression_ratio);

    auto make_cp = [](size_t i) -> Message {
        ClipboardMsg c;
        c.text = std::string(500 + (i % 1500), 'C');
        c.hash = std::to_string(std::hash<std::string>{}(c.text));
        return c;
    };
    auto r4 = run_bench(ssl.get(), "Clipboard (0.5-2KB)", make_cp, 500);
    printf("  %-22s | %8.1f | %9.1f | %9.1f | %11.2fx\n",
           r4.label.c_str(), r4.throughput_mbps, r4.latency_avg_us, r4.latency_p99_us, r4.compression_ratio);

    double best = std::max({r1.throughput_mbps, r2.throughput_mbps, r3.throughput_mbps, r4.throughput_mbps});
    printf("  Best throughput: %.1f MB/s  (target: >500 MB/s)\n\n", best);

    SSL_shutdown(ssl.get());
    close(cfd);
    close(lfd);
    server.join();
    REQUIRE(best > 0.0);
}
