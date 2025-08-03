#ifndef LC_THREAD_POOL_H
#define LC_THREAD_POOL_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "lc_configs.h"
#include "lc_mpmc_queue.h"

LC_NAMESPACE_BEGIN
LC_FILESYSTEM_NAMESPACE_BEGIN

template <typename PriorityWeightType> struct LCPriorityWeights;

template <> struct LCPriorityWeights<LCWriteTaskPriority> {
    static constexpr std::array<
        uint32_t, static_cast<size_t>(LCWriteTaskPriority::NUM_PRIORITIES)>
    get_weights() {
        return {10, 8, 5, 3, 1};
    }
};

template <> struct LCPriorityWeights<LCReadTaskPriority> {
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
        if (std::all_of(weights_.back(), weights_.end(), [](uint32_t w) {
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

    template <typename U> static constexpr bool always_false_v = false;

    size_t current_index_ = 0;
    std::array<uint32_t, static_cast<size_t>(PriorityType::NUM_PRIORITIES)>
        weights_;
};

class LCThreadPool {};

LC_FILESYSTEM_NAMESPACE_END
LC_NAMESPACE_END

#endif  // LC_THREAD_POOL_H
