#include <sys/types.h>

#include <cstdint>
#include <cstring>
#include <filesystem>

#include "gtest/gtest.h"
#include "lc_block.h"
#include "lc_block_buffer.h"
#include "lc_block_manager.h"
using namespace lc::fs;

class LCBlockBufferTest : public ::testing::Test {
protected:
    std::string test_img_path = "./test_imgs/block_buffer_test.img";
    std::unique_ptr<LCBlockManager> block_manager;

    void SetUp() override {
        // 1 GiB = 1024 * 1024 * 1024
        if (std::filesystem::exists("./test_imgs")) {
            // delete all the existing files under the test directory
            std::filesystem::remove_all("./test_imgs");
        }
        std::filesystem::create_directory("./test_imgs");
        LCBlockManager::format(test_img_path, 1024ull * 1024 * 1024);
        block_manager = std::make_unique<LCBlockManager>(test_img_path);
    }

    void TearDown() override {
        block_manager.reset();
        std::filesystem::remove(test_img_path);
    }
};

TEST_F(LCBlockBufferTest, BasicReadWriteTest) {
    LCBlockBufferPool buffer_pool(block_manager.get(), 10, 100);
    LCBlock           block;
    memset(block.data, 42, DEFAULT_BLOCK_SIZE);
    buffer_pool.write_block(10, block);

    LCBlock result = buffer_pool.read_block(10);
    ASSERT_EQ(memcmp(result.data, block.data, DEFAULT_BLOCK_SIZE), 0);
}

TEST_F(LCBlockBufferTest, DirtyBlockFlushTest) {
    LCBlockBufferPool buffer_pool(block_manager.get(), 10, 100);
    LCBlock           block;
    uint32_t          block_id = 20;

    memset(block.data, 42, DEFAULT_BLOCK_SIZE);
    buffer_pool.write_block(block_id, block);

    // Flush the block
    buffer_pool.flush_block(block_id);

    LCBlock read_block = buffer_pool.read_block(block_id);
    ASSERT_EQ(memcmp(read_block.data, block.data, DEFAULT_BLOCK_SIZE), 0);

    // Read back to verify from disk
    LCBlock result {};
    block_manager->read_block(block_id, result);
    ASSERT_EQ(memcmp(result.data, block.data, DEFAULT_BLOCK_SIZE), 0);
}

TEST_F(LCBlockBufferTest, FlushAllBlocks) {
    LCBlockBufferPool buffer_pool(block_manager.get(), 10, 100);

    for (uint32_t i = 0; i < 10; ++i) {
        LCBlock block;
        memset(block.data, i + 1, DEFAULT_BLOCK_SIZE);
        buffer_pool.write_block(i, block);
    }

    buffer_pool.flush_all();

    for (uint32_t i = 0; i < 10; ++i) {
        LCBlock result;
        block_manager->read_block(i, result);
        for (uint32_t j = 0; j < DEFAULT_BLOCK_SIZE; ++j) {
            ASSERT_EQ(result.data[j], static_cast<uint8_t>(i + 1))
                << "Block " << i << " data mismatch at byte " << j;
        }
    }
}

TEST_F(LCBlockBufferTest, MultipleWritesAndOverwrites) {
    LCBlockBufferPool buffer_pool(block_manager.get(), 10, 100);
    LCBlock           block1, block2;
    std::memset(block1.data, 1, DEFAULT_BLOCK_SIZE);
    std::memset(block2.data, 2, DEFAULT_BLOCK_SIZE);

    buffer_pool.write_block(42, block1);
    buffer_pool.write_block(42, block2);  // overwrite
    buffer_pool.flush_block(42);

    LCBlock read_back;
    block_manager->read_block(42, read_back);
    for (size_t i = 0; i < DEFAULT_BLOCK_SIZE; ++i) {
        ASSERT_EQ(read_back.data[i], 2);
    }
}

class LCBlockBufferMultithreadTest : public ::testing::Test {
protected:
    std::string test_img_path = "./test_imgs/block_buffer_mt_test.img";
    std::unique_ptr<LCBlockManager> block_manager;

    void SetUp() override {
        if (std::filesystem::exists("./test_imgs")) {
            // delete all the existing files under the test directory
            std::filesystem::remove_all("./test_imgs");
        }
        std::filesystem::create_directory("./test_imgs");
        LCBlockManager::format(test_img_path, 512ull * 1024 * 1024);  // 512 MiB
        block_manager = std::make_unique<LCBlockManager>(test_img_path);
    }

    void TearDown() override {
        block_manager.reset();
        std::filesystem::remove(test_img_path);
        std::filesystem::remove("./test_imgs");
    }
};

TEST_F(LCBlockBufferMultithreadTest, ConcurrentWriteDifferentBlocks) {
    constexpr uint32_t num_threads           = 8;
    constexpr uint32_t num_blocks_per_thread = 10;
    constexpr uint32_t pool_size             = 32;

    LCBlockBufferPool        buffer_pool(block_manager.get(), pool_size, 100);
    std::vector<std::thread> threads;

    for (uint32_t t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            for (uint32_t i = 0; i < num_blocks_per_thread; ++i) {
                uint32_t block_id = t * num_blocks_per_thread + i;
                LCBlock  block;
                std::memset(block.data,
                            static_cast<int>(block_id),
                            DEFAULT_BLOCK_SIZE);
                buffer_pool.write_block(block_id, block);
            }
        });
    }

    for (auto &thread : threads) {
        thread.join();
    }
    buffer_pool.flush_all();

    for (uint32_t block_id = 0; block_id < num_threads * num_blocks_per_thread;
         ++block_id) {
        LCBlock result;
        block_manager->read_block(block_id, result);
        for (uint32_t j = 0; j < DEFAULT_BLOCK_SIZE; ++j) {
            ASSERT_EQ(result.data[j], static_cast<uint8_t>(block_id))
                << "Mismatch in block " << block_id << " at byte " << j;
        }
    }
}

TEST_F(LCBlockBufferMultithreadTest, ConcurrentReadWriteSameBlocks) {
    constexpr uint32_t pool_size = 16;
    constexpr uint32_t block_id  = 123;

    LCBlockBufferPool        buffer_pool(block_manager.get(), pool_size, 100);
    std::vector<std::thread> threads;

    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&]() {
            LCBlock block;
            std::memset(block.data, 88, DEFAULT_BLOCK_SIZE);
            for (int i = 0; i < 50; ++i) {
                buffer_pool.write_block(block_id, block);
                buffer_pool.read_block(block_id);
                buffer_pool.unpin_block(block_id);
            }
        });
    }

    for (auto &thread : threads) {
        thread.join();
    }
    buffer_pool.flush_block(block_id);

    LCBlock result;
    block_manager->read_block(block_id, result);
    for (uint32_t j = 0; j < DEFAULT_BLOCK_SIZE; ++j) {
        ASSERT_EQ(result.data[j], 88);
    }
}

TEST_F(LCBlockBufferMultithreadTest, MixedConcurrentOps) {
    constexpr uint32_t pool_size = 64;
    LCBlockBufferPool  buffer_pool(block_manager.get(), pool_size, 1);

    std::vector<std::thread> threads;

    for (uint32_t t = 0; t < 16; ++t) {
        threads.emplace_back([&, t]() {
            for (uint32_t i = 0; i < 20; ++i) {
                uint32_t block_id = (t * 31 + i) % 128;  // some hash spread
                LCBlock  block;
                std::memset(block.data, block_id, DEFAULT_BLOCK_SIZE);
                buffer_pool.write_block(block_id, block);
                buffer_pool.read_block(block_id);
                buffer_pool.unpin_block(block_id);
                if (block_id % 5 == 0) {
                    buffer_pool.flush_block(block_id);
                }
            }
        });
    }

    for (auto &thread : threads) {
        thread.join();
    }
    buffer_pool.flush_all();
}
