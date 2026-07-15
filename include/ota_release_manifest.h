#pragma once

#include <stddef.h>

namespace aircannect {

static constexpr size_t AC_OTA_RELEASE_VERSION_MAX = 48;
static constexpr size_t AC_OTA_RELEASE_ERROR_MAX = 48;
static constexpr size_t AC_OTA_RELEASE_ARTIFACT_URL_MAX = 512;

struct OtaReleaseArtifact {
    char url[AC_OTA_RELEASE_ARTIFACT_URL_MAX] = {};
    size_t image_size = 0;
    size_t wire_size = 0;
    bool zlib = false;
};

struct OtaReleaseManifest {
    char version[AC_OTA_RELEASE_VERSION_MAX] = {};
    bool raw_available = false;
    bool zlib_available = false;
    OtaReleaseArtifact preferred_artifact;
};

bool ota_parse_release_manifest(const char *json,
                                size_t json_len,
                                const char *target,
                                OtaReleaseManifest &manifest,
                                char error[AC_OTA_RELEASE_ERROR_MAX]);

bool ota_release_is_newer(const char *current_version,
                          const char *release_version,
                          bool &is_newer);

bool ota_resolve_release_artifact_url(const char *manifest_url,
                                      const char *artifact_url,
                                      char *resolved_url,
                                      size_t resolved_capacity);

}  // namespace aircannect
