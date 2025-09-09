#ifndef FS_JOURNAL_H
#define FS_JOURNAL_H

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include "fs_config.h"
#include "fs_journal_block.h"
#include "fs_memory.h"
#include "fs_types.h"
#include "fs_utils.h"

FS_NAMESPACE_BEGIN

enum class JounralSysError {
    Success = 0,
    OpenFailed,
    ReadFailed,
    WriteFailed,
    FsyncFailed,
    InvalidSuperBlock,
    InvalidJournalFile,
    UnsupportedFeature,
    NoSpace,
    Busy,
    InvalidArgument,
    CorruptedLog,
};

enum class JournalWriteFlags : uint32 {
    None     = 0,
    Sync     = 1u << 0,
    Metadata = 1u << 1,
    Data     = 1u << 2,
};

struct JournalWriteParams {
    const u8 *data;
    uint64    data_bytes;
    uint32    flags;
};

struct JournalHandle {
    uint32 reserved_credits;
    uint32 used_credits;
    uint32 used = 0;
    uint32 tx_seq;
    bool   active = false;
    // 可扩展：调用线程 id、开始时间等
};

struct JournalTxnInfo {
    uint32 tx_seq;     // 事务序号
    uint64 start_off;  // 在日志空间中的起始偏移（字节/块）
    uint64 end_off;    // …结束偏移
    bool   committed;  // 是否已写入 commit record
};

/*
 * buffer range:
 * [head ......... tail]
 * ^^^^^^^^^^^^^^^
 * valid
 *
 *[tail .......... head]
 * ^^^^^^^^^^^^^^^
 * allocatable
 *
 * credit: atomic uint64 -> block
 * tail: atomic uint64 -> block
 * head: atomic uint64 -> block
 * capacity: const uint64 -> block
 *
 * allocate:
 * credit -= space
 * tail advances space
 * write_start = (tail + space) % capacity
 *
 * reclaim (space: uint64):
 * head advances space
 * credit += space
 *
 */

// clang-format off
// JounralSysError journal_start(JournalHandle& h, uint32_t credits_blocks);
// JounralSysError journal_extend(JournalHandle& h, uint32_t more_blocks);
//
// // 每写一个日志“块”时：检查预算 -> 分配物理位置(推进head) -> 写入 -> h.used_blocks++
// JounralSysError journal_write_desc(JournalHandle& h, /*...*/); // h.n_desc_blocks++, h.used_blocks++
// JounralSysError journal_write_data(JournalHandle& h, /*...*/); // h.n_data_blocks++, h.used_blocks++
// JounralSysError journal_write_commit(JournalHandle& h, /*...*/);// h.n_commit_blocks++, h.used_blocks++
//
// // 结束时：把 (reserved_blocks - used_blocks) 退回到全局预算计数；
// // 物理空间不回退，等 checkpoint 推 tail 后覆盖。
// void journal_stop(JournalHandle& h);
// clang-format on
class JournalSys {
public:
    JournalSys() = delete;

    JournalSys(const string &journal_path, JournalSuper &super, u8 *uuid,
               JounralSysError &error) {
        jnl_fd = open(journal_path.c_str(), O_RDWR | O_CREAT, 0644);
        if (jnl_fd < 0) {
            jnl_fd = -1;
            error  = JounralSysError::OpenFailed;
            return;
        }
        // read super block
        journal_super = {};
        if (pread(jnl_fd, &journal_super, sizeof(JournalSuper), 0)) {
            error = JounralSysError::ReadFailed;
            close(jnl_fd);
            return;
        }
        if (journal_super.header.magic != to_be32(JOURNAL_SUPERBLOCK_MAGIC)) {
            error = JounralSysError::InvalidSuperBlock;
            close(jnl_fd);
            return;
        }
        if (fs_memcmp(journal_super.uuid, uuid, 16) != 0) {
            error = JounralSysError::InvalidJournalFile;
            close(jnl_fd);
            return;
        }
        fs_memcpy(&super, &journal_super, sizeof(JournalSuper));
        journal_area_bytes =
            journal_super.len_blocks * JOURNAL_DEFAULT_BLOCK_SIZE;
        is_journal_running.store(true, MemOrder::Release);
        // TODO: start recovery
        // TODO: start background thread
    }

    JournalSys(JournalSys &&)                 = delete;
    JournalSys &operator=(JournalSys &&)      = delete;
    JournalSys(const JournalSys &)            = delete;
    JournalSys &operator=(const JournalSys &) = delete;

    ~JournalSys() {
        if (is_journal_running.load(MemOrder::Acquire)) {
            is_journal_running.store(false, MemOrder::Release);
            // lase fsync
            if (kjournal.joinable()) {
                kjournal.join();
            }
        }
        if (jnl_fd >= 0) {
            close(jnl_fd);
        }
    }

    // 1) journal_start / journal_stop（可选：若实现并发 handle/credits -> how
    // many block should be writen）
    //    - 开启一次 journaling 操作并返回句柄（内部记录 credits
    //    以进行空间/提交管理）
    //    - 查询已有空间
    //      - 不足：回收空间
    //    - 分配已有空间 -> 这里是否要锁定空间?
    //    - 返回结果
    //    这一步只扣除总的credits，如果没用完在journal_stop阶段回退给总credits
    JounralSysError journal_start(JournalHandle &handle,
                                  uint64         credits_blocks) {
        uint64 old = valid_credits.load(MemOrder::Relaxed);
        while (true) {
            if (old < credits_blocks) {
                // 回收空间
            }
            int desried = old - credits_blocks;
            if (valid_credits.compare_exchange_weak(old,
                                                    desried,
                                                    MemOrder::AcqRel,
                                                    MemOrder::Relaxed)) {
                handle.reserved_credits = credits_blocks;
                return JounralSysError::Success;
            }
        }
    }

    //    - 结束本次 journaling，释放 credits；若当前事务所有 handle
    //    均结束，则可进入提交阶段
    JounralSysError journal_stop(JournalHandle &handle);

    // 2) 写一批元数据缓冲到日志（对应 jbd2_journal_write_metadata_buffer）
    //    - 若不实现 handle，可忽略 handle
    //    参数的使用，但接口保留（方便未来演进）
    JounralSysError journal_write_metadata_buffer(JournalHandle *opt_handle,
                                                  const JournalWriteParams &p);

    // 3) 可选：如果也记录数据块（不推荐，但给出独立入口）
    JounralSysError journal_write_data_buffer(JournalHandle *opt_handle,
                                              const JournalWriteParams &p);

    // 4) 强制触发提交（对应 jbd2_journal_force_commit）
    //    - 外部请求立即将当前可提交的事务刷入日志（写入 commit record）
    JounralSysError journal_force_commit();

    // 5) 提交启动（软触发，允许后台线程择机提交；对应 jbd2_journal_start_commit
    // 的语义）
    JounralSysError journal_start_commit();

    // 6) 检查点（将已提交事务中的数据真正应用到主文件系统区域，并推进 tail）
    //    - 对应“checkpoint”阶段，回收日志空间
    JounralSysError journal_checkpoint_once();

    // 7) 恢复（mount/replay）
    //    - 扫描日志，找到最新有效事务序列范围并重放
    JounralSysError journal_recover();

    // 8) 撤销（可选，对应 Revocation blocks）
    //    - 在同一事务中撤销某些块的重放
    JounralSysError journal_revoke_block(uint64 fs_block_number);

    // 9) 同步（fsync）- 针对日志文件本身
    JounralSysError journal_fsync();

    // 10) 查询与统计
    uint64 head_offset_bytes() const;        // 有效数据起点
    uint64 tail_offset_bytes() const;        // 有效数据终点（下一写入位置）
    uint32 current_tx_sequence() const;      // 当前构建中的事务序号
    uint32 last_committed_sequence() const;  // 最近一次提交完成的事务序号

private:

    uint64 estimate_bytes(uint64 credits);

    // 追加一个描述块 + payload 到日志（做切块/CRC/对齐/环区 wrap）
    JounralSysError append_descriptor_and_payload(
        const JournalWriteParams &p,
        /*out*/ uint64           &bytes_appended);

    // 追加 commit record（BlockCommitRecord），推进 last_committed_seq
    JounralSysError append_commit_record(uint32 tx_seq);

    // 追加 revocation block
    JounralSysError append_revocation_block();

    // 执行一次事务提交：封口 + fsync（由后台线程或强制提交触发）
    JounralSysError do_commit_one_transaction(/*in */ uint32          tx_seq,
                                              /*out*/ JournalTxnInfo &info);

    // 将已提交事务进行 checkpoint（把被记录的目标块写回主区，推进 head）
    JounralSysError do_checkpoint_transactions(/*budget*/ uint32 max_txn);

    // 日志空间管理：检查剩余空间，必要时等待 checkpoint 回收或切换事务
    bool ensure_log_space(uint32 blocks_needed);

    // 计算校验（可替换为 CRC32C）
    uint32 calc_block_csum(const u8 *buf, uint64 nbytes, const u8 uuid[16],
                           uint64 block_no) const;

    // 后台提交线程（类似 kjournald2）：周期性提交 & checkpoint
    void journal_sync_worker();

    // 恢复：定位可重放的起始序列与终止序列
    JounralSysError recover_scan(/*out*/ uint32 &first_seq,
                                 /*out*/ uint32 &last_seq);
    // 恢复：重放 [first_seq, last_seq]
    JounralSysError recover_replay(uint32 first_seq, uint32 last_seq);

    // IO
    JounralSysError pwrite_all(const void *buf, uint64 len, uint64 off);
    JounralSysError pread_all(void *buf, uint64 len, uint64 off);

private:
    static constexpr uint32 MAX_COMMIT_TIME_MS = 10;
    mutex                   mtx;
    condition_variable      cv_need_commit;
    condition_variable      cv_commit_done;

    atomic<uint64> head;
    atomic<uint64> tail;
    atomic<uint64> valid_credits;
    atomic<uint64> capacity;

    atomic<uint64> flushed_tail;

    atomic<uint32> sync_goal {0};

    atomic<uint32> cur_tx_seq {0};  // 正在构建的事务
    atomic<uint32> last_committed_seq {0};

    thread       kjournal;
    int32        jnl_fd = -1;
    atomic<bool> is_journal_running;
    JournalSuper journal_super;
    uint64       journal_area_bytes;

    shared_ptr<atomic<uint32>> latest_flushed_disk_seq;
};

FS_NAMESPACE_END

#endif  // FS_JOURNAL_H
