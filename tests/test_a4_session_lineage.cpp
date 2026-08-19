#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../bs-protocol.h"

#include <chrono>
#include <filesystem>
#include <thread>

using namespace bs::mesh;
namespace fs = std::filesystem;

namespace {

fs::path unique_temp_dir() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = fs::temp_directory_path() /
        ("bs_a4_lineage_" + std::to_string(stamp));
    fs::create_directories(path);
    return path;
}

void set_parent_environment(const char* value) {
#ifdef _WIN32
    _putenv_s("BS_PARENT_SESSION_ID", value ? value : "");
#else
    if (value) setenv("BS_PARENT_SESSION_ID", value, 1);
    else unsetenv("BS_PARENT_SESSION_ID");
#endif
}

const nlohmann::json& session_named(const nlohmann::json& sessions,
                                    const std::string& name) {
    for (const auto& session : sessions) {
        if (session.value("name", "") == name) return session;
    }
    throw std::runtime_error("session not found: " + name);
}

#ifndef _WIN32
std::string read_until(Session& session, std::string_view expected) {
    std::string output;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::array<char, 1024> buffer{};
    while (std::chrono::steady_clock::now() < deadline) {
        const ssize_t n = ::read(session.master_fd, buffer.data(), buffer.size());
        if (n > 0) {
            output.append(buffer.data(), static_cast<size_t>(n));
            if (output.find(expected) != std::string::npos) break;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return output;
}
#endif

} // namespace

TEST_CASE("session parent_id survives gossip round-trip and MESH_TREE",
          "[a4][session][lineage]") {
    set_parent_environment(nullptr);
    const auto home = unique_temp_dir();
    MeshConfig cfg;
    cfg.node_name = "lineage-node";
    {
        MeshController controller(cfg, home.string());

        set_parent_environment("parent-session");
#ifdef _WIN32
        const std::string child_command = "cmd.exe /c ping -n 30 127.0.0.1 >NUL";
#else
        const std::string child_command =
            "printf '%s' \"$BS_SESSION_ID\"; sleep 30";
#endif
        auto* child = controller.sessions().attach(
            "child-session",
            ResolvedSessionCommand{child_command, SessionCommandSource::ClientOverride},
            80, 24, "xterm-256color");
        set_parent_environment(nullptr);
        REQUIRE(child != nullptr);
        REQUIRE(child->parent_id == "parent-session");

#ifndef _WIN32
        const auto child_output = read_until(*child, "child-session");
        REQUIRE(child_output.find("child-session") != std::string::npos);
#endif

#ifdef _WIN32
        const std::string primary_command = "cmd.exe /c ping -n 30 127.0.0.1 >NUL";
#else
        const std::string primary_command = "sleep 30";
#endif
        auto* primary = controller.sessions().attach(
            "primary-session",
            ResolvedSessionCommand{primary_command, SessionCommandSource::ClientOverride},
            80, 24, "xterm-256color");
        REQUIRE(primary != nullptr);
        REQUIRE(primary->parent_id.empty());

        ServerInfoMsg outbound;
        outbound.hostname = cfg.node_name;
        outbound.version = std::string(kBridgeSessionsVersion);
        outbound.sessions_summary_json = controller.sessions_summary_json_for_test();
        const Message decoded_message = decode(encode(Message{outbound}, 0));
        REQUIRE(std::holds_alternative<ServerInfoMsg>(decoded_message));
        const auto gossip = nlohmann::json::parse(
            std::get<ServerInfoMsg>(decoded_message).sessions_summary_json);
        REQUIRE(session_named(gossip, "child-session").at("parent_id") == "parent-session");
        REQUIRE_FALSE(session_named(gossip, "primary-session").contains("parent_id"));

        const auto tree = nlohmann::json::parse(controller.mesh_tree_json_for_test());
        REQUIRE(tree.at("node") == "lineage-node");
        REQUIRE(session_named(tree.at("sessions"), "child-session").at("parent_id") ==
                "parent-session");
        REQUIRE_FALSE(session_named(tree.at("sessions"), "primary-session").contains("parent_id"));
    }
    set_parent_environment(nullptr);
    fs::remove_all(home);
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
