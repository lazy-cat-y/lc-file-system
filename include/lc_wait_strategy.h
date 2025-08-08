#ifndef LC_WAIT_STRATEGY_H
#define LC_WAIT_STRATEGY_H

#include <chrono>
#include <condition_variable>
#include <mutex>

#include "lc_configs.h"

LC_NAMESPACE_BEGIN
LC_FILESYSTEM_NAMESPACE_BEGIN

class LCWaitStrategyBase {
public:
    virtual ~LCWaitStrategyBase() = default;
    virtual void wait()           = 0;  // Wait for a condition to be met
    virtual void wait_for(
        std::chrono::milliseconds
            duration)         = 0;      // Wait for a condition with timeout
    virtual void notify()     = 0;      // Notify waiting threads
    virtual void notify_all() = 0;      // Notify all waiting threads
    virtual void reset()      = 0;  // Reset the wait strategy, if applicable
};

class LCConditionVariableWaitStrategy : public LCWaitStrategyBase {
public:
    LCConditionVariableWaitStrategy() : notified_(false) {}

    void wait() override {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this] { return notified_; });
    }

    void wait_for(std::chrono::milliseconds duration) override {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait_for(lock, duration, [this] { return notified_; });
    }

    void notify() override {
        std::scoped_lock<std::mutex> lock(mtx_);
        notified_ = true;
        cv_.notify_one();
    }

    void notify_all() override {
        std::scoped_lock<std::mutex> lock(mtx_);
        notified_ = true;
        cv_.notify_all();
    }

    void reset() override {
        std::scoped_lock<std::mutex> lock(mtx_);
        notified_ = false;
    }

private:
    std::condition_variable cv_;
    std::mutex              mtx_;
    bool                    notified_;
};

LC_FILESYSTEM_NAMESPACE_END
LC_NAMESPACE_END

#endif  // LC_WAIT_STRATEGY_H
