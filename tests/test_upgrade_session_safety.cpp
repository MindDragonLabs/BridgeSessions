// test_upgrade_session_safety.cpp — regression tests for the 2026-09-03 fecv3
// upgrade incident. Contract: `bs upgrade` must NEVER kill a live session
// shell, and the binary swap must never touch the inode a running process
// holds (ETXTBSY), nor race the service manager's restart.
//
// Layers covered:
//   1. rename(2) swap semantics — new inode, old inode keeps running procs.
//   2. Upgrade stop path uses no pkill (pkill would SIGKILL workers, and one
//      of them can be the session hosting the upgrade itself).
//   3. pause/resume keep the unit ENABLED persistently (the 2026-08-25
//      disable-left-a-peer-dead bug) and mask only --runtime during swap.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <filesystem>

#ifndef BRIDGESESSIONS_MAIN_CPP_PATH
#define BRIDGESESSIONS_MAIN_CPP_PATH "main.cpp"
#endif

#if !defined(_WIN32)
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

std::string read_file_or_empty(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("upgrade never pkill -9s the daemon or session workers",
          "[upgrade][safety][p1]") {
    // The upgrade stop path in main.cpp must rely on the service manager's
    // graceful stop only. A pkill -9 here historically:
    //   - SIGKILLed the daemon before it could save_persisted_sessions(),
    //   - raced `Restart=on-failure` (old binary could re-exec mid-swap),
    //   - and, when the operator adapted it to "kill workers too", killed the
    //     session-worker hosting the upgrade's own terminal (fecv3, 2026-09-03).
    const std::string main_cpp = read_file_or_empty(BRIDGESESSIONS_MAIN_CPP_PATH);
    REQUIRE_FALSE(main_cpp.empty());

    const std::string stop_marker = "Stop daemon before swap";
    const auto stop_pos = main_cpp.find(stop_marker);
    REQUIRE(stop_pos != std::string::npos);
    // Scan from the upgrade stop block to the swap block: no pkill allowed.
    const auto swap_pos = main_cpp.find("Swapping binary", stop_pos);
    REQUIRE(swap_pos != std::string::npos);
    const std::string stop_block =
        main_cpp.substr(stop_pos, swap_pos - stop_pos);
    // The word may legitimately appear in the explanatory comment; strip
    // comments (naive: drop any line containing //) before asserting that no
    // pkill COMMAND survives in executable code.
    std::string code_only;
    {
        std::istringstream ss(stop_block);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.find("//") == std::string::npos)
                code_only += line + "\n";
        }
    }
    INFO("upgrade stop block executable code contains pkill: " << code_only);
    REQUIRE(code_only.find("pkill") == std::string::npos);
    // The graceful path must go through the service unit (pause_mesh_daemon),
    // and must wait for real daemon exit rather than a blind sleep(2).
    REQUIRE(stop_block.find("pause_mesh_daemon()") != std::string::npos);
    REQUIRE(stop_block.find("usleep") != std::string::npos);
}

TEST_CASE("swap uses rename(2); in-place copy onto the live path is banned",
          "[upgrade][safety][p1]") {
    const std::string main_cpp = read_file_or_empty(BRIDGESESSIONS_MAIN_CPP_PATH);
    REQUIRE_FALSE(main_cpp.empty());

    const auto swap_pos = main_cpp.find("Atomic swap: install via");
    REQUIRE(swap_pos != std::string::npos);
    // End the swap block before the macOS .app re-bundle (which legitimately
    // shells out to codesign) — we only assert on the swap itself.
    const auto app_block = main_cpp.find("#ifdef __APPLE__", swap_pos);
    const auto restart_pos = main_cpp.find("Starting daemon", swap_pos);
    const auto block_end =
        (app_block != std::string::npos && app_block < restart_pos) ? app_block
                                                                    : restart_pos;
    REQUIRE(block_end != std::string::npos);
    const std::string swap_block =
        main_cpp.substr(swap_pos, block_end - swap_pos);

    // cp '<tmp>' '<bin_path>' in-place is the ETXTBSY bug. The block must not
    // shell out to cp at all (staged fallback uses std::filesystem::copy_file
    // onto a staging path + rename, never onto bin_path directly). Same
    // comment-stripping rule as the stop-block check above.
    std::string swap_code;
    {
        std::istringstream ss(swap_block);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.find("//") == std::string::npos)
                swap_code += line + "\n";
        }
    }
    REQUIRE(swap_code.find("cp ") == std::string::npos);
    REQUIRE(swap_code.find("system(") == std::string::npos);
    REQUIRE(swap_block.find("::rename(") != std::string::npos);
    REQUIRE(swap_block.find("upg-new") != std::string::npos);
}

#if !defined(_WIN32)
TEST_CASE("rename(2) swap leaves the old inode running (no ETXTBSY)",
          "[upgrade][safety][posix]") {
    // Prove the core mechanism the safe swap relies on: a process exec'ing
    // file A keeps running after A is renamed away and a new file lands at
    // the same path — while an in-place open for write on that path (what
    // cp does) fails with ETXTBSY while the process lives.
    //
    // The child must be a real ELF: a #!/bin/sh script is INTERPRETED (the
    // kernel execs /bin/sh, the interpreter reads the script through a
    // ordinary fd) so it does not pin the script inode as busy.
    char dir_tmpl[] = "/tmp/bs_upg_test_XXXXXX";
    const char* dir = mkdtemp(dir_tmpl);
    REQUIRE(dir != nullptr);
    const std::string a = std::string(dir) + "/bridgesessions";
    const std::string staged = std::string(dir) + "/bridgesessions.upg-new";

    // "Binary" A: a compiled 30s-sleeper so the kernel holds it ETXTBSY.
    const std::string child_src = std::string(dir) + "/child.c";
    {
        std::ofstream f(child_src);
        f << "#include <unistd.h>\nint main(void){ sleep(30); return 0; }\n";
    }
    const std::string cc =
        "cc -o '" + a + "' '" + child_src + "' 2>/dev/null";
    REQUIRE(std::system(cc.c_str()) == 0);

    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::execl(a.c_str(), a.c_str(), (char*)nullptr);
        _exit(127);
    }
    // Let the child reach exec.
    ::usleep(300 * 1000);

    // In-place overwrite (what cp does) must FAIL with ETXTBSY while the
    // child runs — this is the incident.
    const int wfd = ::open(a.c_str(), O_WRONLY | O_TRUNC);
    const bool etxtbsy = (wfd < 0 && errno == ETXTBSY);
    if (wfd >= 0) ::close(wfd);
    REQUIRE(etxtbsy);

    // rename-swap (what the fixed upgrade does) must SUCCEED — new binary is
    // also a compiled ELF that prints a marker.
    {
        const std::string new_src = std::string(dir) + "/new.c";
        std::ofstream f(new_src);
        f << "#include <stdio.h>\n"
             "int main(void){ puts(\"new-binary\"); return 0; }\n";
    }
    REQUIRE(std::system(
                ("cc -o '" + staged + "' '" + dir + "/new.c' 2>/dev/null")
                    .c_str()) == 0);
    const std::string old_p = a + ".old";
    REQUIRE(::rename(a.c_str(), old_p.c_str()) == 0);
    REQUIRE(::rename(staged.c_str(), a.c_str()) == 0);

    // The child is still alive, still on the old inode.
    int st = 0;
    const pid_t r = ::waitpid(pid, &st, WNOHANG);
    REQUIRE(r == 0);  // not exited — session-equivalent keeps running

    // New execs at the path get the new content.
    FILE* p = ::popen(a.c_str(), "r");
    REQUIRE(p != nullptr);
    char buf[32] = {};
    const char* got = ::fgets(buf, sizeof(buf), p);
    const int prc = ::pclose(p);
    REQUIRE(got != nullptr);
    REQUIRE(std::string(buf).find("new-binary") != std::string::npos);
    REQUIRE(WIFEXITED(prc));
    REQUIRE(WEXITSTATUS(prc) == 0);

    ::kill(pid, SIGKILL);
    ::waitpid(pid, &st, 0);

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}
#endif

TEST_CASE("pause keeps the unit persistently enabled; resume unblocks it",
          "[upgrade][safety][p2]") {
    const std::string main_cpp = read_file_or_empty(BRIDGESESSIONS_MAIN_CPP_PATH);
    REQUIRE_FALSE(main_cpp.empty());

    // 2026-08-25 regression: an upgrade left the unit disabled and the peer
    // refused inbound sessions (errno 61). The mask must be --runtime only,
    // and resume must `enable --now` (not just start) so the unit survives
    // a logout/reboot.
    REQUIRE(main_cpp.find("mask --runtime bridgesessions.service") !=
            std::string::npos);
    REQUIRE(main_cpp.find("systemctl --user disable") == std::string::npos);
    REQUIRE(main_cpp.find("enable --now bridgesessions.service") !=
            std::string::npos);
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
