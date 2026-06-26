#pragma once

#include <stdexcept>
#include <string>

/**
 * lpkg 基础异常类
 * 所有 lpkg 运行时异常的基类，继承自 std::runtime_error
 */
class LpkgException : public std::runtime_error {
public:
    explicit LpkgException(const std::string& message)
        : std::runtime_error(message) {}
};
