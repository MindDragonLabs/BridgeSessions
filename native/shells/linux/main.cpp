// Linux phase-0 shell: native_phase0_smoke is the supported terminal shell.
// It performs discovery plus an authenticated create/input/output/kill proof;
// no desktop toolkit is introduced for phase 0.
#include "bridge_native/client.hpp"

#include <cstdlib>
#include <iostream>

int main() {
    const char* url = std::getenv("BRIDGEPANEL_URL"); const char* token = std::getenv("BRIDGEPANEL_TOKEN"); const char* machine = std::getenv("BRIDGEPANEL_MACHINE");
    if (!url || !*url || !token || !*token || !machine || !*machine) {
        std::cerr << "Set BRIDGEPANEL_URL, BRIDGEPANEL_TOKEN, and BRIDGEPANEL_MACHINE.\n";
        return 2;
    }
    bridge_native::BridgePanelClient client({url ? url : "", token ? token : "", {5000, true}});
    auto capabilities = client.capabilities();
    auto peers = client.peers();
    auto sessions = client.sessions(machine);
    if (!capabilities.ok() || !peers.ok() || !sessions.ok()) {
        const auto& error = !capabilities.ok() ? capabilities.error() : (!peers.ok() ? peers.error() : sessions.error());
        std::cerr << error.code << ": " << error.message << "\n";
        return 1;
    }
    std::cout << bridge_native::Json{{"capabilities", capabilities.value()}, {"peers", peers.value()}, {"sessions", sessions.value()}}.dump(2) << "\n";
    return 0;
}
