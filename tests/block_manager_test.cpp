#include "gtest/gtest.h"
#include "lc_block_manager.h"
#include "lc_image_format.h"
#include "lc_test_config.h"

using namespace lc::fs;

class LCBlockManagerTest : public ::testing::Test {
protected:

    std::string test_img_path = get_test_img("block_manager_test.img");
    std::unique_ptr<LCBlockDevice>     block_device;
    std::unique_ptr<LCBlockBufferPool> buffer_pool;
    std::unique_ptr<LCBlockManager>    block_manager;

    void SetUp() override {
        // 1 GiB = 1024 * 1024 * 1024
        init_test_img_dir();
        lc_format_image(test_img_path, 1024ull * 1024 * 1024);
        block_device = std::make_unique<LCBlockDevice>(test_img_path);
        const LCSuperBlock &super_block = block_device->get_super_block();
        buffer_pool =
            std::make_unique<LCBlockBufferPool>(block_device.get(), 64, 100);
        buffer_pool->start();
        block_manager =
            std::make_unique<LCBlockManager>(buffer_pool.get(), &super_block);
    }

    void TearDown() override {
        if (buffer_pool) {
            buffer_pool->stop();
        }
        buffer_pool.reset();
        block_manager.reset();
        block_device.reset();
        clear_test_img_dir();
    }
};

TEST_F(LCBlockManagerTest, AllocAndFreeBlockShouldWork) {
    // Allocate a block
    uint32_t block_id = block_manager->alloc_block();
    ASSERT_NE(block_id, LC_BLOCK_ILLEGAL_ID);

    // Write data into the allocated block
    LCBlock write_block;
    block_clear(&write_block);
    uint8_t *data = block_as(&write_block);
    for (int i = 0; i < DEFAULT_BLOCK_SIZE; ++i) {
        data[i] = static_cast<uint8_t>(i % 256);
    }
    block_manager->write_block(block_id, write_block);

    // Read the block and compare
    LCBlock read_block;
    block_manager->read_block(block_id, read_block);
    uint8_t *read_data = block_as(&read_block);
    for (int i = 0; i < DEFAULT_BLOCK_SIZE; ++i) {
        ASSERT_EQ(read_data[i], static_cast<uint8_t>(i % 256));
    }

    // Free the block
    block_manager->free_block(block_id);

    // Allocate again and should get same or another block
    uint32_t new_block_id = block_manager->alloc_block();
    ASSERT_NE(new_block_id, LC_BLOCK_ILLEGAL_ID);
    // Not guaranteed to be same, but if your allocator is simple first-fit, it
    // likely is ASSERT_EQ(new_block_id, block_id); // optional
}

/* 1. 连续分配直到跨越 bitmap 块 */
TEST_F(LCBlockManagerTest, AllocateAcrossBitmapBlockBoundary) {
    // 一个 bitmap 数据块能标记的逻辑块数 = 4096 * 8 = 32768
    constexpr uint32_t kBlocksPerBitmapBlock = DEFAULT_BLOCK_SIZE * 8;
    const uint32_t start = block_manager->alloc_block();  // 先拿一个起点
    ASSERT_NE(start, LC_BLOCK_ILLEGAL_ID);

    std::vector<uint32_t> ids = {start};

    // 再分配 kBlocksPerBitmapBlock（+1 保险）个块，确保光标越界
    for (uint32_t i = 0; i < kBlocksPerBitmapBlock; ++i) {
        uint32_t id = block_manager->alloc_block();
        ASSERT_NE(id, LC_BLOCK_ILLEGAL_ID);
        ids.push_back(id);
    }

    // 检查：最后一个 id 一定和第一个 id 相差 > kBlocksPerBitmapBlock-1
    ASSERT_EQ(ids.back() - ids.front(), kBlocksPerBitmapBlock);

    // 可选：验证全部连续
    for (size_t i = 1; i < ids.size(); ++i) {
        ASSERT_EQ(ids[i], ids[i - 1] + 1);
    }

    // 清理
    for (uint32_t id : ids) {
        block_manager->free_block(id);
    }
}

/* 2. 交错释放再重分配 — 检查回收正确性 */
TEST_F(LCBlockManagerTest, AllocateFreeInterleavedShouldReuseLowestFree) {
    constexpr uint32_t    kBatch = 256;
    std::vector<uint32_t> ids;
    ids.reserve(kBatch);

    // 分配一批
    for (uint32_t i = 0; i < kBatch; ++i) {
        ids.push_back(block_manager->alloc_block());
    }
    ASSERT_EQ(ids.size(), kBatch);

    // 释放偶数下标
    for (size_t i = 0; i < ids.size(); i += 2) {
        block_manager->free_block(ids[i]);
    }

    // 再次分配 kBatch/2 个块，应该优先拿到那些偶数位置
    std::vector<uint32_t> reused;
    for (uint32_t i = 0; i < kBatch / 2; ++i) {
        uint32_t id = block_manager->alloc_block();
        reused.push_back(id);
    }

    // reused 集合应完全包含之前释放的偶数 id（无固定顺序，用集合比较）
    std::sort(reused.begin(), reused.end());
    std::vector<uint32_t> expected;
    for (size_t i = 0; i < ids.size(); i += 2) {
        expected.push_back(ids[i]);
    }
    std::sort(expected.begin(), expected.end());
    ASSERT_EQ(reused, expected);

    // 全部释放
    for (uint32_t id : ids) {
        block_manager->free_block(id);
    }
}

/* 3. 多线程分配 + 读取写入验证 */
TEST_F(LCBlockManagerTest, ConcurrentAllocateWriteReadFree) {
    constexpr uint32_t       kThreads   = 8;
    constexpr uint32_t       kPerThread = 512;  // 每线程分配块数
    std::vector<std::thread> workers;
    std::mutex               all_ids_mu;
    std::vector<uint32_t>    all_ids;

    for (uint32_t t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t]() {
            std::vector<uint32_t> local_ids;
            local_ids.reserve(kPerThread);

            // 1) 分配并写入
            for (uint32_t i = 0; i < kPerThread; ++i) {
                uint32_t id = block_manager->alloc_block();
                ASSERT_NE(id, LC_BLOCK_ILLEGAL_ID);
                local_ids.push_back(id);

                LCBlock blk;
                block_clear(&blk);
                std::fill_n(block_as(&blk),
                            DEFAULT_BLOCK_SIZE,
                            static_cast<uint8_t>((id + t) & 0xFF));
                block_manager->write_block(id, blk);
            }

            // 2) 读回校验
            for (uint32_t id : local_ids) {
                LCBlock blk;
                block_manager->read_block(id, blk);
                uint8_t expect = static_cast<uint8_t>((id + t) & 0xFF);
                for (uint32_t off = 0; off < DEFAULT_BLOCK_SIZE; ++off) {
                    ASSERT_EQ(block_as(&blk)[off], expect);
                }
            }

            // 3) 释放
            for (uint32_t id : local_ids) {
                block_manager->free_block(id);
            }

            // 收集到全局，便于后续可能的唯一性检查
            {
                std::lock_guard<std::mutex> lg(all_ids_mu);
                all_ids.insert(all_ids.end(),
                               std::make_move_iterator(local_ids.begin()),
                               std::make_move_iterator(local_ids.end()));
            }
        });
    }
    for (auto &th : workers) {
        th.join();
    }

    // （可选）检查是否存在重复 id
    std::sort(all_ids.begin(), all_ids.end());
    auto dup = std::adjacent_find(all_ids.begin(), all_ids.end());
    ASSERT_EQ(dup, all_ids.end());  // 若不重复，adjacent_find 返回 end()
}
