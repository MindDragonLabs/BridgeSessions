#include "bridge_native/client.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#include <process.h>
static int process_id() { return _getpid(); }
#else
#include <unistd.h>
static int process_id() { return getpid(); }
#endif

using bridge_native::BridgePanelClient;
using bridge_native::ClientOptions;
using bridge_native::Result;

namespace {

std::string env_or_empty(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

template <typename T>
bool report_failure(const char* operation, const Result<T>& result) {
    if (result.ok()) return false;
    std::cerr << operation << " failed: " << result.error().code << ": " << result.error().message << "\n";
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string url = env_or_empty("BRIDGEPANEL_URL");
    std::string token = env_or_empty("BRIDGEPANEL_TOKEN");
    std::string machine = env_or_empty("BRIDGEPANEL_MACHINE");
    std::string name = "native-phase0-" + std::to_string(process_id());
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        std::string* destination = argument == "--url" ? &url :
                                   argument == "--machine" ? &machine :
                                   argument == "--name" ? &name : nullptr;
        if (!destination || index + 1 >= argc) {
            std::cerr << "Use --url, --machine, or --name; credentials require BRIDGEPANEL_TOKEN.\n";
            return 2;
        }
        *destination = argv[++index];
    }
    if (url.empty() || token.empty() || machine.empty()) {
        std::cerr << "Set BRIDGEPANEL_URL, BRIDGEPANEL_TOKEN, and BRIDGEPANEL_MACHINE.\n";
        return 2;
    }

    BridgePanelClient client(ClientOptions{url, token, {5000, true, {}, {}, {32 * 1024, 8 * 1024 * 1024, 8 * 1024 * 1024}}});
    auto capabilities = client.capabilities();
    if (report_failure("capabilities", capabilities)) return 1;
    auto peers = client.peers();
    if (report_failure("peers", peers)) return 1;
    auto sessions = client.sessions(machine);
    if (report_failure("sessions", sessions)) return 1;
    std::cout << "capabilities ok\n";

    auto created = client.create_session(machine, name, "cat", 80, 24);
    if (report_failure("create session", created)) return 1;
    const std::string marker = "bridge-native-phase0-" + std::to_string(process_id()) + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto input = client.send_input(machine, name, marker + "\n");
    if (report_failure("input", input)) {
        client.kill_session(machine, name);
        return 1;
    }

    bool found = false;
    std::size_t offset = 0;
    std::string suffix;
    for (int attempt = 0; attempt < 20 && !found; ++attempt) {
        auto output = client.read_output(machine, name, offset, 64 * 1024);
        if (report_failure("output", output)) break;
        if (output.value().reset) suffix.clear();
        const std::string& bytes = output.value().bytes;
        const std::string combined = suffix + bytes;
        if (combined.find(marker) != std::string::npos) found = true;
        const std::size_t keep = marker.size() - 1;
        suffix = combined.size() > keep ? combined.substr(combined.size() - keep) : combined;
        offset = output.value().offset;
        if (!found) std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    auto killed = client.kill_session(machine, name);
    if (report_failure("kill session", killed)) return 1;
    if (!found) {
        std::cerr << "output did not contain the unique marker\n";
        return 1;
    }
    std::cout << "authenticated session round-trip ok; session=" << name << "\n";
    return 0;
}
