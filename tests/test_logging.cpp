#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>

#include "../bs-logging.h"

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

TEST_CASE("operational log redactor covers structured and CLI secrets", "[logging][redaction]") {
    using bs::log::redact;
    REQUIRE(redact(R"({"token": "json-secret"})") == R"({"token": "[REDACTED]"})");
    REQUIRE(redact("password: yaml-secret") == "password: [REDACTED]");
    REQUIRE(redact("--token cli-secret --start") == "--token [REDACTED] --start");
    REQUIRE(redact("Bearer eyJhbGciOi.secret") == "Bearer [REDACTED]");
}

TEST_CASE("pre-init fallback logger uses a redacting sink", "[logging][redaction]") {
    bs::log::shutdown();
    const std::string name = "fallback-redaction-test";
    auto logger = bs::log::get(name);
    REQUIRE(logger->sinks().size() == 1);
    REQUIRE(dynamic_cast<bs::log::detail::RedactingSink*>(logger->sinks().front().get()) != nullptr);
    spdlog::drop(name);
}
