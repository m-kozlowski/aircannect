#include "ota_release_manifest.h"

#include <ArduinoJson.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#if defined(ARDUINO_ARCH_ESP32)
#include <esp_heap_caps.h>
#endif

namespace aircannect {
namespace {

static constexpr uint32_t kManifestSchema = 1;
static constexpr char kManifestProduct[] = "aircannect";

#if defined(ARDUINO_ARCH_ESP32)
class ManifestJsonAllocator : public ArduinoJson::Allocator {
public:
    void *allocate(size_t size) override {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    void deallocate(void *ptr) override {
        heap_caps_free(ptr);
    }

    void *reallocate(void *ptr, size_t new_size) override {
        return heap_caps_realloc(ptr, new_size,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
};
#endif

struct ParsedVersion {
    uint32_t major = 0;
    uint32_t minor = 0;
    uint32_t patch = 0;
    const char *prerelease = nullptr;
    size_t prerelease_len = 0;
};

void set_error(char error[AC_OTA_RELEASE_ERROR_MAX], const char *value) {
    snprintf(error, AC_OTA_RELEASE_ERROR_MAX, "%s",
             value ? value : "manifest_invalid");
}

bool parse_number(const char *&cursor, const char *end, uint32_t &value) {
    if (cursor >= end || !isdigit(static_cast<unsigned char>(*cursor))) {
        return false;
    }

    uint64_t parsed = 0;
    while (cursor < end && isdigit(static_cast<unsigned char>(*cursor))) {
        parsed = parsed * 10u + static_cast<unsigned>(*cursor - '0');
        if (parsed > UINT32_MAX) return false;
        ++cursor;
    }

    value = static_cast<uint32_t>(parsed);
    return true;
}

bool valid_prerelease(const char *value, size_t len) {
    if (!value || len == 0) return false;

    bool identifier_has_char = false;
    for (size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c == '.') {
            if (!identifier_has_char) return false;
            identifier_has_char = false;
            continue;
        }
        if (!isalnum(c) && c != '-') return false;
        identifier_has_char = true;
    }
    return identifier_has_char;
}

bool parse_semver_span(const char *value, size_t len, ParsedVersion &out) {
    out = {};
    if (!value || len == 0) return false;

    const char *cursor = value;
    const char *end = value + len;
    if (*cursor == 'v' || *cursor == 'V') ++cursor;

    if (!parse_number(cursor, end, out.major) || cursor >= end ||
        *cursor++ != '.' || !parse_number(cursor, end, out.minor) ||
        cursor >= end || *cursor++ != '.' ||
        !parse_number(cursor, end, out.patch)) {
        return false;
    }

    if (cursor == end) return true;
    if (*cursor == '+') {
        ++cursor;
        return cursor < end;
    }
    if (*cursor != '-') return false;

    ++cursor;
    const char *prerelease = cursor;
    while (cursor < end && *cursor != '+') ++cursor;
    const size_t prerelease_len = static_cast<size_t>(cursor - prerelease);
    if (!valid_prerelease(prerelease, prerelease_len)) return false;
    if (cursor < end && (++cursor == end)) return false;

    out.prerelease = prerelease;
    out.prerelease_len = prerelease_len;
    return true;
}

bool git_describe_suffix(const char *value, size_t len, size_t &base_len) {
    if (!value || len == 0) return false;

    for (size_t i = len; i > 0; --i) {
        if (value[i - 1] != '-') continue;

        const size_t count_start = i;
        size_t cursor = count_start;
        while (cursor < len &&
               isdigit(static_cast<unsigned char>(value[cursor]))) {
            ++cursor;
        }
        if (cursor == count_start || cursor + 2 >= len ||
            value[cursor] != '-' || value[cursor + 1] != 'g') {
            continue;
        }

        cursor += 2;
        const size_t hash_start = cursor;
        while (cursor < len &&
               isxdigit(static_cast<unsigned char>(value[cursor]))) {
            ++cursor;
        }
        if (cursor == hash_start || cursor != len) continue;

        base_len = i - 1;
        return base_len > 0;
    }
    return false;
}

bool parse_current_version(const char *value,
                           ParsedVersion &out,
                           bool &has_local_commits) {
    has_local_commits = false;
    if (!value) return false;

    size_t len = strlen(value);
    static constexpr char kDirtySuffix[] = "-dirty";
    const size_t dirty_len = sizeof(kDirtySuffix) - 1;
    if (len > dirty_len &&
        memcmp(value + len - dirty_len, kDirtySuffix, dirty_len) == 0) {
        len -= dirty_len;
        has_local_commits = true;
    }

    size_t base_len = 0;
    if (git_describe_suffix(value, len, base_len)) {
        len = base_len;
        has_local_commits = true;
    }
    return parse_semver_span(value, len, out);
}

int compare_identifier(const char *left,
                       size_t left_len,
                       const char *right,
                       size_t right_len) {
    bool left_numeric = left_len > 0;
    bool right_numeric = right_len > 0;
    for (size_t i = 0; i < left_len; ++i) {
        left_numeric = left_numeric &&
            isdigit(static_cast<unsigned char>(left[i]));
    }
    for (size_t i = 0; i < right_len; ++i) {
        right_numeric = right_numeric &&
            isdigit(static_cast<unsigned char>(right[i]));
    }

    if (left_numeric != right_numeric) return left_numeric ? -1 : 1;
    if (left_numeric) {
        while (left_len > 1 && *left == '0') {
            ++left;
            --left_len;
        }
        while (right_len > 1 && *right == '0') {
            ++right;
            --right_len;
        }
        if (left_len != right_len) return left_len < right_len ? -1 : 1;
    }

    const size_t common = left_len < right_len ? left_len : right_len;
    const int compared = memcmp(left, right, common);
    if (compared != 0) return compared < 0 ? -1 : 1;
    if (left_len == right_len) return 0;
    return left_len < right_len ? -1 : 1;
}

int compare_prerelease(const ParsedVersion &left,
                       const ParsedVersion &right) {
    if (!left.prerelease && !right.prerelease) return 0;
    if (!left.prerelease) return 1;
    if (!right.prerelease) return -1;

    size_t left_offset = 0;
    size_t right_offset = 0;
    while (left_offset < left.prerelease_len &&
           right_offset < right.prerelease_len) {
        const char *left_value = left.prerelease + left_offset;
        const char *right_value = right.prerelease + right_offset;
        const char *left_dot = static_cast<const char *>(
            memchr(left_value, '.', left.prerelease_len - left_offset));
        const char *right_dot = static_cast<const char *>(
            memchr(right_value, '.', right.prerelease_len - right_offset));
        const size_t left_len = left_dot
            ? static_cast<size_t>(left_dot - left_value)
            : left.prerelease_len - left_offset;
        const size_t right_len = right_dot
            ? static_cast<size_t>(right_dot - right_value)
            : right.prerelease_len - right_offset;

        const int compared = compare_identifier(
            left_value, left_len, right_value, right_len);
        if (compared != 0) return compared;

        left_offset += left_len + (left_dot ? 1 : 0);
        right_offset += right_len + (right_dot ? 1 : 0);
    }

    if (left_offset >= left.prerelease_len &&
        right_offset >= right.prerelease_len) {
        return 0;
    }
    return left_offset >= left.prerelease_len ? -1 : 1;
}

int compare_versions(const ParsedVersion &left,
                     const ParsedVersion &right) {
    if (left.major != right.major) return left.major < right.major ? -1 : 1;
    if (left.minor != right.minor) return left.minor < right.minor ? -1 : 1;
    if (left.patch != right.patch) return left.patch < right.patch ? -1 : 1;
    return compare_prerelease(left, right);
}

bool valid_sha256(const char *value) {
    if (!value || strlen(value) != 64) return false;
    for (size_t i = 0; i < 64; ++i) {
        if (!isxdigit(static_cast<unsigned char>(value[i]))) return false;
    }
    return true;
}

bool parse_artifact(JsonObjectConst artifact,
                    bool zlib,
                    OtaReleaseArtifact &parsed) {
    parsed = {};

    const char *url = artifact["url"].as<const char *>();
    const char *sha256 = artifact["sha256"].as<const char *>();
    const uint64_t wire_size = artifact["size"].as<uint64_t>();
    if (!url || !*url ||
        strlen(url) >= sizeof(parsed.url) ||
        !artifact["size"].is<uint64_t>() ||
        wire_size == 0 || wire_size > SIZE_MAX ||
        !valid_sha256(sha256)) {
        return false;
    }

    uint64_t image_size = wire_size;
    if (zlib) {
        const char *decoded_sha256 =
            artifact["decoded_sha256"].as<const char *>();
        image_size = artifact["decoded_size"].as<uint64_t>();
        if (!artifact["decoded_size"].is<uint64_t>() ||
            image_size == 0 || image_size > SIZE_MAX ||
            !valid_sha256(decoded_sha256)) {
            return false;
        }
    }

    snprintf(parsed.url, sizeof(parsed.url), "%s", url);
    parsed.image_size = static_cast<size_t>(image_size);
    parsed.wire_size = static_cast<size_t>(wire_size);
    parsed.zlib = zlib;
    return true;
}

bool valid_url_text(const char *url) {
    if (!url || !*url) return false;
    for (const unsigned char *cursor =
             reinterpret_cast<const unsigned char *>(url);
         *cursor; ++cursor) {
        if (*cursor <= 0x20 || *cursor == 0x7f) return false;
    }
    return true;
}

bool absolute_http_url(const char *url) {
    if (!url) return false;
    return strncasecmp(url, "http://", 7) == 0 ||
           strncasecmp(url, "https://", 8) == 0;
}

bool copy_url_parts(char *output,
                    size_t capacity,
                    const char *prefix,
                    size_t prefix_len,
                    const char *suffix) {
    const size_t suffix_len = strlen(suffix);
    if (!output || capacity == 0 ||
        prefix_len >= capacity || suffix_len >= capacity - prefix_len) {
        return false;
    }

    memcpy(output, prefix, prefix_len);
    memcpy(output + prefix_len, suffix, suffix_len + 1);
    return true;
}

}  // namespace

bool ota_parse_release_manifest(const char *json,
                                size_t json_len,
                                const char *target,
                                OtaReleaseManifest &manifest,
                                char error[AC_OTA_RELEASE_ERROR_MAX]) {
    manifest = {};
    if (error) error[0] = '\0';
    if (!json || json_len == 0 || !target || !*target || !error) return false;

#if defined(ARDUINO_ARCH_ESP32)
    ManifestJsonAllocator allocator;
    JsonDocument document(&allocator);
#else
    JsonDocument document;
#endif
    const DeserializationError parse_error =
        deserializeJson(document, json, json_len);
    if (parse_error) {
        set_error(error, parse_error == DeserializationError::NoMemory
                             ? "manifest_alloc_failed"
                             : "manifest_json_invalid");
        return false;
    }

    JsonObjectConst root = document.as<JsonObjectConst>();
    if (root.isNull() || root["schema"] != kManifestSchema) {
        set_error(error, "manifest_schema_unsupported");
        return false;
    }

    const char *product = root["product"].as<const char *>();
    if (!product || strcmp(product, kManifestProduct) != 0) {
        set_error(error, "manifest_product_mismatch");
        return false;
    }

    const char *version = root["version"].as<const char *>();
    ParsedVersion parsed_version;
    if (!version || strlen(version) >= sizeof(manifest.version) ||
        !parse_semver_span(version, strlen(version), parsed_version)) {
        set_error(error, "manifest_version_invalid");
        return false;
    }

    JsonObjectConst targets = root["targets"].as<JsonObjectConst>();
    JsonObjectConst target_object = targets[target].as<JsonObjectConst>();
    if (target_object.isNull()) {
        set_error(error, "manifest_target_missing");
        return false;
    }

    const JsonObjectConst raw = target_object["raw"].as<JsonObjectConst>();
    const JsonObjectConst zlib = target_object["zlib"].as<JsonObjectConst>();
    OtaReleaseArtifact raw_artifact;
    OtaReleaseArtifact zlib_artifact;
    manifest.raw_available =
        !raw.isNull() && parse_artifact(raw, false, raw_artifact);
    manifest.zlib_available =
        !zlib.isNull() && parse_artifact(zlib, true, zlib_artifact);
    if (!manifest.raw_available && !manifest.zlib_available) {
        set_error(error, "manifest_artifact_invalid");
        return false;
    }

    snprintf(manifest.version, sizeof(manifest.version), "%s", version);
    manifest.preferred_artifact = manifest.zlib_available
        ? zlib_artifact
        : raw_artifact;
    return true;
}

bool ota_release_is_newer(const char *current_version,
                          const char *release_version,
                          bool &is_newer) {
    is_newer = false;

    ParsedVersion current;
    bool current_has_local_commits = false;
    if (!parse_current_version(current_version, current,
                               current_has_local_commits)) {
        return false;
    }

    ParsedVersion release;
    if (!release_version ||
        !parse_semver_span(release_version, strlen(release_version), release)) {
        return false;
    }

    const int compared = compare_versions(release, current);
    is_newer = compared > 0;
    if (current_has_local_commits && compared == 0) is_newer = false;
    return true;
}

bool ota_resolve_release_artifact_url(const char *manifest_url,
                                      const char *artifact_url,
                                      char *resolved_url,
                                      size_t resolved_capacity) {
    if (!valid_url_text(manifest_url) || !valid_url_text(artifact_url) ||
        !resolved_url || resolved_capacity == 0 ||
        !absolute_http_url(manifest_url)) {
        return false;
    }

    if (absolute_http_url(artifact_url)) {
        return copy_url_parts(resolved_url, resolved_capacity,
                              artifact_url, strlen(artifact_url), "");
    }
    if (artifact_url[0] == '/' && artifact_url[1] == '/') return false;

    const char *scheme = strstr(manifest_url, "://");
    const char *authority = scheme ? scheme + 3 : nullptr;
    if (!authority || !*authority) return false;

    const char *end = manifest_url + strlen(manifest_url);
    const char *query = strpbrk(authority, "?#");
    if (query) end = query;

    const char *path = static_cast<const char *>(
        memchr(authority, '/', static_cast<size_t>(end - authority)));
    if (artifact_url[0] == '/') {
        const size_t origin_len = path
            ? static_cast<size_t>(path - manifest_url)
            : static_cast<size_t>(end - manifest_url);
        return copy_url_parts(resolved_url, resolved_capacity,
                              manifest_url, origin_len, artifact_url);
    }

    const char *directory_end = path;
    for (const char *cursor = path; cursor && cursor < end; ++cursor) {
        if (*cursor == '/') directory_end = cursor + 1;
    }
    if (!directory_end) return false;

    const size_t directory_len =
        static_cast<size_t>(directory_end - manifest_url);
    return copy_url_parts(resolved_url, resolved_capacity,
                          manifest_url, directory_len, artifact_url);
}

}  // namespace aircannect
