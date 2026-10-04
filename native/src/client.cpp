#include "bridge_native/client.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>

namespace bridge_native {
namespace {

Error error_from_response(const HttpResponse& response, std::string_view token) {
    Error result{response.status, "http_error", "BridgePanel request failed", response.status == 408 || response.status >= 500};
    try {
        const Json body = Json::parse(response.body);
        if (body.is_object() && body.value("ok", true) == false && body.contains("error")) {
            const auto& error = body.at("error");
            if (error.is_object()) {
                result.code = error.value("code", result.code);
                result.message = error.value("message", result.message);
            } else if (error.is_string()) {
                result.message = error.get<std::string>();
            }
        }
    } catch (...) {
        if (!response.body.empty()) result.message += ": " + response.body.substr(0, 160);
    }
    result.message = HttpTransport::redact(result.message, token);
    return result;
}

bool is_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~';
}

} // namespace

BridgePanelClient::BridgePanelClient(ClientOptions options)
    : options_(std::move(options)), transport_(options_.http) {
    while (!options_.base_url.empty() && options_.base_url.back() == '/') options_.base_url.pop_back();
    if (options_.base_url.size() < 7 ||
        (options_.base_url.find("http://") != 0 && options_.base_url.find("https://") != 0)) {
        options_.base_url.clear();
    }
    if (!options_.bearer_token.empty() && options_.bearer_token.find_first_of("\r\n") != std::string::npos)
        options_.bearer_token.clear();
}

std::string BridgePanelClient::encode_query(std::string_view input) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(input.size());
    for (unsigned char c : input) {
        if (is_unreserved(c)) result.push_back(static_cast<char>(c));
        else { result.push_back('%'); result.push_back(hex[c >> 4]); result.push_back(hex[c & 15]); }
    }
    return result;
}

Result<std::string> BridgePanelClient::decode_base64(std::string_view input) {
    if (input.size() % 4 != 0 || input.size() > 32 * 1024 * 1024)
        return Result<std::string>::failure({0, "invalid_base64", "base64 length is invalid", false});
    if (input.empty()) return Result<std::string>::success({});
    const std::size_t padding = input.ends_with("==") ? 2 : input.ends_with("=") ? 1 : 0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(input[i]);
        const bool alphabet = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                              (c >= '0' && c <= '9') || c == '+' || c == '/';
        if (i < input.size() - padding ? !alphabet : c != '=')
            return Result<std::string>::failure({0, "invalid_base64", "base64 contains an invalid character", false});
    }
    std::string decoded((input.size() / 4) * 3, '\0');
    const int count = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(decoded.data()),
                                      reinterpret_cast<const unsigned char*>(input.data()),
                                      static_cast<int>(input.size()));
    if (count < 0) return Result<std::string>::failure({0, "invalid_base64", "base64 decoding failed", false});
    decoded.resize(static_cast<std::size_t>(count) - padding);
    std::string canonical(input.size(), '\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char*>(canonical.data()),
                    reinterpret_cast<const unsigned char*>(decoded.data()), static_cast<int>(decoded.size()));
    if (canonical != input)
        return Result<std::string>::failure({0, "invalid_base64", "base64 padding bits are invalid", false});
    return Result<std::string>::success(std::move(decoded));
}

Result<Json> BridgePanelClient::decode_json_response(const HttpResponse& response) const {
    if (response.status < 200 || response.status >= 300)
        return Result<Json>::failure(error_from_response(response, options_.bearer_token));
    try {
        Json body = response.body.empty() ? Json::object() : Json::parse(response.body);
        if (!body.is_object()) return Result<Json>::failure({response.status, "invalid_api_response", "API response must be a JSON object", false});
        if (body.value("ok", true) == false) return Result<Json>::failure(error_from_response(response, options_.bearer_token));
        return Result<Json>::success(std::move(body));
    } catch (const std::exception& exception) {
        return Result<Json>::failure({response.status, "invalid_api_response", HttpTransport::redact(exception.what(), options_.bearer_token), false});
    }
}

Result<Json> BridgePanelClient::request_json(std::string method, std::string path,
                                             const Json* body, std::size_t max_body) {
    if (options_.base_url.empty()) return Result<Json>::failure({0, "invalid_config", "base URL is missing or invalid", false});
    if (options_.bearer_token.empty()) return Result<Json>::failure({0, "invalid_config", "bearer token is required", false});
    if (options_.base_url.find(options_.bearer_token) != std::string::npos)
        return Result<Json>::failure({0, "invalid_config", "bearer credential must not appear in the URL", false});
    if (path.empty() || path.front() != '/') return Result<Json>::failure({0, "invalid_request", "API path must be absolute", false});
    HttpRequest request{std::move(method), options_.base_url + path, {}, {}};
    request.headers.emplace("Accept", "application/json");
    request.headers.emplace("Authorization", "Bearer " + options_.bearer_token);
    if (body) {
        request.headers.emplace("Content-Type", "application/json");
        request.body = body->dump();
        if (request.body.size() > 256 * 1024 || request.body.size() > options_.http.limits.max_request_bytes)
            return Result<Json>::failure({0, "request_too_large", "JSON request exceeds limit", false});
    }
    auto response = transport_.perform(request);
    if (!response.ok()) {
        Error error = response.error();
        error.message = HttpTransport::redact(error.message, options_.bearer_token);
        return Result<Json>::failure(std::move(error));
    }
    if (response.value().body.size() > max_body)
        return Result<Json>::failure({response.value().status, "response_too_large", "API response exceeds operation limit", false});
    return decode_json_response(response.value());
}

Result<Json> BridgePanelClient::capabilities() {
    return request_json("GET", "/api/v1/capabilities");
}

Result<Json> BridgePanelClient::peers() {
    return request_json("GET", "/api/v1/peers");
}

Result<Json> BridgePanelClient::sessions(std::string_view machine) {
    if (machine.empty()) return Result<Json>::failure({0, "invalid_request", "machine is required", false});
    return request_json("GET", "/api/v1/sessions?machine=" + encode_query(machine));
}

Result<Json> BridgePanelClient::create_session(std::string_view machine, std::string_view name,
                                               std::string_view command, int cols, int rows) {
    if (machine.empty() || name.empty() || command.empty() || cols < 1 || cols > 500 || rows < 1 || rows > 300)
        return Result<Json>::failure({0, "invalid_request", "machine, session, command, and terminal size are required", false});
    Json body{{"machine", machine}, {"name", name}, {"command", command}, {"cols", cols}, {"rows", rows}};
    return request_json("POST", "/api/v1/sessions", &body);
}

Result<Json> BridgePanelClient::send_input(std::string_view machine, std::string_view session,
                                           std::string_view bytes) {
    if (machine.empty() || session.empty()) return Result<Json>::failure({0, "invalid_request", "machine and session are required", false});
    if (bytes.size() > 65536 || bytes.size() > options_.http.limits.max_request_bytes)
        return Result<Json>::failure({0, "request_too_large", "session input exceeds limit", false});
    const int encoded_size = 4 * static_cast<int>((bytes.size() + 2) / 3);
    std::string encoded(static_cast<std::size_t>(encoded_size), '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                                         reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()));
    encoded.resize(written < 0 ? 0 : static_cast<std::size_t>(written));
    Json body{{"machine", machine}, {"session", session}, {"data_b64", encoded}};
    return request_json("POST", "/api/v1/sessions/input", &body);
}

Result<OutputChunk> BridgePanelClient::read_output(std::string_view machine, std::string_view session,
                                                   std::size_t offset, std::size_t limit) {
    if (machine.empty() || session.empty() || limit == 0 || limit > 65536)
        return Result<OutputChunk>::failure({0, "invalid_request", "machine, session, and bounded limit are required", false});
    const std::string path = "/api/v1/sessions/output?machine=" + encode_query(machine) +
                             "&session=" + encode_query(session) + "&offset=" + std::to_string(offset) +
                             "&limit=" + std::to_string(limit);
    auto response = request_json("GET", path, nullptr, 2 * 1024 * 1024);
    if (!response.ok()) return Result<OutputChunk>::failure(response.error());
    if (!response.value().contains("offset") || !response.value().contains("text_b64") ||
        !response.value().contains("reset"))
        return Result<OutputChunk>::failure({200, "invalid_api_response", "output response lacks offset, text_b64, or reset", false});
    try {
        const auto& cursor = response.value().at("offset");
        if ((!cursor.is_number_unsigned() && !cursor.is_number_integer()) ||
            (cursor.is_number_integer() && !cursor.is_number_unsigned() && cursor.get<std::int64_t>() < 0) ||
            !response.value().at("text_b64").is_string() || !response.value().at("reset").is_boolean())
            return Result<OutputChunk>::failure({200, "invalid_api_response", "invalid output field types", false});
        const auto raw_offset = cursor.get<std::uint64_t>();
        if (raw_offset > std::numeric_limits<std::size_t>::max())
            return Result<OutputChunk>::failure({200, "invalid_api_response", "output offset exceeds platform bounds", false});
        const auto server_offset = static_cast<std::size_t>(raw_offset);
        const auto encoded = response.value().at("text_b64").get<std::string>();
        auto decoded = decode_base64(encoded);
        if (!decoded.ok()) return Result<OutputChunk>::failure(decoded.error());
        if (decoded.value().size() > limit) return Result<OutputChunk>::failure({200, "response_too_large", "output chunk exceeds requested limit", false});
        const bool reset = response.value().at("reset").get<bool>();
        if (server_offset < decoded.value().size() ||
            (!reset && (offset > std::numeric_limits<std::size_t>::max() - decoded.value().size() ||
                        server_offset != offset + decoded.value().size())))
            return Result<OutputChunk>::failure({200, "invalid_api_response", "output offset is not the next byte cursor", false});
        return Result<OutputChunk>::success({server_offset, std::move(decoded.value()), reset});
    } catch (const std::exception& exception) {
        return Result<OutputChunk>::failure({200, "invalid_api_response", HttpTransport::redact(exception.what(), options_.bearer_token), false});
    }
}

Result<Json> BridgePanelClient::kill_session(std::string_view machine, std::string_view session) {
    return request_json("DELETE", "/api/v1/sessions?machine=" + encode_query(machine) + "&session=" + encode_query(session));
}

Result<Json> BridgePanelClient::list_files(std::string_view machine, std::string_view root, std::string_view path) {
    return request_json("GET", "/api/v1/files?machine=" + encode_query(machine) + "&root=" + encode_query(root) + "&path=" + encode_query(path));
}

Result<Json> BridgePanelClient::read_file(std::string_view machine, std::string_view root, std::string_view path) {
    return request_json("GET", "/api/v1/files/content?machine=" + encode_query(machine) + "&root=" + encode_query(root) + "&path=" + encode_query(path), nullptr, 16 * 1024 * 1024);
}

Result<Json> BridgePanelClient::write_file(std::string_view machine, std::string_view root, std::string_view path, std::string_view content) {
    if (content.size() > options_.http.limits.max_request_bytes)
        return Result<Json>::failure({0, "request_too_large", "file content exceeds limit", false});
    std::string encoded((content.size() + 2) / 3 * 4, '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()), reinterpret_cast<const unsigned char*>(content.data()), static_cast<int>(content.size()));
    encoded.resize(written < 0 ? 0 : static_cast<std::size_t>(written));
    Json body{{"machine", machine}, {"root", root}, {"path", path}, {"content_b64", encoded}};
    return request_json("POST", "/api/v1/files/content", &body, 16 * 1024 * 1024);
}

Result<Json> BridgePanelClient::agents(std::string_view machine) {
    std::string path = "/api/v1/agents";
    if (!machine.empty()) path += "?machine=" + encode_query(machine);
    return request_json("GET", std::move(path));
}

Result<Json> BridgePanelClient::start_chat(std::string_view machine, std::string_view agent,
                                           std::string_view prompt, std::string_view request_id) {
    Json body{{"machine", machine}, {"agent", agent}, {"prompt", prompt}, {"request_id", request_id}};
    return request_json("POST", "/api/v1/chat", &body);
}

Result<Json> BridgePanelClient::poll_chat(std::string_view request_id) {
    return request_json("GET", "/api/v1/chat/" + encode_query(request_id));
}

Result<Json> BridgePanelClient::cancel_chat(std::string_view request_id) {
    return request_json("DELETE", "/api/v1/chat/" + encode_query(request_id));
}

} // namespace bridge_native
