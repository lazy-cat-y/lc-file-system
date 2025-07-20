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
        block_device.reset();
        if (buffer_pool) {
            buffer_pool->stop();
        }
        buffer_pool.reset();
        block_manager.reset();
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
