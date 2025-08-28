#ifndef FS_JOURNAL_H
#define FS_JOURNAL_H

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <new>

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
    InvalidSuperBlock,
    InvalidJournalFile,
    UnsupportedFeature,
};

struct JournalCommitParams {
    JournalBlockDescriptor &desc;
    uint8                  *data;
    bool                    sync;
};

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
        if (journal_super.header.magic != htobe32(JOURNAL_SUPERBLOCK_MAGIC)) {
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
        is_journal_running.store(true, MemOrder::memory_order_release);
    }

    JournalSys(JournalSys &&)                 = delete;
    JournalSys &operator=(JournalSys &&)      = delete;
    JournalSys(const JournalSys &)            = delete;
    JournalSys &operator=(const JournalSys &) = delete;

    ~JournalSys() {
        if (is_journal_running.load(MemOrder::memory_order_acquire)) {
            is_journal_running.store(false, MemOrder::memory_order_release);
            // lase fsync
            if (kjournal.joinable()) {
                kjournal.join();
            }
        }
        if (jnl_fd >= 0) {
            close(jnl_fd);
        }
    }

    JounralSysError commit(const JournalCommitParams &params) {
        if (jnl_fd < 0) {
            return JounralSysError::OpenFailed;
        }

        // TODO: recycle the useless area

        const uint64 header_bytes = sizeof(params.desc.header);
        const uint64 tag_bytes =
            params.desc.tags.size() * sizeof(JournalBlockTag);
        const uint64 data_bytes  = params.desc.tags.size() * DEFAULT_BLOCK_SIZE;
        const uint64 block_count = htobe64(params.desc.block_count);

        uint64 write_size =
            header_bytes + tag_bytes + data_bytes + sizeof(uint64);

        unique_ptr<uint8_t[]> write_buf(new (std::nothrow) uint8_t[write_size]);
        if (!write_buf) {
            return JounralSysError::WriteFailed;
        }

        uint8_t *p = write_buf.get();
        fs_memcpy(p, &params.desc.header, header_bytes);
        p += header_bytes;
        fs_memcpy(p, &block_count, sizeof(uint64));
        p += sizeof(uint64);
        if (tag_bytes) {
            fs_memcpy(p, params.desc.tags.data(), tag_bytes);
            p += tag_bytes;
        }
        if (data_bytes) {
            fs_memcpy(p, params.data, data_bytes);
        }

        uint64 base_off =
            tail.fetch_add(write_size, MemOrder::memory_order_acq_rel);
        uint64 written = 0;
        while (written < write_size) {
            ssize_t n = pwrite(jnl_fd,
                               write_buf.get() + written,
                               write_size - written,
                               static_cast<off_t>(base_off + written));
            if (n < 0) {
                return JounralSysError::WriteFailed;
            }
            written += static_cast<size_t>(n);
        }

        uint64 my_end = base_off + write_size;

        if (params.sync) {
            {
                lock_guard<mutex> lk(mtx);
                uint64            cur_goal =
                    sync_goal.load(MemOrder::memory_order_relaxed);
                if (my_end > cur_goal) {
                    sync_goal.store(my_end, MemOrder::memory_order_relaxed);
                }
            }
            cv.notify_all();

            unique_lock<mutex> lk(mtx);
            cv_done.wait(lk, [&] {
                return flushed_tail.load(MemOrder::memory_order_acquire) >=
                           my_end ||
                       !is_journal_running.load(MemOrder::memory_order_acquire);
            });
        }

        return JounralSysError::Success;
    }

private:

    void journal_sync_worker() {}

    // move header to the seq that flush to the disk
    void checkout_journal() {}

private:
    static constexpr uint32 MAX_COMMIT_TIME_MS = 10;
    mutex                   mtx;
    condition_variable      cv;
    condition_variable      cv_done;

    // ---|-------------------|----
    //    head  vaild         tail
    atomic<uint64> head;
    atomic<uint64> tail;
    atomic<uint64> flushed_tail;
    atomic<uint32> sync_goal;

    thread       kjournal;
    int32        jnl_fd = -1;
    atomic<bool> is_journal_running;
    JournalSuper journal_super;

    shared_ptr<atomic<uint32>> latest_flushed_disk_seq;
};

FS_NAMESPACE_END

#endif  // FS_JOURNAL_H
