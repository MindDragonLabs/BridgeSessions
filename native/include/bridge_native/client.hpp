#pragma once

#include "bridge_native/http.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace bridge_native {

using Json = nlohmann::json;

struct ClientOptions {
    std::string base_url;
    std::string bearer_token;
    HttpOptions http;
};

struct OutputChunk {
    // Server's next byte cursor; do not add bytes.size() again.
    std::size_t offset = 0;
    std::string bytes;
    bool reset = false;
};

class BridgePanelClient {
public:
    explicit BridgePanelClient(ClientOptions options);

    Result<Json> capabilities();
    Result<Json> peers();
    Result<Json> sessions(std::string_view machine);
    Result<Json> create_session(std::string_view machine, std::string_view name,
                                std::string_view command, int cols = 80, int rows = 24);
    Result<Json> send_input(std::string_view machine, std::string_view session,
                            std::string_view bytes);
    Result<OutputChunk> read_output(std::string_view machine, std::string_view session,
                                    std::size_t offset, std::size_t limit = 64 * 1024);
    Result<Json> kill_session(std::string_view machine, std::string_view session);

    Result<Json> list_files(std::string_view machine, std::string_view root,
                            std::string_view path);
    Result<Json> read_file(std::string_view machine, std::string_view root,
                           std::string_view path);
    Result<Json> write_file(std::string_view machine, std::string_view root,
                            std::string_view path, std::string_view content);
    Result<Json> agents(std::string_view machine = {});
    Result<Json> start_chat(std::string_view machine, std::string_view agent,
                            std::string_view prompt, std::string_view request_id);
    Result<Json> poll_chat(std::string_view request_id);
    Result<Json> cancel_chat(std::string_view request_id);

    const ClientOptions& options() const { return options_; }

private:
    Result<Json> request_json(std::string method, std::string path,
                              const Json* body = nullptr,
                              std::size_t max_body = 8 * 1024 * 1024);
    Result<Json> decode_json_response(const HttpResponse& response) const;
    static std::string encode_query(std::string_view input);
    static Result<std::string> decode_base64(std::string_view input);

    ClientOptions options_;
    HttpTransport transport_;
};

} // namespace bridge_native
