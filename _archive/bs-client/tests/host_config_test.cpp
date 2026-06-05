#include <catch2/catch_test_macros.hpp>
#include "host_config.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace bs::client;

namespace {
std::filesystem::path temp_hosts_path(const char* name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove(p);
    return p;
}
} // namespace

TEST_CASE("HostConfig: parses ssh-like aliases", "[host_config]") {
    auto path = temp_hosts_path("bs-hosts-parse.txt");
    std::ofstream(path) << "# comment\n"
                        << "linux-a 203.0.113.11:9948 key=/tmp/k cert=/tmp/c session=work\n"
                        << "local 127.0.0.1:9947\n";

    auto hosts = load_hosts_file(path.string());
    REQUIRE(hosts.size() == 2);
    REQUIRE(hosts[0].name == "linux-a");
    REQUIRE(hosts[0].server == "203.0.113.11:9948");
    REQUIRE(hosts[0].key_file == "/tmp/k");
    REQUIRE(hosts[0].cert_file == "/tmp/c");
    REQUIRE(hosts[0].session_name == "work");
    REQUIRE(hosts[1].name == "local");
    REQUIRE(hosts[1].server == "127.0.0.1:9947");
}

TEST_CASE("HostConfig: resolves positional alias like ssh linux-a", "[host_config]") {
    std::vector<HostEntry> hosts{
        HostEntry{.name="linux-a", .server="203.0.113.11:9948", .key_file="/keys/k", .cert_file="/keys/c", .session_name="hermes"}
    };
    ConnectionOptions opts;
    opts.server = "127.0.0.1:9948";
    opts.session_name = "default";

    auto err = apply_target_and_hosts("linux-a", hosts, opts);
    REQUIRE_FALSE(err.has_value());
    REQUIRE(opts.server == "203.0.113.11:9948");
    REQUIRE(opts.key_file == "/keys/k");
    REQUIRE(opts.cert_file == "/keys/c");
    REQUIRE(opts.session_name == "hermes");
}

TEST_CASE("HostConfig: command line flags override alias defaults", "[host_config]") {
    std::vector<HostEntry> hosts{
        HostEntry{.name="linux-a", .server="203.0.113.11:9948", .key_file="/keys/k", .cert_file="/keys/c", .session_name="hermes"}
    };
    ConnectionOptions opts;
    opts.server = "127.0.0.1:9948";
    opts.session_name = "override";
    opts.key_file = "/explicit/k";
    opts.cert_file = "/explicit/c";

    auto err = apply_target_and_hosts("linux-a", hosts, opts);
    REQUIRE_FALSE(err.has_value());
    REQUIRE(opts.server == "203.0.113.11:9948");
    REQUIRE(opts.key_file == "/explicit/k");
    REQUIRE(opts.cert_file == "/explicit/c");
    REQUIRE(opts.session_name == "override");
}

TEST_CASE("HostConfig: unknown positional target becomes host[:port]", "[host_config]") {
    ConnectionOptions opts;
    opts.server = "127.0.0.1:9948";
    opts.session_name = "default";

    auto err = apply_target_and_hosts("example.internal:9950", {}, opts);
    REQUIRE_FALSE(err.has_value());
    REQUIRE(opts.server == "example.internal:9950");
}

TEST_CASE("HostConfig: upsert and remove host entries", "[host_config]") {
    auto path = temp_hosts_path("bs-hosts-upsert.txt");
    REQUIRE(upsert_host(path.string(), HostEntry{.name="linux-a", .server="203.0.113.11:9948"}));
    REQUIRE(upsert_host(path.string(), HostEntry{.name="linux-a", .server="127.0.0.1:9947", .session_name="smoke"}));

    auto hosts = load_hosts_file(path.string());
    REQUIRE(hosts.size() == 1);
    REQUIRE(hosts[0].server == "127.0.0.1:9947");
    REQUIRE(hosts[0].session_name == "smoke");

    REQUIRE(remove_host(path.string(), "linux-a"));
    REQUIRE(load_hosts_file(path.string()).empty());
}
