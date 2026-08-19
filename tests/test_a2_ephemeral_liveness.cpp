#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include "../bs-protocol.h"

#include <chrono>
#include <filesystem>

using namespace bs::mesh;
namespace fs = std::filesystem;

namespace {

fs::path unique_temp_dir(const char* label) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = fs::temp_directory_path() /
        (std::string("bs_a2_") + label + "_" + std::to_string(stamp));
    fs::create_directories(path);
    return path;
}

} // namespace

TEST_CASE("quiet running ephemeral session is not pruned", "[a2][session][prune]") {
    SessionRegistry registry;
#ifdef _WIN32
    const std::string command = "cmd.exe /c ping -n 30 127.0.0.1 >NUL";
#else
    const std::string command = "sleep 30";
#endif
    auto* session = registry.attach(
        "cmd-a2-quiet",
        ResolvedSessionCommand{command, SessionCommandSource::ClientOverride},
        80, 24, "xterm-256color");
    REQUIRE(session != nullptr);
    session->state = SessionState::Detached;
    session->attachments.clear();
    session->last_attach_at =
        std::chrono::steady_clock::now() - std::chrono::minutes(5);
    session->last_output_at = session->last_attach_at;

    registry.prune_ephemeral_sessions(std::chrono::seconds(1));

    REQUIRE(registry.get("cmd-a2-quiet") == session);
#ifdef _WIN32
    REQUIRE(WaitForSingleObject(session->child_pid, 0) == WAIT_TIMEOUT);
#else
    REQUIRE(::kill(session->child_pid, 0) == 0);
#endif
}

TEST_CASE("stale exec watchdog preserves a quiet live attached child",
          "[a2][exec][watchdog]") {
    const auto home = unique_temp_dir("watchdog");
    MeshConfig cfg;
    {
        MeshController controller(cfg, home.string());
#ifdef _WIN32
        const std::string command = "cmd.exe /c ping -n 30 127.0.0.1 >NUL";
#else
        const std::string command = "sleep 30";
#endif
        auto* session = controller.sessions().attach(
            "cmd-a2-watchdog",
            ResolvedSessionCommand{command, SessionCommandSource::ClientOverride},
            80, 24, "xterm-256color");
        REQUIRE(session != nullptr);

#ifndef _WIN32
        int sockets[2] = {-1, -1};
        REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
#endif
        MeshController::Conn connection;
        connection.peer_name = "quiet-live-peer";
        connection.attached_session = session;
#ifdef _WIN32
        connection.sock_fd = INVALID_SOCKET;
#else
        connection.sock_fd = sockets[0];
#endif
        connection.exec_busy->store(true);
        connection.exec_cancelled->store(false);
        connection.exec_started_at =
            std::chrono::steady_clock::now() - std::chrono::minutes(5);
        connection.exec_last_progress_at->store(
            connection.exec_started_at.time_since_epoch().count());
        const auto index = controller.add_connection_for_test(std::move(connection));

        controller.check_stale_exec_for_test();

        REQUIRE_FALSE(controller.exec_cancelled_for_test(index));
        REQUIRE_FALSE(controller.conn_close_requested_for_test("quiet-live-peer"));
#ifndef _WIN32
        CLOSESOCK(sockets[1]);
#endif
    }
    fs::remove_all(home);
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
