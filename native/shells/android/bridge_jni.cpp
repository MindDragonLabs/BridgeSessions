#include "bridge_native/client.hpp"

#include <jni.h>
#include <cstdlib>

extern "C" JNIEXPORT jstring JNICALL
Java_com_bridgesessions_nativeapp_NativeBridge_capabilities(JNIEnv* env, jobject, jstring machine_string) {
    const char* machine_chars = env->GetStringUTFChars(machine_string, nullptr);
    const char* url = std::getenv("BRIDGEPANEL_URL"); const char* token = std::getenv("BRIDGEPANEL_TOKEN");
    bridge_native::BridgePanelClient client({url ? url : "", token ? token : "", {5000, true}});
    const std::string machine = machine_chars ? machine_chars : "";
    auto capabilities = client.capabilities();
    auto peers = client.peers();
    auto sessions = client.sessions(machine);
    if (machine_chars) env->ReleaseStringUTFChars(machine_string, machine_chars);
    std::string text;
    if (capabilities.ok() && peers.ok() && sessions.ok())
        text = bridge_native::Json{{"capabilities", capabilities.value()}, {"peers", peers.value()}, {"sessions", sessions.value()}}.dump();
    else {
        const auto& error = !capabilities.ok() ? capabilities.error() : (!peers.ok() ? peers.error() : sessions.error());
        text = error.code + ": " + error.message;
    }
    return env->NewStringUTF(text.c_str());
}
