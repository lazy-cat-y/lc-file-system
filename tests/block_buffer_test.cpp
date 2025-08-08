#include <sys/types.h>

#include <cstdint>
#include <cstring>
#include <memory>

#include "gtest/gtest.h"
#include "lc_block.h"
#include "lc_block_buffer.h"
#include "lc_block_device.h"
#include "lc_image_format.h"
#include "lc_mpmc_queue.h"
#include "lc_test_config.h"
#include "lc_thread_pool.h"

using namespace lc::fs;

class LCBlockBufferMultithreadTest : public ::testing::Test {
protected:
    std::string test_img_path = get_test_img("block_buffer_mt_test.img");
    std::unique_ptr<LCBlockDevice> block_device;
    std::unique_ptr<LCSuperBlock>  super_block;

    void SetUp() override {
        init_test_img_dir();
        lc_format_image(test_img_path, 512ull * 1024 * 1024);
        block_device = std::make_unique<LCBlockDevice>(test_img_path);
        super_block =
            std::make_unique<LCSuperBlock>(block_device->get_super_block());
    }

    void TearDown() override {
        block_device.reset();
        super_block.reset();
        clear_test_img_dir();
    }
};

TEST_F(LCBlockBufferMultithreadTest, ConcurrentWriteDifferentBlocks) {
    constexpr uint32_t num_threads           = 4;
    constexpr uint32_t num_blocks_per_thread = 10;
    constexpr uint32_t pool_size             = 32;

    auto write_thread_pool =
        std::make_shared<LCThreadPool<LCTaskPriority>>("write_pool", 16);
    auto read_thread_pool =
        std::make_shared<LCThreadPool<LCTaskPriority>>("read_pool", 16);

    LCBlockBufferPool buffer_pool(block_device.get(),
                                  pool_size,
                                  /*frame_interval_ms=*/10,
                                  write_thread_pool,
                                  read_thread_pool);
    buffer_pool.start();

    std::vector<std::thread> threads;
    for (uint32_t t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t]() {
            for (uint32_t i = 0; i < num_blocks_per_thread; ++i) {
                uint32_t block_id = super_block->data_start +
                                    t * num_blocks_per_thread +
                                    i;  // <-- include i
                LCBlock block {};
                std::memset(block.data,
                            static_cast<int>(block_id & 0xFF),
                            DEFAULT_BLOCK_SIZE);
                buffer_pool.write_block(block_id,
                                        LCTaskPriority::High,
                                        block.data,
                                        DEFAULT_BLOCK_SIZE);
            }
        });
    }
    for (auto &th : threads) {
        th.join();
    }

    buffer_pool.flush_all(LCTaskPriority::Critical);
    write_thread_pool->wait_for_all_tasks();
    buffer_pool.stop();

    // Verify the exact range we wrote.
    uint32_t first = super_block->data_start;
    uint32_t last =
        first + num_threads * num_blocks_per_thread;  // exclusive end
    for (uint32_t block_id = first; block_id < last; ++block_id) {
        LCBlock result {};
        block_device->read_block(block_id, result);
        for (uint32_t j = 0; j < DEFAULT_BLOCK_SIZE; ++j) {
            ASSERT_EQ(result.data[j], static_cast<uint8_t>(block_id & 0xFF))
                << "Mismatch in block " << block_id << " at byte " << j;
        }
    }
}

TEST_F(LCBlockBufferMultithreadTest, ConcurrentReadWriteSameBlocks) {
    constexpr uint32_t pool_size = 16;
    const uint32_t     block_id =
        super_block->data_start + 123;  // ensure inside data area

    auto write_thread_pool =
        std::make_shared<LCThreadPool<LCTaskPriority>>("write_pool", 8);
    auto read_thread_pool =
        std::make_shared<LCThreadPool<LCTaskPriority>>("read_pool", 8);

    LCBlockBufferPool buffer_pool(block_device.get(),
                                  pool_size,
                                  /*frame_interval_ms=*/10,
                                  write_thread_pool,
                                  read_thread_pool);
    buffer_pool.start();

    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&]() {
            LCBlock block {};
            std::memset(block.data, 88, DEFAULT_BLOCK_SIZE);
            for (int i = 1; i < 51; ++i) {
                buffer_pool.write_block(block_id,
                                        LCTaskPriority::High,
                                        block.data,
                                        DEFAULT_BLOCK_SIZE);

                LCBlock tmp {};
                buffer_pool.read_block(block_id,
                                       LCTaskPriority::High,
                                       tmp.data,
                                       DEFAULT_BLOCK_SIZE);
                // no unpin needed: read copies into caller buffer
            }
        });
    }
    for (auto &th : threads) {
        th.join();
    }

    buffer_pool.flush_block(block_id,
                            LCTaskPriority::Critical,
                            /*cancel_token=*/nullptr);
    buffer_pool.stop();

    LCBlock result {};
    block_device->read_block(block_id, result);
    for (uint32_t j = 0; j < DEFAULT_BLOCK_SIZE; ++j) {
        ASSERT_EQ(result.data[j], 88);
    }
}

TEST_F(LCBlockBufferMultithreadTest, MixedConcurrentOps) {
    constexpr uint32_t pool_size = 64;

    auto write_thread_pool =
        std::make_shared<LCThreadPool<LCTaskPriority>>("write_pool", 16);
    auto read_thread_pool =
        std::make_shared<LCThreadPool<LCTaskPriority>>("read_pool", 16);

    LCBlockBufferPool buffer_pool(block_device.get(),
                                  pool_size,
                                  /*frame_interval_ms=*/10,
                                  write_thread_pool,
                                  read_thread_pool);
    buffer_pool.start();

    std::vector<std::thread> threads;
    for (uint32_t t = 0; t < 16; ++t) {
        threads.emplace_back([&, t]() {
            for (uint32_t i = 0; i < 20; ++i) {
                // IDs in [data_start, data_start + 127]
                uint32_t offset   = (t * 31u + i) % 128u;
                uint32_t block_id = super_block->data_start + offset;

                LCBlock block {};
                std::memset(block.data,
                            static_cast<int>(block_id & 0xFF),
                            DEFAULT_BLOCK_SIZE);

                buffer_pool.write_block(block_id,
                                        LCTaskPriority::Normal,
                                        block.data,
                                        DEFAULT_BLOCK_SIZE);

                LCBlock tmp {};
                buffer_pool.read_block(block_id,
                                       LCTaskPriority::Normal,
                                       tmp.data,
                                       DEFAULT_BLOCK_SIZE);

                if ((block_id % 5u) == 0u) {
                    buffer_pool.flush_block(block_id,
                                            LCTaskPriority::Low,
                                            /*cancel_token=*/nullptr);
                }
            }
        });
    }
    for (auto &th : threads) {
        th.join();
    }

    buffer_pool.flush_all(LCTaskPriority::Critical);
    buffer_pool.stop();
}
