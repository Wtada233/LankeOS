#pragma once

#include <stdexcept>
#include <string>

class LpkgException : public std::runtime_error {
public:
    explicit LpkgException(const std::string& message)
        : std::runtime_error(message) {}
};
