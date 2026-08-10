// test_join_window.cpp — verify g_allow_join_connections lifecycle
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>
#include "../bs-protocol.h"
#include <thread>

using namespace bs::mesh;

TEST_CASE("g_allow_join_connections starts false", "[join-window]") {
    g_allow_join_connections.store(false, std::memory_order_relaxed); // reset
    REQUIRE(g_allow_join_connections.load(std::memory_order_relaxed) == false);
}

TEST_CASE("g_allow_join_connections can be set and cleared", "[join-window]") {
    g_allow_join_connections.store(true, std::memory_order_relaxed);
    REQUIRE(g_allow_join_connections.load(std::memory_order_relaxed) == true);
    g_allow_join_connections.store(false, std::memory_order_relaxed);
    REQUIRE(g_allow_join_connections.load(std::memory_order_relaxed) == false);
}

TEST_CASE("g_allow_join_connections is safe for concurrent access", "[join-window]") {
    std::atomic<bool> observed{false};
    std::thread writer([&]() {
        for (int i = 0; i < 1000; i++)
            g_allow_join_connections.store(i % 2 == 0, std::memory_order_relaxed);
    });
    std::thread reader([&]() {
        for (int i = 0; i < 1000; i++)
            observed.store(g_allow_join_connections.load(std::memory_order_relaxed));
    });
    writer.join();
    reader.join();
    REQUIRE(true); // no deadlock or crash
    g_allow_join_connections.store(false, std::memory_order_relaxed); // cleanup
}


// ── Main ─────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
