// test_cua_driver.cpp — cua-driver-rs backend seam tests (26.09.28)
//
// Drives the seam with a mock cua-driver script (canned JSON on stdout, args
// logged to a sidecar file) and covers: detection, opt-out, verb mapping,
// capture decode, error propagation, and the timeout path.

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_session.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "../bs-protocol.h"

int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}

namespace {

namespace fs = std::filesystem;
using namespace bs::mesh;

struct MockDriver {
    fs::path dir;
    fs::path script;
    fs::path args_log;

    MockDriver() {
        dir = fs::temp_directory_path() / ("bs-cua-mock-" + std::to_string(::getpid()));
        fs::create_directories(dir);
        script = dir / "cua-driver";
        args_log = dir / "args.log";
        std::ofstream f(script);
        f << "#!/bin/bash\n"
             "echo \"$2|$3\" >> \"" << args_log.string() << "\"\n"
             "case \"$2\" in\n"
             "  get_screen_size)\n"
             "    echo '{\"structuredContent\":{\"width\":2560,\"height\":1440}}' ;;\n"
             "  get_desktop_state)\n"
             // \"png\" in base64 is iVBOR... — use a tiny fixed payload (\"HELLO\" = SEVMTE8=)\n"
             "    echo '{\"structuredContent\":{\"screenshot_png_b64\":\"SEVMTE8=\",\"width\":100,\"height\":50}}' ;;\n"
             "  type_text|press_key|click|scroll)\n"
             "    case \"$3\" in *FAILME*)\n"
             "      echo '{\"isError\":true,\"content\":[{\"type\":\"text\",\"text\":\"boom\"}]}'; exit 0;;\n"
             "    esac\n"
             "    echo '{\"structuredContent\":{\"ok\":true}}' ;;\n"
             "  fail_tool)\n"
             "    echo '{\"isError\":true,\"content\":[{\"type\":\"text\",\"text\":\"boom\"}]}' ;;\n"
             "  slow_tool)\n"
             "    sleep 30 ;;\n"
             "  *)\n"
             "    exit 1 ;;\n"
             "esac\n";
        f.close();
        fs::permissions(script, fs::perms::owner_all, fs::perm_options::add);
    }

    ~MockDriver() { fs::remove_all(dir); }

    std::string logged() const {
        std::ifstream f(args_log);
        return f ? std::string(std::istreambuf_iterator<char>(f), {}) : "";
    }
};

struct EnvGuard {
    std::string name;
    std::optional<std::string> old;
    explicit EnvGuard(const char* n) : name(n) {
        if (const char* v = std::getenv(n)) old = v;
    }
    void set(const std::string& v) { ::setenv(name.c_str(), v.c_str(), 1); }
    void unset() { ::unsetenv(name.c_str()); }
    ~EnvGuard() {
        if (old) ::setenv(name.c_str(), old->c_str(), 1);
        else ::unsetenv(name.c_str());
    }
};

}  // namespace

TEST_CASE("cua-driver seam: absent binary yields nullopt", "[cua][driver]") {
    EnvGuard path("CUA_DRIVER_PATH"), off("BS_CUA_DRIVER");
    path.set("/nonexistent/cua-driver");
    off.unset();
    CuaRequestMsg req{};
    req.action = 0;
    CHECK_FALSE(cua_driver_execute(req).has_value());
}

TEST_CASE("cua-driver seam: BS_CUA_DRIVER=0 disables", "[cua][driver]") {
    MockDriver mock;
    EnvGuard path("CUA_DRIVER_PATH"), off("BS_CUA_DRIVER");
    path.set(mock.script.string());
    off.set("0");
    CuaRequestMsg req{};
    req.action = 0;
    CHECK_FALSE(cua_driver_execute(req).has_value());
}

TEST_CASE("cua-driver seam: screen_info maps width/height", "[cua][driver]") {
    MockDriver mock;
    EnvGuard path("CUA_DRIVER_PATH"), off("BS_CUA_DRIVER");
    path.set(mock.script.string());
    off.unset();
    CuaRequestMsg req{};
    req.action = 0;
    auto resp = cua_driver_execute(req);
    REQUIRE(resp.has_value());
    CHECK(resp->status == 0);
    CHECK(resp->screen_w == 2560);
    CHECK(resp->screen_h == 1440);
    CHECK(mock.logged().find("get_screen_size") != std::string::npos);
}

TEST_CASE("cua-driver seam: capture decodes png payload", "[cua][driver]") {
    MockDriver mock;
    EnvGuard path("CUA_DRIVER_PATH"), off("BS_CUA_DRIVER");
    path.set(mock.script.string());
    off.unset();
    CuaRequestMsg req{};
    req.action = 6;
    auto resp = cua_driver_execute(req);
    REQUIRE(resp.has_value());
    CHECK(resp->status == 0);
    CHECK(resp->format == 1);
    REQUIRE(resp->data.size() == 5);
    CHECK(std::string(resp->data.begin(), resp->data.end()) == "HELLO");
    CHECK(resp->screen_w == 100);
}

TEST_CASE("cua-driver seam: text verb maps to type_text desktop scope", "[cua][driver]") {
    MockDriver mock;
    EnvGuard path("CUA_DRIVER_PATH"), off("BS_CUA_DRIVER");
    path.set(mock.script.string());
    off.unset();
    CuaRequestMsg req{};
    req.action = 2;
    req.text = "hello fleet";
    auto resp = cua_driver_execute(req);
    REQUIRE(resp.has_value());
    CHECK(resp->status == 0);
    CHECK(mock.logged().find("type_text|") != std::string::npos);
    CHECK(mock.logged().find("hello fleet") != std::string::npos);
    CHECK(mock.logged().find("\"scope\":\"desktop\"") != std::string::npos);
}

TEST_CASE("cua-driver seam: HID 0x28 maps to return key", "[cua][driver]") {
    MockDriver mock;
    EnvGuard path("CUA_DRIVER_PATH"), off("BS_CUA_DRIVER");
    path.set(mock.script.string());
    off.unset();
    CuaRequestMsg req{};
    req.action = 1;
    req.hid_key = 0x28;
    req.modifiers = 1;  // ctrl
    auto resp = cua_driver_execute(req);
    REQUIRE(resp.has_value());
    CHECK(resp->status == 0);
    CHECK(mock.logged().find("press_key|") != std::string::npos);
    CHECK(mock.logged().find("\"key\":\"return\"") != std::string::npos);
    CHECK(mock.logged().find("\"ctrl\"") != std::string::npos);
}

TEST_CASE("cua-driver seam: unmapped HID falls through", "[cua][driver]") {
    MockDriver mock;
    EnvGuard path("CUA_DRIVER_PATH"), off("BS_CUA_DRIVER");
    path.set(mock.script.string());
    off.unset();
    CuaRequestMsg req{};
    req.action = 1;
    req.hid_key = 0xE8;  // not in the table
    CHECK_FALSE(cua_driver_execute(req).has_value());
}

TEST_CASE("cua-driver seam: click maps button + coords", "[cua][driver]") {
    MockDriver mock;
    EnvGuard path("CUA_DRIVER_PATH"), off("BS_CUA_DRIVER");
    path.set(mock.script.string());
    off.unset();
    CuaRequestMsg req{};
    req.action = 4;
    req.x = 640;
    req.y = 480;
    req.button = 2;
    auto resp = cua_driver_execute(req);
    REQUIRE(resp.has_value());
    CHECK(resp->status == 0);
    CHECK(mock.logged().find("click|") != std::string::npos);
    CHECK(mock.logged().find("\"button\":\"right\"") != std::string::npos);
    CHECK(mock.logged().find("\"x\":640") != std::string::npos);
}

TEST_CASE("cua-driver seam: tool error surfaces as response error", "[cua][driver]") {
    MockDriver mock;
    EnvGuard path("CUA_DRIVER_PATH"), off("BS_CUA_DRIVER");
    path.set(mock.script.string());
    off.unset();
    CuaRequestMsg req{};
    req.action = 2;
    req.text = "FAILME";  // mock replies isError for this payload
    auto resp = cua_driver_execute(req);
    REQUIRE(resp.has_value());
    CHECK(resp->status == 1);
    CHECK(resp->error == "boom");
}

TEST_CASE("cua-driver seam: hung driver is bounded by timeout", "[cua][driver]") {
    MockDriver mock;
    // Rewrite mock: every tool sleeps.
    {
        std::ofstream f(mock.script);
        f << "#!/bin/bash\nsleep 30\n";
        fs::permissions(mock.script, fs::perms::owner_all, fs::perm_options::add);
    }
    EnvGuard path("CUA_DRIVER_PATH"), off("BS_CUA_DRIVER"), tmo("BS_CUA_DRIVER_TIMEOUT_MS");
    path.set(mock.script.string());
    off.unset();
    tmo.set("800");
    CuaRequestMsg req{};
    req.action = 0;
    auto t0 = std::chrono::steady_clock::now();
    auto resp = cua_driver_execute(req);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    REQUIRE(resp.has_value());
    CHECK(resp->status == 1);
    CHECK(resp->error.find("timed out") != std::string::npos);
    CHECK(ms < 5000);
}
