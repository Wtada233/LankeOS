#pragma once

#include <stdexcept>
#include <string>

/// 所有 lpkg 运行时异常的基类。
class LpkgException : public std::runtime_error
{
public:
    explicit LpkgException(const std::string& message) : std::runtime_error(message)
    {
    }
};

/**
 * 归档因**安全原因**被整包拒绝（成员名消毒：控制字符 / 字面 " → " / lpkg 自用后缀）。
 *
 * 单独成类是为了让调用方能把它与"普通解压失败"区分开：构建期"自动解压源码归档"
 * （`download_and_prepare_sources`）对普通失败是**容忍**的（扩展名骗人，如 `.tar.gz`
 * 其实不是归档 —— 告警后继续），但**安全拒绝绝不能吞**：那意味着归档里有危险成员，
 * 而且拒绝发生在循环中途、源码树只解出了一半，继续构建只会以离奇方式失败。
 */
class UnsafeArchiveException : public LpkgException
{
public:
    explicit UnsafeArchiveException(const std::string& message) : LpkgException(message)
    {
    }
};

/**
 * **用户取消**：交互提示上答"不"、验证码输错，或按了 Ctrl+C（SIGINT 优雅中止）后由各事务
 * 路径抛出。三条路径（install / upgrade / remove…）都用它，**与具体操作无关**。
 *
 * 与普通 `LpkgException` 的区别只在**呈现与退出语义**（`main_cli.cpp` 单独接它）：
 *   · 用 `log_info` 打消息 —— **不套 `Error:` 前缀**，用户取消不是错误；
 *   · **退出码非零** —— 脚本/farm 必须能区分"取消（什么都没做）"与"做完了"。
 * 普通异常仍是 `Error: …` + 退出码 1。
 *
 * 它派生自 `LpkgException`，所以既有的 `catch (const LpkgException&)` 与
 * `EXPECT_THROW(…, LpkgException)` 照旧生效（取消仍然是"这次命令没做成"）。
 */
class UserAbort : public LpkgException
{
public:
    explicit UserAbort(const std::string& message) : LpkgException(message)
    {
    }
};
