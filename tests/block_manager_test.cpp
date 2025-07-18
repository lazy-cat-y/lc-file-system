#include <filesystem>

#include "gtest/gtest.h"
#include "lc_block_manager.h"
using namespace lc::fs;

class LCBlockManagerTest : public ::testing::Test {
protected:

    void SetUp() override {
        // 1 GiB = 1024 * 1024 * 1024
    }

    void TearDown() override {}
};
