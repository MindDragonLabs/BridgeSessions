#include <catch2/catch_test_macros.hpp>
#include "pty.hpp"

#include <fcntl.h>
#include <unistd.h>

using namespace bs::server;

TEST_CASE("PTY master is close-on-exec", "[pty]") {
    auto fd = open_pty();
    REQUIRE(fd.has_value());
    int flags = ::fcntl(*fd, F_GETFD);
    REQUIRE(flags >= 0);
    REQUIRE((flags & FD_CLOEXEC) != 0);
    ::close(*fd);
}
