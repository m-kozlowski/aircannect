#pragma once

#include "export_backend.h"

namespace aircannect {

class StorageSyncEngine;
class SleepHqSyncEngine;

extern const ExportBackendDefinition SMB_EXPORT_DEFINITION;
extern const ExportBackendDefinition SLEEPHQ_EXPORT_DEFINITION;

ExportBackend make_export_backend(StorageSyncEngine &engine);
ExportBackend make_export_backend(SleepHqSyncEngine &engine);

}  // namespace aircannect
