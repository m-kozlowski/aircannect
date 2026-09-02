#pragma once

#include <memory>
#include <stddef.h>
#include <stdint.h>

#include "large_byte_buffer.h"
#include "operation_outcome.h"
#include "report_spool_port.h"
#include "sleep_day_id.h"
#include "storage_file_client.h"

namespace aircannect {

struct EdfStrSummaryRefreshEntry;

enum class EdfStrSummaryRefreshState : uint8_t {
    Idle,
    FetchingSummary,
    SubmittingRead,
    ReadingFile,
    CopyingFile,
    UpdatingRecords,
    SubmittingWrite,
    WritingFile,
    Complete,
    Failed,
};

struct EdfStrSummaryRefreshStatus {
    EdfStrSummaryRefreshState state = EdfStrSummaryRefreshState::Idle;
    uint32_t generation = 0;
    SleepDayId start_day;
    SleepDayId end_day;
    uint32_t matched = 0;
    uint32_t updated = 0;
    uint32_t unchanged = 0;
    uint32_t missing = 0;
    char error[64] = {};

    bool active() const {
        return state >= EdfStrSummaryRefreshState::FetchingSummary &&
               state <= EdfStrSummaryRefreshState::WritingFile;
    }
};

class EdfStrSummaryRefresh {
public:
    ~EdfStrSummaryRefresh();

    void begin(ReportSpoolPort &spool_port,
               StorageReadPort &read_port,
               StorageAtomicWritePort &write_port,
               StoragePathPort &path_port);

    OperationAdmission request(SleepDayId start_day,
                               SleepDayId end_day,
                               uint32_t generation);
    void abort(const char *error);
    void poll();

    const EdfStrSummaryRefreshStatus &status() const { return status_; }

private:
    bool build_summary_entries(const ReportSpoolResult &result);
    bool begin_file_copy(StoragePreparedFile file);
    bool prepare_file_update();
    void update_next_record();
    EdfStrSummaryRefreshEntry *find_summary(int32_t day);
    void finish_update();
    void finish();
    void fail(const char *error);
    void clear_work();

    ReportSpoolPort *spool_port_ = nullptr;
    StorageFileClient storage_file_;

    OperationTicket spool_ticket_;
    StoragePreparedFile source_file_;
    std::unique_ptr<LargeByteBuffer> file_buffer_;
    std::shared_ptr<const LargeByteBuffer> output_file_;

    EdfStrSummaryRefreshEntry *summary_entries_ = nullptr;
    size_t summary_capacity_ = 0;
    size_t summary_count_ = 0;

    size_t copy_offset_ = 0;
    int32_t header_start_day_ = -1;
    uint32_t record_count_ = 0;
    uint32_t record_index_ = 0;

    EdfStrSummaryRefreshStatus status_;
};

}  // namespace aircannect
