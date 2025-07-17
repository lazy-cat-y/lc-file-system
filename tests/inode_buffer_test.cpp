
#include <string>

#include "gtest/gtest.h"
#include "lc_block.h"
#include "lc_block_buffer.h"
#include "lc_block_manager.h"
#include "lc_inode_buffer.h"
#include "lc_test_config.h"

using namespace lc::fs;

class LCInodeBufferTest : public ::testing::Test {

protected:
    std::string        test_img_path = get_test_img("test_inode_buffer.img");
    LCBlockManager    *block_manager;
    LCBlockHeader      header;
    LCBlockBufferPool *block_buffer_pool;
    LCInodeBufferPool *inode_buffer_pool;

    void SetUp() override {
        init_test_img_dir();
        LCBlockManager::format(test_img_path, 1024ull * 1024 * 1024);  // 1 GiB
        block_manager     = new LCBlockManager(test_img_path);
        header            = *block_manager->get_header();
        block_buffer_pool = new LCBlockBufferPool(block_manager, 64, 100);
        block_buffer_pool->start();
        inode_buffer_pool = new LCInodeBufferPool(block_buffer_pool,
                                                  64,
                                                  1,
                                                  header.inode_start,
                                                  header.inode_count);
        inode_buffer_pool->start();
    }

    void TearDown() override {
        if (inode_buffer_pool) {
            inode_buffer_pool->stop();
        }

        if (block_buffer_pool) {
            block_buffer_pool->stop();
        }

        if (inode_buffer_pool) {
            delete inode_buffer_pool;
            inode_buffer_pool = nullptr;
        }

        if (block_buffer_pool) {
            delete block_buffer_pool;
            block_buffer_pool = nullptr;
        }

        if (block_manager) {
            delete block_manager;
            block_manager = nullptr;
        }
        clear_test_img_dir();
    }
};

TEST_F(LCInodeBufferTest, WriteReadInode) {
    LCInode inode {};
    inode.uid          = 1001;
    inode.gid          = 1002;
    inode.size         = 123456;
    inode.block_ptr[0] = 42;

    uint32_t test_ino = 0;

    inode_buffer_pool->write_inode(test_ino, inode);
    inode_buffer_pool->unpin_inode(test_ino);
    LCInode loaded = inode_buffer_pool->read_inode(test_ino);
    inode_buffer_pool->unpin_inode(test_ino);

    EXPECT_EQ(loaded.uid, inode.uid);
    EXPECT_EQ(loaded.gid, inode.gid);
    EXPECT_EQ(loaded.size, inode.size);
    EXPECT_EQ(loaded.block_ptr[0], inode.block_ptr[0]);
}

TEST_F(LCInodeBufferTest, MarkDirtyAndFlush) {
    uint32_t test_ino = 1;

    {
        auto guard = inode_buffer_pool->access_frame_lock(test_ino);

        guard.frame->inode.uid = 777;
        guard.mark_dirty();
    }
    inode_buffer_pool->unpin_inode(test_ino);
    inode_buffer_pool->flush_inode(test_ino);

    LCInode reloaded = inode_buffer_pool->read_inode(test_ino);
    inode_buffer_pool->unpin_inode(test_ino);
    EXPECT_EQ(reloaded.uid, 777);
}

TEST_F(LCInodeBufferTest, MultipleWriteRead) {
    for (uint32_t i = 0; i < 10; ++i) {
        LCInode inode {};
        inode.uid  = i * 10;
        inode.size = i * 100;

        inode_buffer_pool->write_inode(i, inode);
        inode_buffer_pool->unpin_inode(i);
    }

    for (uint32_t i = 0; i < 10; ++i) {
        LCInode inode = inode_buffer_pool->read_inode(i);
        EXPECT_EQ(inode.uid, i * 10);
        EXPECT_EQ(inode.size, i * 100);
        inode_buffer_pool->unpin_inode(i);
    }
}

TEST_F(LCInodeBufferTest, MixedConcurrentOps) {
    constexpr uint32_t thread_count        = 16;
    constexpr uint32_t op_count_per_thread = 50;
    constexpr uint32_t inode_range         = 128;

    std::vector<std::thread> threads;

    for (uint32_t t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t]() {
            for (uint32_t i = 0; i < op_count_per_thread; ++i) {
                uint32_t inode_id = (t * 37 + i * 13) % inode_range;

                LCInode inode {};
                inode.uid          = inode_id + t;
                inode.gid          = inode_id * 2 + t;
                inode.size         = inode_id * 100 + t;
                inode.block_ptr[0] = inode_id;

                inode_buffer_pool->write_inode(inode_id, inode);
                inode_buffer_pool->unpin_inode(inode_id);

                LCInode loaded = inode_buffer_pool->read_inode(inode_id);
                EXPECT_EQ(loaded.block_ptr[0], inode_id);
                inode_buffer_pool->unpin_inode(inode_id);

                if (inode_id % 7 == 0) {
                    inode_buffer_pool->flush_inode(inode_id);
                }

                if (inode_id % 5 == 0) {
                    inode_buffer_pool->unpin_inode(inode_id);
                }
            }
        });
    }

    for (auto &th : threads) {
        th.join();
    }

    inode_buffer_pool->flush_all();
}
