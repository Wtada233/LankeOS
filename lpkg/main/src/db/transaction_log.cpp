#include "transaction_log.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <filesystem>

#include "base/exception.hpp"
#include "base/utils.hpp"
#include "config/config.hpp"
#include "i18n/localization.hpp"
#include "wal_op.hpp"

namespace fs = std::filesystem;

namespace wal
{

// ============================================================================
// WalWriter
// ============================================================================

WalWriter::WalWriter()
{
    std::string path = wal_log_path();

    // 确保父目录存在
    fs::path p(path);
    if (auto parent = p.parent_path(); !parent.empty()) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }

    fd_ = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, constants::PERM_WAL_LOG);
    if (fd_ < 0) throw LpkgException(string_format("error.wal_open_failed", path));
    // 新建/首次打开后 fsync 父目录：否则断电可能只丢了目录项——已经 fsync 过的 WAL 行
    // （描述了文件系统改动）变得不可达，回滚依据随之消失（I-FSYNC-3，TODO.md X7）。
    //
    // **必须恒生效**（所以套守卫，不受默认关闭的 durable_fsync_enabled() 影响）：
    // POSIX 下 fsync(文件) 不覆盖父目录的 dentry，于是"WAL 行已 fsync、文件整个不存在"
    // 是断电后真实可能的状态 —— 而行的持久化是"行 = 一个已开始但可能未完成的操作"这条
    // 不变量的前提，丢了行就没有回滚依据，物理操作却已经做完（不可恢复组合）。
    //
    // 守卫**只在此块内**：WalWriter 的生存期横跨整批，绝不能把它提成成员/外层局部，
    // 否则会顺带把批量文件数据的 fsync 一起打开（那类数据是故意留给开关控制的）。
    {
        DurableFsyncGuard durable;  // WAL 文件目录项：创建即落盘，与开关无关
        fsync_parent_dir(path);
    }
}

WalWriter::~WalWriter()
{
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

void WalWriter::log(std::string_view line)
{
    if (fd_ < 0) throw LpkgException(string_format("error.wal_open_failed", wal_log_path()));

    std::string l = std::string(line) + "\n";
    ssize_t written = ::write(fd_, l.data(), l.size());
    if (written < 0 || static_cast<size_t>(written) != l.size())
        throw LpkgException(string_format("error.wal_write_failed", wal_log_path()));

    // WAL 行**自己的 fsync 恒生效**（I-FSYNC-1，不受 durable_fsync_enabled() 影响）：
    // 单行是顺序 write，进程死亡不会写坏它，所以 kill/回滚语义与 fsync 无关；但**断电**
    // 会丢掉没 fsync 的行，而"行 = 一个已开始、可能未完成的操作"是回滚的唯一依据 ——
    // 行丢了、物理操作却已经做完，就是不可恢复组合。默认模式省下的是**批量文件数据**的
    // fsync（包内容/.lpkgtmp 的内容与其 rename 父目录），不是 WAL 行：生产路径
    // `wal::log_wal_line` 本来就是无条件 fsync（ARCH §1.6/§2.1）。本类此前"跟开关走"
    // 与头文件/ARCH 的契约相反，且会静默弱化将来经 begin_batch()+log() 写出的行 ——
    // 要去掉 fsync 请**显式**用 log_no_fsync()，别让它取决于一个全局开关。
    if (::fsync(fd_) != 0)
        throw LpkgException(string_format("error.wal_fsync_failed", wal_log_path()));

    ++lines_;
}

void WalWriter::log_no_fsync(std::string_view line)
{
    if (fd_ < 0) return;

    std::string l = std::string(line) + "\n";
    ::write(fd_, l.data(), l.size());
    ++lines_;
}

void WalWriter::fsync_wal()
{
    if (fd_ >= 0) ::fsync(fd_);
}

// ============================================================================
// 便捷函数
// ============================================================================

WalWriter begin_batch()
{
    WalWriter w;
    // 不写包数 N：批次开启时无法预知最终包数（metadata 重解析会增长批次），
    // 且恢复逻辑从不读取 N（只按 BEGIN_PKGS/COMMIT_PKGS 类型做 depth 跟踪）。
    // 一个不可信的数字不应留在协议里。
    w.log("BEGIN_PKGS");
    return w;
}

void log_wal_line(std::string_view line)
{
    std::string path = wal_log_path();
    bool created = false;
    int fd = open_wal_append(created);
    if (fd < 0) throw LpkgException(string_format("error.wal_open_failed", path));

    std::string l = std::string(line) + "\n";
    ssize_t written = ::write(fd, l.data(), l.size());
    if (written < 0 || static_cast<size_t>(written) != l.size()) {
        ::close(fd);
        throw LpkgException(string_format("error.wal_write_failed", path));
    }
    // **首次创建时**把目录项也落盘（见 WalWriter 注释）。同样**必须恒生效**：这条打开路径
    // 与 WalWriter 构造是同一个"WAL 首次创建"路径，fsync(文件) 管不到父目录项，而 WAL
    // 行的持久性是该系统"行 = 一个已开始但可能未完成的操作"这条不变量的前提 ——
    // 行在、文件却因 dentry 未落盘而整个消失，就是那个不可恢复组合。
    //
    // **只在真的创建了文件时才做**：目录项只需要在创建那一刻落盘，而本函数**每写一行**
    // 都要走一次 —— 恒做等于每条 WAL 行白付一次父目录 fsync（实测占全部 fsync 的
    // 34%~40%）。`open_wal_append` 用 O_CREAT|O_EXCL 原子地判出"本次是否创建"（不是
    // TOCTOU 的 fs::exists）。行内容自己的 ::fsync(fd) 不受影响，恒生效（I-FSYNC-1 不变）。
    if (created) {
        DurableFsyncGuard durable;  // WAL 文件目录项：创建即落盘，与开关无关
        fsync_parent_dir(path);
    }

    if (::fsync(fd) != 0) {
        ::close(fd);
        throw LpkgException(string_format("error.wal_fsync_failed", path));
    }

    ::close(fd);
}

void commit_batch()
{
    log_wal_line("COMMIT_PKGS");
}

}  // namespace wal
