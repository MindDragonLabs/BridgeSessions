#pragma once

#include <string>
#include <utility>
#include <variant>

namespace bridge_native {

struct Error {
    int status = 0;
    std::string code;
    std::string message;
    bool retryable = false;
};

template <typename T>
class Result {
public:
    static Result success(T value) { return Result(std::move(value)); }
    static Result failure(Error error) { return Result(std::move(error)); }

    bool ok() const { return std::holds_alternative<T>(value_); }
    const T& value() const { return std::get<T>(value_); }
    T& value() { return std::get<T>(value_); }
    const Error& error() const { return std::get<Error>(value_); }

private:
    explicit Result(T value) : value_(std::move(value)) {}
    explicit Result(Error error) : value_(std::move(error)) {}
    std::variant<T, Error> value_;
};

} // namespace bridge_native

