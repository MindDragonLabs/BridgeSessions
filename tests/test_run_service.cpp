#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>

#include "../bs-protocol.h"

using namespace bs::mesh::runsrv;

namespace {

// Minimal valid state-file content through the real save path.
std::string temp_path(const char* tag) {
    static unsigned seq = 0;
    return "/tmp/bs-run-test-" + std::string(tag) + "-" +
           std::to_string(getpid()) + "-" + std::to_string(++seq) + ".json";
}

RunServiceState sample(const std::string& name) {
    RunServiceState s;
    s.name = name;
    s.command = "./agent.sh";
    s.desired = "running";
    s.pid = 4242;
    s.created_at = run_now_unix();
    return s;
}

} // namespace

TEST_CASE("run service names are restricted to safe charset", "[run]") {
    REQUIRE(run_service_name_valid("worker"));
    REQUIRE(run_service_name_valid("worker-1.node:aux"));
    REQUIRE_FALSE(run_service_name_valid(""));
    REQUIRE_FALSE(run_service_name_valid(std::string(65, 'a')));
    REQUIRE_FALSE(run_service_name_valid("../escape"));
    REQUIRE_FALSE(run_service_name_valid("space name"));
}

TEST_CASE("run service state round-trips through the real save/load path",
          "[run]") {
    auto path = temp_path("roundtrip");
    std::vector<RunServiceState> out;
    {
        std::vector<RunServiceState> in{sample("worker"), sample("aux"),
                                        sample("watcher-2")};
        in[1].desired = "stopped";
        in[2].restart_count = 3;
        in[2].next_start_after = run_now_unix() + 30;
        REQUIRE(save_run_services(path, in));
        out = load_run_services(path);
    }
    REQUIRE(out.size() == 3);
    REQUIRE(out[0].name == "worker");
    REQUIRE(out[0].desired == "running");
    REQUIRE(out[1].desired == "stopped");
    REQUIRE(out[2].restart_count == 3);
    REQUIRE(out[2].next_start_after == out[0].created_at + 30);
    std::remove(path.c_str());
}

TEST_CASE("corrupt run-service state is quarantined, not silently dropped",
          "[run]") {
    auto path = temp_path("corrupt");
    {
        std::ofstream f(path);
        f << "{ this is not json";
    }
    const auto loaded = load_run_services(path);
    REQUIRE(loaded.empty());
    REQUIRE_FALSE(std::ifstream(path).good());   // renamed away
    REQUIRE(std::ifstream(path + ".corrupt").good());  // evidence kept
    std::remove((path + ".corrupt").c_str());
}

TEST_CASE("supervision: clean exit and operator stop leave the service dead",
          "[run]") {
    auto s = sample("worker");
    auto clean = run_supervise_after_exit(s, 0, /*clean_exit=*/true, 1000);
    REQUIRE_FALSE(clean.restart);
    REQUIRE_FALSE(clean.give_up);
    s.desired = "stopped";
    auto stopped = run_supervise_after_exit(s, 1, /*clean_exit=*/false, 1000);
    REQUIRE_FALSE(stopped.restart);
}

TEST_CASE("supervision: crash restarts with exponential backoff, gives up after 10",
          "[run]") {
    auto s = sample("worker");
    int64_t now = 1000;
    int last_backoff = 0;
    for (int attempt = 1; attempt <= kRunRestartGiveUp + 2; ++attempt) {
        auto r = run_supervise_after_exit(s, 1, /*clean_exit=*/false, now);
        s.restart_count = r.next_restart_count;
        if (attempt <= kRunRestartGiveUp) {
            REQUIRE(r.restart);
            REQUIRE_FALSE(r.give_up);
            REQUIRE(r.backoff_secs >= last_backoff);           // non-decreasing
            REQUIRE(r.backoff_secs <= kRunRestartMaxBackoffSecs);  // capped
            last_backoff = r.backoff_secs;
        } else {
            REQUIRE(r.give_up);
            REQUIRE_FALSE(r.restart);
        }
    }
    REQUIRE(last_backoff == kRunRestartMaxBackoffSecs);
}

TEST_CASE("argv joins into a single-quote-escaped shell line", "[run]") {
    REQUIRE(run_join_command({}) .empty());
    REQUIRE(run_join_command({"./agent.sh"}) == "'./agent.sh'");
    auto q = run_join_command({"echo", "it's", "a b"});
    REQUIRE(q.find("'it'\\''s'") != std::string::npos);  // embedded quote escaped
    REQUIRE(q.find("'a b'") != std::string::npos);       // space kept in one word
}

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
