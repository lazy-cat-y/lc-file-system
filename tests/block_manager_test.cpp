#include <filesystem>
#include "gtest/gtest.h"

#include "lc_block_manager.h"
using namespace lc::fs;

class LCBlockManagerTest : public ::testing::Test {
protected:
    std::string test_img_path = "./test_imgs/test.img";

    void SetUp() override {
        // 1 GiB = 1024 * 1024 * 1024
        std::filesystem::create_directory("./test_imgs");
        LCBlockManager::format(test_img_path, 1024ull * 1024 * 1024);
    }

    void TearDown() override {
        std::filesystem::remove(test_img_path);
        std::filesystem::remove("./test_imgs");
    }
};

TEST_F(LCBlockManagerTest, HeaderValidationAfterFormat) {
    LCBlockManager manager(test_img_path);
    const LCSuperBlock* header = manager.get_header();

    EXPECT_EQ(header->magic, BLOCK_MAGIC_NUMBER);
    EXPECT_EQ(header->block_size, DEFAULT_BLOCK_SIZE);
    EXPECT_EQ(header->total_blocks, 1024 * 1024 * 1024 / DEFAULT_BLOCK_SIZE);
    EXPECT_EQ(header->inode_count, 1024 * 1024 * 1024 / (16 * 1024));
}

TEST_F(LCBlockManagerTest, WriteAndReadBlock) {
    LCBlockManager manager(test_img_path);

    LCBlock write_block;
    block_clear(&write_block);
    const char* msg = "Hello LCBlock!";
    memcpy(write_block.data, msg, strlen(msg) + 1);

    manager.write_block(10, write_block);

    LCBlock read_block;
    manager.read_block(10, read_block);

    EXPECT_STREQ(reinterpret_cast<const char*>(read_block.data), msg);
}