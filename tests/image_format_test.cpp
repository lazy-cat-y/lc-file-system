#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "lc_block.h"
#include "lc_configs.h"
#include "lc_image_format.h"
#include "lc_test_config.h"

LC_NAMESPACE_BEGIN
LC_FILESYSTEM_NAMESPACE_BEGIN

TEST(LCFormatImageTest, CreateAndVerifyImage) {
    std::string test_img_path = get_test_img("format_image_test.img");
    init_test_img_dir();
    uint64_t img_size = 16 * 1024 * 1024;  // 16MB

    // Step 1: Call format
    lc_format_image(test_img_path, img_size);

    // Step 2: Check file exists
    ASSERT_TRUE(std::filesystem::exists(test_img_path));

    // Step 3: Check file size
    auto actual_size = std::filesystem::file_size(test_img_path);
    ASSERT_EQ(actual_size, img_size);

    // Step 4: Read first block and check header
    std::ifstream img_file(test_img_path, std::ios::binary);
    ASSERT_TRUE(img_file.is_open());

    LCBlock header_block {};
    img_file.seekg(0, std::ios::beg);
    img_file.read(reinterpret_cast<char *>(block_as(&header_block)),
                  DEFAULT_BLOCK_SIZE);

    const LCSuperBlock *header =
        reinterpret_cast<LCSuperBlock *>(block_as(&header_block));

    EXPECT_EQ(header->total_blocks, img_size / DEFAULT_BLOCK_SIZE);
    EXPECT_GT(header->inode_count, 0u);
    EXPECT_GT(header->inode_block_count, 0u);
    EXPECT_GT(header->data_start, header->inode_block_start);

    img_file.close();
    clear_test_img_dir();
}

LC_FILESYSTEM_NAMESPACE_END
LC_NAMESPACE_END
