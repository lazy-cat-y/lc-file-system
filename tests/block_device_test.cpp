

#include "gtest/gtest.h"
#include "lc_block.h"
#include "lc_block_device.h"
#include "lc_image_format.h"
#include "lc_inode.h"
#include "lc_memory.h"
#include "lc_test_config.h"
#include "lc_utils.h"

using namespace lc::fs;

class LCBlockDeviceTest : public ::testing::Test {
protected:
    std::string  test_img_path = get_test_img("block_device_test.img");
    LCSuperBlock expected_header {
        .magic              = BLOCK_MAGIC_NUMBER,
        .block_size         = DEFAULT_BLOCK_SIZE,
        .total_blocks       = 262144,  // 1 GiB
        .block_bitmap_start = 1,
        .inode_count        = 65536,   // 16 KiB inodes
        .inode_bitmap_start = 9,
        .inode_block_start  = 11,
        .inode_block_count  = 4096,
        .data_start         = 4107  // 11 + 4096

    };

    void SetUp() override {
        init_test_img_dir();
        lc_format_image(test_img_path, 1024ull * 1024 * 1024);  // 1 GiB
    }

    void TearDown() override {
        clear_test_img_dir();
    }
};

TEST_F(LCBlockDeviceTest, SuperBlockInitialization) {
    LCBlockDevice device(test_img_path);
    LCSuperBlock  header = device.get_super_block();

    EXPECT_EQ(header.magic, expected_header.magic);
    EXPECT_EQ(header.block_size, expected_header.block_size);
    EXPECT_EQ(header.total_blocks, expected_header.total_blocks);
    EXPECT_EQ(header.block_bitmap_start, expected_header.block_bitmap_start);
    EXPECT_EQ(header.inode_count, expected_header.inode_count);
    EXPECT_EQ(header.inode_bitmap_start, expected_header.inode_bitmap_start);
    EXPECT_EQ(header.inode_block_start, expected_header.inode_block_start);
    EXPECT_EQ(header.inode_block_count, expected_header.inode_block_count);
    EXPECT_EQ(header.data_start, expected_header.data_start);
}

// TEST inode initialization
TEST_F(LCBlockDeviceTest, InodeInitialization) {
    LCBlockDevice device(test_img_path);
    LCBlock       inode_block {};
    block_clear(&inode_block);
    device.read_block(device.get_super_block().inode_block_start,
                      inode_block);  // Read the first block (superblock)
    LCInode inode {};
    lc_memcpy(&inode, block_as(&inode_block), LC_INODE_SIZE);

    uint32_t ff_buf[sizeof(inode.block_ptr) / sizeof(uint32_t)];
    std::fill_n(reinterpret_cast<uint8_t *>(ff_buf),
                sizeof(inode.block_ptr),
                0xff);
    EXPECT_TRUE(lc_memcmp(inode.block_ptr, ff_buf, sizeof(inode.block_ptr)) ==
                0)
        << "Inode block pointers should be initialized to 0xFF";
}

TEST_F(LCBlockDeviceTest, BlockReadWriteRoundtrip) {
    LCBlockDevice      device(test_img_path);
    constexpr uint32_t test_block_id = 8;

    LCBlock write_block;
    block_clear(&write_block);

    // Write a pattern to block
    uint8_t *data = reinterpret_cast<uint8_t *>(block_as(&write_block));
    for (size_t i = 0; i < DEFAULT_BLOCK_SIZE; ++i) {
        data[i] = static_cast<uint8_t>(i % 256);
    }

    device.write_block(test_block_id, write_block);

    // Read back and compare
    LCBlock read_block;
    block_clear(&read_block);

    device.read_block(test_block_id, read_block);

    EXPECT_EQ(memcmp(block_as(&write_block),
                     block_as(&read_block),
                     DEFAULT_BLOCK_SIZE),
              0);
}
