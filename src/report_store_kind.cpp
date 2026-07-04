#include "report_store.h"

namespace aircannect {
namespace ReportStore {

const char *kind_name(ReportStoreChunkKind kind) {
    switch (kind) {
        case ReportStoreChunkKind::Events: return "events";
        case ReportStoreChunkKind::Series:
        default: return "series";
    }
}

}  // namespace ReportStore
}  // namespace aircannect
