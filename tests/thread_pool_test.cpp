

#include <gtest/gtest.h>

#include <unordered_set>

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
    LCThreadPool<LCWriteTaskPriority> pool("test_pool", kThreadCount);

    for (int i = 0; i < kNumTasks; ++i) {
        auto shared_state = state;
        auto task = std::make_shared<LCLambdaTask<std::function<void()>>>(
            [i, shared_state]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::lock_guard<std::mutex> lock(shared_state->mutex);
            shared_state->completed.insert(i);
            shared_state->counter.fetch_add(1, std::memory_order_relaxed);
        });

        LCThreadPoolContextMetaData<LCWriteTaskPriority> metadata {
            .listener_id = 0,
            .trace_id    = std::to_string(i),
            .timestamp   = std::time(nullptr),
            .priority    = LCWriteTaskPriority::Normal};

        LCTreadPoolContextFactory<LCWriteTaskPriority> factory(
            metadata,
            task,
            std::make_shared<std::atomic<bool>>(false));

        ASSERT_TRUE(pool.wait_and_submit_task(factory));
    }

    std::this_thread::sleep_for(std::chrono::seconds(5));

    EXPECT_EQ(state->counter.load(), kNumTasks);
    EXPECT_EQ(state->completed.size(), kNumTasks);

    pool.shutdown();
}

TEST_F(LCThreadPoolTest, CancelledTaskIsNotExecuted) {
    LCThreadPool<LCWriteTaskPriority> pool("test_cancel", kThreadCount);

    auto cancel_token = std::make_shared<std::atomic<bool>>(true);
    auto task = std::make_shared<LCLambdaTask<std::function<void()>>>([this]() {
        state->counter.fetch_add(100,
                                 std::memory_order_relaxed);  // should not run
    });

    LCThreadPoolContextMetaData<LCWriteTaskPriority> metadata {
        .listener_id = "test",
        .trace_id    = "cancel_test",
        .timestamp   = std::time(nullptr),
        .priority    = LCWriteTaskPriority::Normal};

    LCTreadPoolContextFactory<LCWriteTaskPriority> factory(metadata,
                                                           task,
                                                           cancel_token);
    ASSERT_TRUE(pool.wait_and_submit_task(factory));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(state->counter.load(), 0);

    pool.shutdown();
}

TEST_F(LCThreadPoolTest, ShutdownPreventsFurtherSubmission) {
    LCThreadPool<LCWriteTaskPriority> pool("test_shutdown", kThreadCount);

    pool.shutdown();

    auto task = std::make_shared<LCLambdaTask<std::function<void()>>>([]() {});

    LCThreadPoolContextMetaData<LCWriteTaskPriority> metadata {
        .listener_id = "test",
        .trace_id    = "shutdown_test",
        .timestamp   = std::time(nullptr),
        .priority    = LCWriteTaskPriority::Normal};

    LCTreadPoolContextFactory<LCWriteTaskPriority> factory(
        metadata,
        task,
        std::make_shared<std::atomic<bool>>(false));

    ASSERT_FALSE(pool.wait_and_submit_task(factory));
}
