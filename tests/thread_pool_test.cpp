

#include <gtest/gtest.h>

#include <unordered_set>

#include "lc_mpmc_queue.h"
#include "lc_thread_pool.h"

using namespace lc::fs;

class LCThreadPoolTest : public ::testing::Test {
protected:
    static constexpr size_t kNumTasks    = 100;
    static constexpr size_t kThreadCount = 20;

    struct TestSharedState {
        std::mutex              mutex;
        std::unordered_set<int> completed;
        std::atomic<int>        counter {0};
    };

    std::shared_ptr<TestSharedState> state =
        std::make_shared<TestSharedState>();
};

TEST_F(LCThreadPoolTest, SubmitAndExecuteAllTasks) {
    ThreadPool<LCTaskPriority> pool("test_pool", kThreadCount);

    for (int i = 0; i < kNumTasks; ++i) {
        auto shared_state = state;
        auto task = std::make_shared<LambdaTask<std::function<void()>>>(
            [i, shared_state]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::lock_guard<std::mutex> lock(shared_state->mutex);
            shared_state->completed.insert(i);
            shared_state->counter.fetch_add(1, std::memory_order_relaxed);
        });

        ThreadPoolContextMetaData<LCTaskPriority> metadata {
            .listener_id = "test",
            .trace_id    = std::to_string(i),
            .timestamp   = std::time(nullptr),
            .priority    = LCTaskPriority::Normal};

        TreadPoolContextFactory<LCTaskPriority> factory(metadata, task);

        ASSERT_TRUE(pool.wait_and_submit_task(factory));
    }

    std::this_thread::sleep_for(std::chrono::seconds(5));

    EXPECT_EQ(state->counter.load(), kNumTasks);
    EXPECT_EQ(state->completed.size(), kNumTasks);

    pool.shutdown();
}

TEST_F(LCThreadPoolTest, ShutdownPreventsFurtherSubmission) {
    ThreadPool<LCTaskPriority> pool("test_shutdown", kThreadCount);

    pool.shutdown();

    auto task = std::make_shared<LambdaTask<std::function<void()>>>([]() {});

    ThreadPoolContextMetaData<LCTaskPriority> metadata {
        .listener_id = "test",
        .trace_id    = "shutdown_test",
        .timestamp   = std::time(nullptr),
        .priority    = LCTaskPriority::Normal};

    TreadPoolContextFactory<LCTaskPriority> factory(metadata, task);

    ASSERT_FALSE(pool.wait_and_submit_task(factory));
}
