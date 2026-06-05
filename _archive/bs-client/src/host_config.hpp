#pragma once

#include <optional>
#include <string>
#include <vector>

namespace bs::client {

struct HostEntry {
    std::string name;
    std::string server;
    std::string key_file;
    std::string cert_file;
    std::string session_name;
};

struct ConnectionOptions {
    std::string server;
    std::string key_file;
    std::string cert_file;
    std::string session_name = "default";
};

[[nodiscard]] std::string default_hosts_file();
[[nodiscard]] std::vector<HostEntry> load_hosts_file(const std::string& path);
[[nodiscard]] bool save_hosts_file(const std::string& path, const std::vector<HostEntry>& hosts);
[[nodiscard]] bool upsert_host(const std::string& path, const HostEntry& entry);
[[nodiscard]] bool remove_host(const std::string& path, const std::string& name);
[[nodiscard]] std::optional<HostEntry> find_host(const std::vector<HostEntry>& hosts, const std::string& name);

// Apply ssh-style positional target. If target matches a configured host alias,
// its server/key/cert/default-session are applied. Otherwise target is treated as
// a raw host or host:port string. Explicit key/cert/session values in opts win.
[[nodiscard]] std::optional<std::string> apply_target_and_hosts(
    const std::string& target,
    const std::vector<HostEntry>& hosts,
    ConnectionOptions& opts);

} // namespace bs::client
