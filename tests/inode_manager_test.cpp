

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>

#include "lc_block_manager.h"
#include "lc_inode_manager.h"

using namespace lc::fs;

class LCInodeBitmapTest : public ::testing::Test {
protected:
    static constexpr const char *TEST_IMG_PATH =
        "./test_imgs/inode_manager_test.img";

    void SetUp() override {
        std::filesystem::create_directory("./test_imgs");
        LCBlockManager::format(TEST_IMG_PATH, 16 * 1024 * 1024);  // 16 MiB
        LCBlockManager blockManager_ = LCBlockManager(TEST_IMG_PATH);
        const auto    &header        = blockManager_.get_header();

        uint32_t inode_bitmap_start = header->inode_bitmap_start;
        LCBlock  bitmap_block;
        block_clear(&bitmap_block);

        uint8_t pattern = 0b10101010;
        memcpy(block_as(&bitmap_block), &pattern, 1);

        blockManager_.write_block(inode_bitmap_start, bitmap_block);
    }

    void TearDown() override {
        std::filesystem::remove(TEST_IMG_PATH);
        std::filesystem::remove("./test_imgs");
    }
};

TEST_F(LCInodeBitmapTest, ReadInodeBitmapCorrectly) {
    auto           bm = std::make_shared<LCBlockManager>(TEST_IMG_PATH);
    LCInodeManager im(bm);
    LCInodeManagerTestAccess::read_inode_bitmap(im);
    uint8_t *bitmap = LCInodeManagerTestAccess::get_inode_bitmap(im);

    ASSERT_NE(LCInodeManagerTestAccess::get_inode_bitmap(im), nullptr);
    EXPECT_EQ(LCInodeManagerTestAccess::get_inode_bitmap(im)[0], 0b10101010);
}

TEST_F(LCInodeBitmapTest, BitmapEdgeWriteAndReadBack) {
    auto bm = std::make_shared<LCBlockManager>(TEST_IMG_PATH);

    {
        LCInodeManager im(bm);
        LCInodeManagerTestAccess::read_inode_bitmap(im);
        uint8_t       *bitmap = LCInodeManagerTestAccess::get_inode_bitmap(im);
        const uint32_t inode_count = bm->get_header()->inode_count;

        LCInodeManagerTestAccess::set_inode_bitmap_bit(im, inode_count - 1);
        LCInodeManagerTestAccess::write_inode_bitmap(im);
    }

    {
        LCInodeManager im(bm);
        LCInodeManagerTestAccess::read_inode_bitmap(im);
        uint8_t       *bitmap = LCInodeManagerTestAccess::get_inode_bitmap(im);
        const uint32_t inode_count = bm->get_header()->inode_count;

        EXPECT_TRUE(
            LCInodeManagerTestAccess::test_inode_bitmap_bit(im,
                                                            inode_count - 1));
    }
}
