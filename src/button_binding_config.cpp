#include "button_binding_config.h"

namespace aircannect {

namespace {

constexpr uint8_t BLOB_MAGIC_0 = 'B';
constexpr uint8_t BLOB_MAGIC_1 = 'K';
constexpr uint8_t BLOB_VERSION = 1;
constexpr uint8_t BLOB_RECORD_SIZE = 5;
constexpr size_t BLOB_HEADER_SIZE = 6;

uint16_t read_u16_le(const uint8_t *bytes) {
    return static_cast<uint16_t>(bytes[0]) |
           static_cast<uint16_t>(bytes[1] << 8);
}

void append_u16_le(std::vector<uint8_t> &bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xff));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
}

bool valid_gesture_value(uint8_t value) {
    return value == static_cast<uint8_t>(ButtonGesture::ShortPress) ||
           value == static_cast<uint8_t>(ButtonGesture::LongPress);
}

}  // namespace

bool operator==(const ButtonBinding &left, const ButtonBinding &right) {
    return left.button_key == right.button_key &&
           left.gesture == right.gesture && left.action == right.action;
}

bool operator==(const ButtonBindingConfig &left,
                const ButtonBindingConfig &right) {
    return left.state == right.state &&
           left.overrides == right.overrides &&
           left.preserved_blob == right.preserved_blob;
}

const char *button_binding_blob_state_name(ButtonBindingBlobState state) {
    switch (state) {
        case ButtonBindingBlobState::Valid:
            return "valid";
        case ButtonBindingBlobState::Invalid:
            return "invalid";
        case ButtonBindingBlobState::UnsupportedVersion:
            return "unsupported_version";
    }
    return "invalid";
}

bool button_binding_config_decode(const uint8_t *bytes,
                                  size_t length,
                                  ButtonBindingConfig &config) {
    config = {};
    if (!bytes || length == 0) return true;

    config.preserved_blob.assign(bytes, bytes + length);
    if (length < BLOB_HEADER_SIZE || bytes[0] != BLOB_MAGIC_0 ||
        bytes[1] != BLOB_MAGIC_1) {
        config.state = ButtonBindingBlobState::Invalid;
        return false;
    }
    if (bytes[2] != BLOB_VERSION) {
        config.state = ButtonBindingBlobState::UnsupportedVersion;
        return false;
    }
    if (bytes[3] != BLOB_RECORD_SIZE) {
        config.state = ButtonBindingBlobState::Invalid;
        return false;
    }

    const uint16_t count = read_u16_le(bytes + 4);
    const size_t expected = BLOB_HEADER_SIZE +
                            static_cast<size_t>(count) * BLOB_RECORD_SIZE;
    if (expected != length) {
        config.state = ButtonBindingBlobState::Invalid;
        return false;
    }

    config.overrides.reserve(count);
    size_t offset = BLOB_HEADER_SIZE;
    for (uint16_t i = 0; i < count; ++i) {
        const uint16_t button_key = read_u16_le(bytes + offset);
        const uint8_t gesture = bytes[offset + 2];
        const uint16_t action = read_u16_le(bytes + offset + 3);
        if (button_key == 0 || !valid_gesture_value(gesture)) {
            config.overrides.clear();
            config.state = ButtonBindingBlobState::Invalid;
            return false;
        }

        config.overrides.push_back(
            {button_key, static_cast<ButtonGesture>(gesture),
             static_cast<LocalActionId>(action)});
        offset += BLOB_RECORD_SIZE;
    }

    config.state = ButtonBindingBlobState::Valid;
    config.preserved_blob.clear();
    return true;
}

bool button_binding_config_encode(const ButtonBindingConfig &config,
                                  std::vector<uint8_t> &bytes) {
    bytes.clear();
    if (config.state != ButtonBindingBlobState::Valid) {
        bytes = config.preserved_blob;
        return !bytes.empty();
    }
    if (config.overrides.size() > UINT16_MAX) return false;

    bytes.reserve(BLOB_HEADER_SIZE +
                  config.overrides.size() * BLOB_RECORD_SIZE);
    bytes.push_back(BLOB_MAGIC_0);
    bytes.push_back(BLOB_MAGIC_1);
    bytes.push_back(BLOB_VERSION);
    bytes.push_back(BLOB_RECORD_SIZE);
    append_u16_le(bytes,
                  static_cast<uint16_t>(config.overrides.size()));

    for (const ButtonBinding &binding : config.overrides) {
        if (binding.button_key == 0 ||
            !valid_gesture_value(static_cast<uint8_t>(binding.gesture))) {
            bytes.clear();
            return false;
        }

        append_u16_le(bytes, binding.button_key);
        bytes.push_back(static_cast<uint8_t>(binding.gesture));
        append_u16_le(bytes, static_cast<uint16_t>(binding.action));
    }
    return true;
}

bool button_binding_set_override(ButtonBindingConfig &config,
                                 uint16_t button_key,
                                 ButtonGesture gesture,
                                 LocalActionId action) {
    if (config.state != ButtonBindingBlobState::Valid || button_key == 0) {
        return false;
    }

    for (ButtonBinding &binding : config.overrides) {
        if (binding.button_key != button_key ||
            binding.gesture != gesture) {
            continue;
        }
        binding.action = action;
        return true;
    }

    config.overrides.push_back({button_key, gesture, action});
    return true;
}

bool button_binding_remove_override(ButtonBindingConfig &config,
                                    uint16_t button_key,
                                    ButtonGesture gesture) {
    if (config.state != ButtonBindingBlobState::Valid) return false;

    for (auto it = config.overrides.begin();
         it != config.overrides.end(); ++it) {
        if (it->button_key == button_key && it->gesture == gesture) {
            config.overrides.erase(it);
            break;
        }
    }
    return true;
}

void button_binding_reset(ButtonBindingConfig &config) {
    config = {};
}

bool resolve_button_bindings(const BoardButtonDefinition *buttons,
                             size_t button_count,
                             const ButtonBindingConfig &config,
                             std::vector<ButtonBinding> &resolved) {
    resolved.clear();
    if (!validate_button_catalog(buttons, button_count)) return false;

    resolved.reserve(button_count * 2);
    for (size_t i = 0; i < button_count; ++i) {
        const BoardButtonDefinition &button = buttons[i];
        if (board_button_supports_gesture(
                button, ButtonGesture::ShortPress)) {
            resolved.push_back({button.key, ButtonGesture::ShortPress,
                                button.short_action});
        }
        if (board_button_supports_gesture(
                button, ButtonGesture::LongPress)) {
            resolved.push_back({button.key, ButtonGesture::LongPress,
                                button.long_action});
        }
    }

    if (config.state != ButtonBindingBlobState::Valid) return true;
    for (const ButtonBinding &override : config.overrides) {
        for (ButtonBinding &binding : resolved) {
            if (binding.button_key == override.button_key &&
                binding.gesture == override.gesture) {
                binding.action = override.action;
                break;
            }
        }
    }
    return true;
}

}  // namespace aircannect
