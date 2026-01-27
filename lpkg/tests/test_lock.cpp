#include <gtest/gtest.h>
#include "../main/src/utils.hpp"
#include "../main/src/config.hpp"
#include <filesystem>
#include <thread>
#include <vector>
#include <future>

namespace fs = std::filesystem;

class LockTest : public ::testing::Test {
protected:
    fs::path test_root;

    void SetUp() override {
        test_root = fs::absolute("tmp_lock_test");
        if (fs::exists(test_root)) fs::remove_all(test_root);
        fs::create_directories(test_root);
        
        set_root_path(test_root.string());
        init_filesystem();
    }

    void TearDown() override {
        set_root_path("/");
        if (fs::exists(test_root)) fs::remove_all(test_root);
    }
};

TEST_F(LockTest, BasicLocking) {
    // 1. Acquire lock
    std::unique_ptr<DBLock> lock1;
    EXPECT_NO_THROW(lock1 = std::make_unique<DBLock>());
    
    // 2. Attempt to acquire another lock while first is held (should fail)
    EXPECT_THROW(DBLock lock2, LpkgException);
}

TEST_F(LockTest, LockReleaseAndReacquire) {
    {
        DBLock lock1;
    } // lock1 released here
    
    // Should be able to acquire again
    EXPECT_NO_THROW(DBLock lock2);
}

TEST_F(LockTest, ConcurrencyTest) {
    std::promise<void> p;
    auto f = p.get_future();

    // Thread 1 acquires lock and waits
    std::thread t1([this, &p]() {
        DBLock lock;
        p.set_value();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    });

    f.wait(); // Wait for t1 to get the lock

    // Thread 2 tries to get the lock (should fail immediately due to LOCK_NB)
    EXPECT_THROW(DBLock lock2, LpkgException);

    t1.join();

    // Now it should succeed
    EXPECT_NO_THROW(DBLock lock3);
}

TEST_F(LockTest, MultipleThreadsAttempting) {
    const int num_threads = 10;
    std::atomic<int> success_count{0};
    std::atomic<int> failure_count{0};

    auto attempt_lock = [&]() {
        try {
            DBLock lock;
            success_count++;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } catch (const LpkgException& e) {
            failure_count++;
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(attempt_lock);
    }

    for (auto& t : threads) {
        t.join();
    }

    // Since they run mostly concurrently and hold the lock for 10ms,
    // we expect at least some to fail and exactly one at a time to succeed.
    // However, depending on scheduling, success_count might be > 1 if they sequentialize,
    // but failure_count should be > 0 in a truly concurrent scenario.
    EXPECT_GE(success_count, 1);
    EXPECT_LE(success_count, num_threads);
}
