#include <sys/types.h>

#include <cstdint>
#include <cstring>

#include "gtest/gtest.h"
#include "lc_block.h"
#include "lc_block_buffer.h"
#include "lc_block_device.h"
#include "lc_image_format.h"
#include "lc_test_config.h"

using namespace lc::fs;

class LCBlockBufferMultithreadTest : public ::testing::Test {
protected:
    std::string test_img_path = get_test_img("block_buffer_mt_test.img");
    std::unique_ptr<LCBlockDevice> block_device;

    void SetUp() override {
        init_test_img_dir();
        lc_format_image(test_img_path, 512ull * 1024 * 1024);
        block_device = std::make_unique<LCBlockDevice>(test_img_path);
    }

    void TearDown() override {
        block_device.reset();
        clear_test_img_dir();
    }
};

TEST_F(LCBlockBufferMultithreadTest, ConcurrentWriteDifferentBlocks) {
    constexpr uint32_t num_threads           = 8;
    constexpr uint32_t num_blocks_per_thread = 10;
    constexpr uint32_t pool_size             = 32;

    LCBlockBufferPool        buffer_pool(block_device.get(), pool_size, 100);
    std::vector<std::thread> threads;

    for (uint32_t t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            for (uint32_t i = 0; i < num_blocks_per_thread; ++i) {
                uint32_t block_id = t * num_blocks_per_thread + i + 1;
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

    for (uint32_t block_id = 1;
         block_id < num_threads * num_blocks_per_thread + 1;
         ++block_id) {
        LCBlock result;
        block_device->read_block(block_id, result);
        for (uint32_t j = 0; j < DEFAULT_BLOCK_SIZE; ++j) {
            ASSERT_EQ(result.data[j], static_cast<uint8_t>(block_id))
                << "Mismatch in block " << block_id << " at byte " << j;
        }
    }
}

TEST_F(LCBlockBufferMultithreadTest, ConcurrentReadWriteSameBlocks) {
    constexpr uint32_t pool_size = 16;
    constexpr uint32_t block_id  = 123;

    LCBlockBufferPool        buffer_pool(block_device.get(), pool_size, 100);
    std::vector<std::thread> threads;

    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&]() {
            LCBlock block;
            std::memset(block.data, 88, DEFAULT_BLOCK_SIZE);
            for (int i = 1; i < 51; ++i) {
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
    block_device->read_block(block_id, result);
    for (uint32_t j = 0; j < DEFAULT_BLOCK_SIZE; ++j) {
        ASSERT_EQ(result.data[j], 88);
    }
}

TEST_F(LCBlockBufferMultithreadTest, MixedConcurrentOps) {
    constexpr uint32_t pool_size = 64;
    LCBlockBufferPool  buffer_pool(block_device.get(), pool_size, 10);

    std::vector<std::thread> threads;

    for (uint32_t t = 0; t < 16; ++t) {
        threads.emplace_back([&, t]() {
            for (uint32_t i = 0; i < 20; ++i) {
                uint32_t block_id = (t * 31 + i) % 128;  // some hash spread
                if (block_id == 0) {
                    continue;
                }
                LCBlock block {};
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
