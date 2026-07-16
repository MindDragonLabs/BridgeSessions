#include "host_config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

namespace bs::client {
namespace {

std::string trim(std::string_view sv) {
    size_t first = 0;
    while (first < sv.size() && std::isspace(static_cast<unsigned char>(sv[first]))) ++first;
    size_t last = sv.size();
    while (last > first && std::isspace(static_cast<unsigned char>(sv[last - 1]))) --last;
    return std::string(sv.substr(first, last - first));
}

std::string expand_home(std::string path) {
    if (path == "~" || path.starts_with("~/")) {
        if (const char* home = std::getenv("HOME"); home && *home) {
            return std::string(home) + path.substr(1);
        }
    }
    return path;
}

std::vector<std::string> split_words(const std::string& line) {
    std::istringstream iss(line);
    std::vector<std::string> out;
    std::string word;
    while (iss >> word) out.push_back(word);
    return out;
}

bool valid_entry(const HostEntry& e) {
    return !e.name.empty() && !e.server.empty();
}

} // namespace

std::string default_hosts_file() {
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::string(home) + "/.bridgesessions/hosts";
    }
    return "/tmp/bs-hosts";
}

std::vector<HostEntry> load_hosts_file(const std::string& path) {
    std::ifstream in(expand_home(path));
    if (!in) return {};

    std::vector<HostEntry> hosts;
    std::string line;
    while (std::getline(in, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        line = trim(line);
        if (line.empty()) continue;

        auto words = split_words(line);
        if (words.size() < 2) continue;

        HostEntry e;
        e.name = words[0];
        e.server = words[1];
        for (size_t i = 2; i < words.size(); ++i) {
            auto eq = words[i].find('=');
            if (eq == std::string::npos) continue;
            auto key = words[i].substr(0, eq);
            auto value = expand_home(words[i].substr(eq + 1));
            if (key == "key") e.key_file = value;
            else if (key == "cert") e.cert_file = value;
            else if (key == "session" || key == "name") e.session_name = value;
        }
        if (valid_entry(e)) hosts.push_back(std::move(e));
    }
    return hosts;
}

bool save_hosts_file(const std::string& path, const std::vector<HostEntry>& hosts) {
    auto expanded = expand_home(path);
    std::error_code ec;
    auto parent = std::filesystem::path(expanded).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);

    auto tmp = expanded + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) return false;
        out << "# bridgesessions hosts: name server[:port] [key=path] [cert=path] [session=name]\n";
        for (const auto& h : hosts) {
            if (!valid_entry(h)) continue;
            out << h.name << ' ' << h.server;
            if (!h.key_file.empty()) out << " key=" << h.key_file;
            if (!h.cert_file.empty()) out << " cert=" << h.cert_file;
            if (!h.session_name.empty()) out << " session=" << h.session_name;
            out << '\n';
        }
        out.flush();
        if (!out) {
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    std::filesystem::rename(tmp, expanded, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

bool upsert_host(const std::string& path, const HostEntry& entry) {
    if (!valid_entry(entry)) return false;
    auto hosts = load_hosts_file(path);
    hosts.erase(std::remove_if(hosts.begin(), hosts.end(), [&](const HostEntry& h) {
        return h.name == entry.name;
    }), hosts.end());
    hosts.push_back(entry);
    return save_hosts_file(path, hosts);
}

bool remove_host(const std::string& path, const std::string& name) {
    auto hosts = load_hosts_file(path);
    hosts.erase(std::remove_if(hosts.begin(), hosts.end(), [&](const HostEntry& h) {
        return h.name == name;
    }), hosts.end());
    return save_hosts_file(path, hosts);
}

std::optional<HostEntry> find_host(const std::vector<HostEntry>& hosts, const std::string& name) {
    auto it = std::find_if(hosts.begin(), hosts.end(), [&](const HostEntry& h) { return h.name == name; });
    if (it == hosts.end()) return std::nullopt;
    return *it;
}

std::optional<std::string> apply_target_and_hosts(
    const std::string& target,
    const std::vector<HostEntry>& hosts,
    ConnectionOptions& opts)
{
    if (target.empty()) return std::nullopt;
    if (auto entry = find_host(hosts, target)) {
        opts.server = entry->server;
        if (opts.key_file.empty()) opts.key_file = entry->key_file;
        if (opts.cert_file.empty()) opts.cert_file = entry->cert_file;
        if (opts.session_name == "default" && !entry->session_name.empty()) opts.session_name = entry->session_name;
        return std::nullopt;
    }

    // ssh-like fallback: an unknown positional target is a hostname or host:port.
    opts.server = target;
    return std::nullopt;
}

} // namespace bs::client
