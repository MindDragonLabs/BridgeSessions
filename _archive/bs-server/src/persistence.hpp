// persistence.hpp — Phase 10: JSON session persistence
// Uses nlohmann/json (header-only, MIT) to save/load session metadata.

#pragma once

#include <chrono>
#include <nlohmann/json.hpp>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <cstdio>
#include <unistd.h>
#endif

namespace bs::server {

struct SessionMeta {
    std::string name;
    std::string owner_id;
    std::string command;
    std::string state;
    std::string created_at;  // ISO 8601
};

// Save session list to JSON file (atomic write: temp + rename)
inline bool save_sessions(const std::string& path,
                          const std::vector<SessionMeta>& sessions) {
    nlohmann::json j = nlohmann::json::array();
    for (auto& s : sessions) {
        nlohmann::json entry;
        entry["name"] = s.name;
        entry["owner_id"] = s.owner_id;
        entry["command"] = s.command;
        entry["state"] = s.state;
        entry["created_at"] = s.created_at;
        j.push_back(entry);
    }

    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) return false;
        f << j.dump(2) << '\n';
        f.flush();
        if (!f) {
            std::filesystem::remove(tmp);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp);
        return false;
    }
    return true;
}

// Load session list from JSON
inline std::vector<SessionMeta> load_sessions(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    try {
        auto j = nlohmann::json::parse(f);
        std::vector<SessionMeta> result;
        for (auto& entry : j) {
            SessionMeta m;
            m.name = entry.value("name", "");
            m.owner_id = entry.value("owner_id", "");
            m.command = entry.value("command", "");
            m.state = entry.value("state", "detached");
            m.created_at = entry.value("created_at", "");
            result.push_back(m);
        }
        return result;
    } catch (...) { return {}; }
}

} // namespace bs::server
