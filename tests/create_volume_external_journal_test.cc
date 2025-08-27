#include <gtest/gtest.h>

#include <filesystem>

#include "fs_crc32c.h"
#include "fs_image_creation.h"
#include "fs_uuid.h"
#include "test_config.h"

namespace fs = std::filesystem;
using namespace lc::fs;

class CreateVolumeExternalJournalTest : public ::testing::Test {
protected:
    void SetUp() override {
        init_test_img_dir();
    }

    void TearDown() override {
        clear_test_img_dir();
    }
};

TEST_F(CreateVolumeExternalJournalTest,
       CreateVolume_WithExternalJournal_BasicChecks) {
    const std::string img_path = get_test_img("vol_ext.img");
    const std::string jnl_path = get_test_img("journal_ext.jnl");

    VolumeParams p {};
    p.img_path         = img_path;
    p.img_size_bytes   = 64ull * 1024 * 1024;  // 64 MiB
    p.block_size_bytes = 4096;
    p.inode_size_bytes = 256;
    p.inode_ratio =
        16384;  // 每 16KiB 一个 inode（若你的实现优先 inode_count，请相应修改）
    p.inode_count = 0;  // 让实现根据 ratio 推导
    // Given the default 128MiB(2^27 bytes) block group size and 64-byte group
    // descriptors
    p.group_size_bytes = 4ull * 1024 * 1024;  // 4 MiB 组（需是块大小的整数倍）

    p.csum_type        = CsumType::CRC32C;
    p.magic            = 0xEF53;              // 如果你有自定义魔数，用你的
    p.reserved_inodes  = 11;
    p.first_data_block = 0;

    // —— External Journal 配置 ——
    p.journal_params.type                 = JournalType::External;
    p.journal_params.jnl_block_size_bytes = 4096;
    p.journal_params.jnl_path             = jnl_path;
    p.external_journal_size_bytes         = 8ull * 1024 * 1024;  // 8 MiB

    // 卷 UUID（可选；若实现内部生成也可不设）
    generate_uuid_v4(p.uuid);

    // —— 调用 ——
    CreateVolumeResult out {};
    ImgCreateStatue    st = create_volume(p, &out);

    // 如果你的成功枚举不是 Success，请替换这里
    EXPECT_EQ(st, ImgCreateStatue::Success);

    // —— 结果字段基本检查 ——
    EXPECT_EQ(out.jnl_type, JournalType::External);
    EXPECT_GT(out.blocks_total, 0u);
    EXPECT_GT(out.blocks_per_group, 0u);
    EXPECT_GT(out.groups, 0u);
    EXPECT_GT(out.inodes_total, 0u);
    EXPECT_GT(out.inodes_per_group, 0u);

    // 组大小应是块大小的整数倍
    EXPECT_EQ(p.group_size_bytes % p.block_size_bytes, 0u);

    // —— 卷镜像文件存在与大小对齐 ——
    ASSERT_TRUE(fs::exists(img_path)) << "volume image not created";
    const uint64_t img_actual = fs::file_size(img_path);
    EXPECT_GE(img_actual, p.img_size_bytes);
    EXPECT_EQ(img_actual % p.block_size_bytes, 0u);

    // blocks_total*block_size 不应大于镜像实际大小
    EXPECT_LE(out.blocks_total * static_cast<uint64_t>(p.block_size_bytes),
              img_actual);

    // —— 外部 journal 文件存在与大小对齐 ——
    ASSERT_TRUE(fs::exists(jnl_path)) << "external journal not created";
    const uint64_t jnl_actual = fs::file_size(jnl_path);
    const uint64_t jnl_expect_min =
        test_align_up(p.external_journal_size_bytes,
                      p.journal_params.jnl_block_size_bytes);
    EXPECT_GE(jnl_actual, jnl_expect_min);
    EXPECT_EQ(jnl_actual % p.journal_params.jnl_block_size_bytes, 0u);

    // out.jnl_blocks 应与外部 journal 实际块数一致（取实际文件大小 /
    // jnl_block_size_bytes）
    EXPECT_EQ(out.jnl_blocks,
              jnl_actual / p.journal_params.jnl_block_size_bytes);
}

TEST(UtilsSanity, CRC32C_KnownVector_And_UUIDv4Bits) {
    // CRC32C(Castagnoli) 的标准向量 "123456789" -> 0xE3069283
    const char *msg = "123456789";
    uint32_t    c   = lc::fs::crc32c(msg, 9);
    EXPECT_EQ(c, 0xE3069283u) << "CRC32C test vector mismatch";

    // UUIDv4 版本/变体位
    uint8 u[16] {};
    generate_uuid_v4(u);
    // 第 6 字节高 4 位应为 0100 (0x40)
    EXPECT_EQ((u[6] & 0xF0), 0x40);
    // 第 8 字节高 2 位应为 10xx xxxx (0x80)
    EXPECT_EQ((u[8] & 0xC0), 0x80);
}
