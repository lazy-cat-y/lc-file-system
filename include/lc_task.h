#ifndef LC_TASK_H
#define LC_TASK_H

#include <algorithm>

#include "lc_configs.h"

struct Task {
    virtual void run() = 0;
    virtual ~Task()  = default;
};

template <typename Fn>
struct LambdaTask : Task {
    Fn fn;

    LC_EXPLICIT LambdaTask(Fn &&f) : fn(std::move(f)) {}

    void run() override {
        fn();
    }
};

#endif  // LC_TASK_H
