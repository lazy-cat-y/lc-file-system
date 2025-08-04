#ifndef LC_TASK_H
#define LC_TASK_H

#include <algorithm>

#include "lc_configs.h"

struct LCTask {
    virtual void run() = 0;
    virtual ~LCTask()  = default;
};

template <typename Fn>
struct LCLambdaTask : LCTask {
    Fn fn;

    LC_EXPLICIT LCLambdaTask(Fn &&f) : fn(std::move(f)) {}

    void run() override {
        fn();
    }
};

#endif  // LC_TASK_H
