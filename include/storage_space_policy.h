#pragma once

#include <stdint.h>

namespace aircannect::StorageSpacePolicy {

constexpr uint64_t MiB = 1024 * 1024;

// Free space, not card size. Reclamation must leave the warning active.
constexpr uint64_t ReclaimBelow = 32 * MiB;
constexpr uint64_t WarnBelow = 64 * MiB;
constexpr uint64_t ReclaimTarget = WarnBelow;
constexpr uint64_t ClearWarningAt = 96 * MiB;
constexpr uint64_t CompressionReserve = 128 * MiB;

// Admission after a recording failure, not a limit on ongoing capture.
constexpr uint64_t EdfRecoveryReserve = 128 * 1024;

static_assert(ReclaimBelow < ReclaimTarget &&
              ReclaimTarget < ClearWarningAt &&
              ClearWarningAt < CompressionReserve);

}  // namespace aircannect::StorageSpacePolicy
