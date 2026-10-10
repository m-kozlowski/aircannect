#pragma once

#include "export_endpoint.h"
#include "export_step.h"

namespace aircannect {

class StorageSyncEngine;
class SleepHqSyncEngine;

// Non-owning adapter. Engines remain owned and stepped by ExportTask.
class ExportBackend {
public:
    explicit ExportBackend(StorageSyncEngine &engine);
    explicit ExportBackend(SleepHqSyncEngine &engine);

    // Called with the request already admitted by ExportTask.
    bool request(const ExportRequest &request) const {
        return ops_->request(engine_, request);
    }
    ExportStep step() const { return ops_->step(engine_); }
    ExportEndpointStatus status() const { return ops_->status(engine_); }
    bool release_media() const { return ops_->release_media(engine_); }

    void cancel(ExportReason reason) const { ops_->cancel(engine_, reason); }
    void set_network_available(bool value) const { ops_->network(engine_, value); }
    void set_runtime_blocked(bool value) const { ops_->blocked(engine_, value); }

private:
    struct Operations {
        bool (*request)(void *, const ExportRequest &);
        ExportStep (*step)(void *);
        ExportEndpointStatus (*status)(void *);
        bool (*release_media)(void *);
        void (*cancel)(void *, ExportReason);
        void (*network)(void *, bool);
        void (*blocked)(void *, bool);
    };

    template <typename Engine> static const Operations &operations();

    void *engine_;
    const Operations *ops_;
};

}  // namespace aircannect
