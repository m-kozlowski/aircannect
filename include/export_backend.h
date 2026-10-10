#pragma once

#include "export_endpoint.h"
#include "export_step.h"

namespace aircannect {

// Non-owning adapter. Engines remain owned and stepped by ExportTask.
class ExportBackend {
public:
    struct Operations {
        bool (*request)(void *, const ExportRequest &);
        ExportStep (*step)(void *);
        ExportEndpointStatus (*status)(void *);
        bool (*release_media)(void *);
        void (*cancel)(void *, ExportReason);
        void (*network)(void *, bool);
        void (*blocked)(void *, bool);
        void (*defer_idle)(void *, uint32_t);
    };

    ExportBackend(void *engine, const ExportBackendDefinition &definition,
                  const Operations &operations)
        : engine_(engine), definition_(&definition), ops_(&operations) {}

    const ExportBackendDefinition &definition() const { return *definition_; }

    // Called with the request already admitted by ExportTask.
    bool request(const ExportRequest &request) const {
        return ops_->request(engine_, request);
    }
    ExportStep step() const { return ops_->step(engine_); }
    ExportEndpointStatus status() const {
        auto status = ops_->status(engine_);
        status.definition = definition_;
        return status;
    }
    bool release_media() const { return ops_->release_media(engine_); }

    void cancel(ExportReason reason) const { ops_->cancel(engine_, reason); }
    void set_network_available(bool value) const { ops_->network(engine_, value); }
    void set_runtime_blocked(bool value) const { ops_->blocked(engine_, value); }
    void defer_idle_work_until(uint32_t value) const {
        if (ops_->defer_idle) ops_->defer_idle(engine_, value);
    }

private:
    void *engine_;
    const ExportBackendDefinition *definition_;
    const Operations *ops_;
};

// Borrowed, fixed-lifetime registration list, in scheduling order.
struct ExportBackendRegistry {
    const ExportBackend *backends;
    size_t count;

    const ExportBackend *begin() const { return backends; }
    const ExportBackend *end() const { return backends + count; }
    const ExportBackend &operator[](size_t index) const { return backends[index]; }
    const ExportBackend *find(ExportEndpoint id) const {
        for (const auto &backend : *this) {
            if (backend.definition().id == id) return &backend;
        }
        return nullptr;
    }
};

}  // namespace aircannect
