#ifndef LC_THREAD_POOL_H
#define LC_THREAD_POOL_H

#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "lc_configs.h"
#include "lc_context.h"
#include "lc_mpmc_queue.h"
#include "lc_task.h"

LC_NAMESPACE_BEGIN
LC_FILESYSTEM_NAMESPACE_BEGIN

template <typename PriorityType>
struct LCTreadPoolContextMetaData {
    uint32_t listener_id;
    // TODO: trace id generation
    std::string  trace_id;
    time_t       timestamp;
    PriorityType priority;
};

template <typename PriorityType>
struct LCContext<LCTreadPoolContextMetaData<PriorityType>> {
    static_assert(
        std::is_same<PriorityType, LCWriteTaskPriority>::value ||
            std::is_same<PriorityType, LCReadTaskPriority>::value,
        "Invalid priority type, must be either LCWriteTaskPriority or LCReadTaskPriority");
    LCTreadPoolContextMetaData<PriorityType> metadata;
    // std::bind(f, args...) or a lambda function
    // std::make_shared<LambdaTask<std::function<void()>>>(std::bind(&Foo::bar,
    // &foo));
    // std::make_shared<LambdaTask<decltype(real_lambda)>>(std::move(real_lambda));
    std::shared_ptr<LCTask>            task;
    std::shared_ptr<std::atomic<bool>> cancel_token;

    LCContext() = default;

    LCContext(const LCTreadPoolContextMetaData<PriorityType> &meta,
              std::shared_ptr<LCTask>                         data) :
        metadata(meta),
        task(std::move(data)) {
        cancel_token = std::make_shared<std::atomic<bool>>(false);
    }

    LCContext(const LCTreadPoolContextMetaData<PriorityType> &meta,
              std::shared_ptr<LCTask>                         data,
              std::shared_ptr<std::atomic<bool>>              cancel_token) :
        metadata(meta),
        task(std::move(data)),
        cancel_token(std::move(cancel_token)) {}

    LCContext(const LCContext &)            = delete;
    LCContext &operator=(const LCContext &) = delete;

    LCContext(LCContext &&other) {
        metadata     = std::move(other.metadata);
        task         = std::move(other.task);
        cancel_token = std::move(other.cancel_token);
    }

    LCContext &operator=(LCContext &&other) {
        if (this != &other) {
            metadata     = std::move(other.metadata);
            task         = std::move(other.task);
            cancel_token = std::move(other.cancel_token);
        }
        return *this;
    }

    bool is_cancelled() const {
        LC_ASSERT(cancel_token != nullptr, "cancel_token is nullptr!");
        return cancel_token && cancel_token->load();
    }

    void cancel() {
        if (cancel_token) {
            cancel_token->store(true);
        }
    }

    void operator()() const {
        if (!is_cancelled() && task) {
            task->run();
        }
    }
};

template <typename PriorityType>
class LCTreadPoolContextFactory {
    using MetadataType = LCTreadPoolContextMetaData<PriorityType>;
    using ContextType  = LCContext<MetadataType>;
public:

    LCTreadPoolContextFactory() = delete;

    LCTreadPoolContextFactory(const LCTreadPoolContextFactory &) = delete;
    LCTreadPoolContextFactory &operator=(const LCTreadPoolContextFactory &) =
        delete;
    LCTreadPoolContextFactory(LCTreadPoolContextFactory &&)            = delete;
    LCTreadPoolContextFactory &operator=(LCTreadPoolContextFactory &&) = delete;

    LCTreadPoolContextFactory(const MetadataType                &metadata,
                              std::shared_ptr<LCTask>            task,
                              std::shared_ptr<std::atomic<bool>> cancel_token) :
        metadata_(metadata),
        task_(std::move(task)),
        cancel_token_(std::move(cancel_token)) {}

    ContextType make() {
        return ContextType(metadata_, task_, cancel_token_);
    }

private:
    MetadataType                       metadata_;
    std::shared_ptr<LCTask>            task_;
    std::shared_ptr<std::atomic<bool>> cancel_token_;
};

template <typename PriorityWeightType>
struct LCPriorityWeights;

template <>
struct LCPriorityWeights<LCWriteTaskPriority> {
    static constexpr std::array<
        uint32_t, static_cast<size_t>(LCWriteTaskPriority::NUM_PRIORITIES)>
    get_weights() {
        return {10, 8, 5, 3, 1};
    }
};

template <>
struct LCPriorityWeights<LCReadTaskPriority> {
    static constexpr std::array<
        uint32_t, static_cast<size_t>(LCReadTaskPriority::NUM_PRIORITIES)>
    get_weights() {
        return {10, 8, 5, 3, 1};
    }
};

template <typename T, typename PriorityType>
class LCWeightedRoundRobinScheduler {
    static_assert(
        std::is_same<PriorityType, LCWriteTaskPriority>::value ||
            std::is_same<PriorityType, LCReadTaskPriority>::value,
        "Invalid priority type, must be either LCWriteTaskPriority or LCReadTaskPriority");
public:

    LCWeightedRoundRobinScheduler() {
        reset_weights();
    }

    ~LCWeightedRoundRobinScheduler() = default;

    LCWeightedRoundRobinScheduler(const LCWeightedRoundRobinScheduler &) =
        delete;
    LCWeightedRoundRobinScheduler &operator=(
        const LCWeightedRoundRobinScheduler &)                      = delete;
    LCWeightedRoundRobinScheduler(LCWeightedRoundRobinScheduler &&) = delete;
    LCWeightedRoundRobinScheduler &operator=(LCWeightedRoundRobinScheduler &&) =
        delete;

    bool try_schedule(LCMPMCMultiPriorityQueue<T, PriorityType> &queue,
                      T                                         &task) {
        const size_t num_priorities =
            static_cast<size_t>(PriorityType::NUM_PRIORITIES);
        for (size_t attempt = 0; attempt < num_priorities; ++attempt) {
            size_t index = current_index_ % num_priorities;
            if (weights_[index] > 0) {
                if (queue.dequeue(task, static_cast<PriorityType>(index))) {
                    weights_[index]--;
                    return true;
                }
            }
            current_index_ = (current_index_ + 1) % num_priorities;
        }
        if (std::all_of(weights_.begin(), weights_.end(), [](uint32_t w) {
            return w == 0;
        })) {
            reset_weights();
        }
        return false;
    }

private:

    void reset_weights() {
        weights_ = LCPriorityWeights<PriorityType>::get_weights();
    }

    template <typename U>
    static constexpr bool always_false_v = false;

    size_t current_index_ = 0;
    std::array<uint32_t, static_cast<size_t>(PriorityType::NUM_PRIORITIES)>
        weights_;
};

inline void safe_launch_function(std::function<void()> func) {
    try {
        func();
    } catch (const std::exception &e) {
        // log
    } catch (...) {}
}

template <class PriorityType, size_t ThreadCount>
class LCThreadPool {
    using ContextType = LCContext<LCTreadPoolContextMetaData<PriorityType>>;
    static_assert(
        std::is_same<PriorityType, LCWriteTaskPriority>::value ||
            std::is_same<PriorityType, LCReadTaskPriority>::value,
        "Invalid priority type, must be either LCWriteTaskPriority or LCReadTaskPriority");
public:

    LCThreadPool() = delete;

    LCThreadPool(const LCThreadPool &)            = delete;
    LCThreadPool &operator=(const LCThreadPool &) = delete;
    LCThreadPool(LCThreadPool &&)                 = delete;
    LCThreadPool &operator=(LCThreadPool &&)      = delete;

    LC_EXPLICIT LCThreadPool(const std::string &name) : name_(std::move(name)) {
        for (size_t i = 0; i < ThreadCount; ++i) {
            cancel_tokens_[i] = std::make_shared<std::atomic<bool>>(false);
        }
        launch_worker_threads();
        stop_.store(false);
        draining_.store(false);
        for (size_t i = 0; i < ThreadCount; ++i) {
            last_heartbeat_[i].store(std::chrono::steady_clock::now(),
                                     std::memory_order_relaxed);
        }
    }

    ~LCThreadPool() {
        shutdown();
    }

    bool submit_task(ContextType &&context) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_.load()) {
                return false;  // Cannot submit tasks when stopped
            }
            if (!task_queue_.enqueue(std::move(context),
                                     context.metadata.priority)) {
                return false;  // Queue is full
            }
        }
        cv_.notify_one();  // Notify one thread to wake up and process the task
        return true;
    }

    bool wait_and_submit_task(
        LCTreadPoolContextFactory<PriorityType> &factory) {
        while (true) {
            if (is_stopped()) {
                return false;  // Cannot submit tasks when stopped
            }
            ContextType ctx = factory.make();
            if (submit_task(std::move(ctx))) {
                return true;  // Task submitted successfully
            }
        }
        LC_ASSERT(false, "Should not reach here");
        return false;  // Should not reach here
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (is_stopped() || is_draining()) {
                return;
            }
            stop_.store(true);
            draining_.store(true);
        }

        watchdog_cv_.notify_all();  // Notify all threads to wake up and stop
        cv_.notify_all();           // Notify all threads to wake up and stop

        while (!task_queue_.is_empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        cv_.notify_all();  // Notify all threads to wake up and stop

        join_threads();    // Wait for all threads to finish

        {
            std::lock_guard<std::mutex> lock(mutex_);
            draining_.store(false);
        }
    }

    bool is_stopped() const {
        return stop_.load(std::memory_order_acquire);
    }

    bool is_draining() const {
        return draining_.load(std::memory_order_acquire);
    }

    bool is_running() const {
        return !stop_.load(std::memory_order_acquire) ||
               draining_.load(std::memory_order_acquire);
    }

private:

    void launch_worker_threads() {
        for (size_t i = 0; i < ThreadCount; ++i) {
            threads_[i] = std::thread([this, i]() {
                auto token = cancel_tokens_[i];
                safe_launch_function([this, i, token]() {
                    std::string thread_name =
                        name_ + "_thread_" + std::to_string(i);
                    pthread_setname_np(pthread_self(), thread_name.c_str());
                    worker_pool(thread_name, i, token);
                });
            });
        }
        watchdog_thread_ = std::thread(
            [this]() { safe_launch_function([this]() { watchdog_loop(); }); });
    }

    void worker_pool(const std::string &thread_name, size_t thread_index,
                     std::shared_ptr<std::atomic<bool>> cancel_token) {
        // Run the thread's main loop
        LCWeightedRoundRobinScheduler<ContextType, PriorityType> scheduler_;
        ContextType                                              context;
        while (true) {
            if (cancel_token->load(std::memory_order_relaxed)) {
                break;
            }
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(5), [this]() {
                    return stop_.load() || !task_queue_.is_empty();
                });
            }
            if (scheduler_.try_schedule(task_queue_, context)) {
                last_heartbeat_[thread_index].store(
                    std::chrono::steady_clock::now(),
                    std::memory_order_relaxed);
                if (!context.is_cancelled()) {
                    context();
                }
                continue;
            }

            if (stop_.load() && task_queue_.is_empty()) {
                break;
            }
        }
    }

    void watchdog_loop() {
        const auto timeout_duration =
            std::chrono::seconds(10);  // Adjust as needed
        while (!stop_.load()) {
            std::unique_lock<std::mutex> lock(watchdog_mutex_);
            watchdog_cv_.wait_for(lock, timeout_duration, [this]() {
                return stop_.load();
            });
            for (size_t i = 0; i < ThreadCount; ++i) {
                auto last = last_heartbeat_[i].load(std::memory_order_relaxed);
                if (std::chrono::steady_clock::now() - last >
                    timeout_duration) {
                    // TODO log and restart thread
                    std::string thread_name =
                        name_ + "_thread_" + std::to_string(i);
                    std::cerr << "Thread " << thread_name
                              << " is not responding, restarting...\n";

                    cancel_tokens_[i]->store(true, std::memory_order_relaxed);

                    if (threads_[i].joinable()) {
                        threads_[i].join();
                    }

                    cancel_tokens_[i]->store(false, std::memory_order_acq_rel);
                    last_heartbeat_[i].store(std::chrono::steady_clock::now(),
                                             std::memory_order_relaxed);
                    threads_[i] = std::thread([this, i]() {
                        auto token = cancel_tokens_[i];
                        safe_launch_function([this, i, token]() {
                            std::string thread_name =
                                name_ + "_thread_" + std::to_string(i);
                            pthread_setname_np(pthread_self(),
                                               thread_name.c_str());
                            worker_pool(thread_name, i, token);
                        });
                    });
                }
            }
        }
    }

    void join_threads() {
        for (auto &thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        if (watchdog_thread_.joinable()) {
            watchdog_thread_.join();
        }
    }

    LCMPMCMultiPriorityQueue<ContextType, PriorityType> task_queue_;

    const std::string                    name_;
    std::array<std::thread, ThreadCount> threads_;
    std::atomic<bool>                    stop_;
    std::atomic<bool>                    draining_;
    std::condition_variable              cv_;
    std::mutex                           mutex_;

    std::array<std::atomic<std::chrono::steady_clock::time_point>, ThreadCount>
                                                                last_heartbeat_;
    std::array<std::shared_ptr<std::atomic<bool>>, ThreadCount> cancel_tokens_;
    std::thread             watchdog_thread_;
    std::mutex              watchdog_mutex_;
    std::condition_variable watchdog_cv_;
};

LC_FILESYSTEM_NAMESPACE_END
LC_NAMESPACE_END

#endif  // LC_THREAD_POOL_H
