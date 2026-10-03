#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "../../main/src/base/utils.hpp"
#include "../../main/src/config/config.hpp"

namespace fs = std::filesystem;

class LockTest : public ::testing::Test
{
protected:
    fs::path test_root;

    void SetUp() override
    {
        // 沙箱目录名带 PID：两个测试进程并发时会各自 rm -rf 对方的固定名目录
        // （实测 58 条 SetUp 假失败：cannot remove all: Directory not empty）。同进程内
        // 多次 SetUp/TearDown 仍复用同一路径（getpid 不变），TearDown 照旧清理干净。
        test_root = fs::absolute("tmp_lock_test_" + std::to_string(getpid()));
        if (fs::exists(test_root)) fs::remove_all(test_root);
        fs::create_directories(test_root);

        Config::instance().set_root_path(test_root.string());
        Config::instance().init_filesystem();
    }

    void TearDown() override
    {
        Config::instance().set_root_path("/");
        if (fs::exists(test_root)) fs::remove_all(test_root);
    }
};

TEST_F(LockTest, BasicLocking)
{
    // 1. Acquire lock
    std::unique_ptr<DBLock> lock1;
    EXPECT_NO_THROW(lock1 = std::make_unique<DBLock>());

    // 2. Attempt to acquire another lock while first is held (should fail)
    EXPECT_THROW(DBLock lock2, LpkgException);
}

TEST_F(LockTest, LockReleaseAndReacquire)
{
    {
        DBLock lock1;
    }  // lock1 released here

    // Should be able to acquire again
    EXPECT_NO_THROW(DBLock lock2);
}

TEST_F(LockTest, ConcurrencyTest)
{
    std::promise<void> p;
    auto f = p.get_future();

    // Thread 1 acquires lock and waits
    std::thread t1([&p]() {
        DBLock lock;
        p.set_value();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    });

    f.wait();  // Wait for t1 to get the lock

    // Thread 2 tries to get the lock (should fail immediately due to LOCK_NB)
    EXPECT_THROW(DBLock lock2, LpkgException);

    t1.join();

    // Now it should succeed
    EXPECT_NO_THROW(DBLock lock3);
}

TEST_F(LockTest, MultipleThreadsAttempting)
{
    // 被测语义（读 DBLock 实现）：构造 = open() + flock(LOCK_EX | LOCK_NB)，拿不到就抛
    // LpkgException。flock 是"每 open file description"的排他锁 —— 每个线程各自 open，
    // 所以**同一进程内**也互斥，且**任意时刻至多一个线程**持有它；析构释放后别的线程才可能拿到。
    const int num_threads = 10;
    std::promise<void> go;
    auto go_signal = go.get_future().share();  // 让 10 个线程"同时"开抢，而不是各起各的

    std::atomic<int> success_count{0};
    std::atomic<int> failure_count{0};
    std::atomic<int> holders{0};      // 此刻正在持锁的线程数
    std::atomic<int> max_holders{0};  // 观测到的并发持有者峰值

    auto attempt_lock = [&]() {
        go_signal.wait();
        try {
            DBLock lock;
            // 记"并发持有者"：互斥成立 ⟺ 这个数在任何观测点都不超过 1。
            const int now = holders.fetch_add(1) + 1;
            int seen = max_holders.load();
            while (now > seen && !max_holders.compare_exchange_weak(seen, now)) {
                // CAS 失败时 seen 已被更新成当前值，循环重试即可（只用原子操作记录峰值，
                // 即便互斥真的坏了也不会漏记并发持有者）。
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            holders.fetch_sub(1);
            success_count.fetch_add(1);
        } catch (const LpkgException&) {
            failure_count.fetch_add(1);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(attempt_lock);
    }
    go.set_value();  // 放行：此刻 10 个线程一起调 flock

    for (auto& t : threads) {
        t.join();
    }

    // (1) 每个线程都得出过结论（成功或失败），没有线程半路消失、统计漏掉它。
    EXPECT_EQ(success_count + failure_count, num_threads) << "有线程既没算成功也没算失败";
    // (2) 至少一个线程拿到锁（否则这把锁等于永远锁不上，测试根本没走到互斥）。
    EXPECT_GE(success_count, 1);
    // (3) **互斥本身**：任意观测时刻至多一个线程持锁。这是本用例能证明的核心 ——
    //     若 flock 失效（锁退化成空操作），同时开抢、各持 10ms 的 10 个线程会被观测到并发
    //     持有（max_holders > 1），这里就会红。
    EXPECT_EQ(max_holders, 1) << "同一时刻有多个线程共同持锁 —— 互斥失效";
    // 不写 success_count == 1：线程可以**依次**拿到锁（前一个释放、后一个才成功），
    // 于是成功数 > 1 是合法结果（"同时只有一个活着持有者"与"先后有多个成功者"并不矛盾），
    // 钉它会假红。也不写 failure_count >= 1：完全串行化的调度下 10 个可以全成功，同样合法。
    // （"刚好一个持有者"的**确定性**证明在 ConcurrencyTest：t1 持锁期间主线程取锁必然抛。）
}
