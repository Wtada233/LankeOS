#pragma once

// ============ 并发控制 ============

/// RAII：构造时加锁、析构时自动解锁，防止并发操作数据库。
class DBLock
{
public:
    DBLock();
    ~DBLock();
    DBLock(const DBLock&) = delete;
    DBLock& operator=(const DBLock&) = delete;

private:
    int lock_fd = -1;
};
