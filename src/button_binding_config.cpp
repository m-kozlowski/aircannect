#include "button_binding_config.h"

namespace aircannect {

namespace {

constexpr uint8_t BLOB_MAGIC_0 = 'B';
constexpr uint8_t BLOB_MAGIC_1 = 'K';
constexpr uint8_t BLOB_VERSION = 2;
constexpr uint8_t BLOB_RECORD_SIZE = 7;
constexpr uint8_t LEGACY_BLOB_VERSION = 1;
constexpr uint8_t LEGACY_BLOB_RECORD_SIZE = 5;
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

bool valid_input(ButtonInput input) {
    return input.first_button_key != 0 &&
           input.first_button_key != input.second_button_key;
}

}  // namespace

bool operator==(const ButtonBinding &left, const ButtonBinding &right) {
    return left.input == right.input &&
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
    const bool legacy = bytes[2] == LEGACY_BLOB_VERSION &&
                        bytes[3] == LEGACY_BLOB_RECORD_SIZE;
    const bool current = bytes[2] == BLOB_VERSION &&
                         bytes[3] == BLOB_RECORD_SIZE;
    if (bytes[2] != LEGACY_BLOB_VERSION && bytes[2] != BLOB_VERSION) {
        config.state = ButtonBindingBlobState::UnsupportedVersion;
        return false;
    }
    if (!legacy && !current) {
        config.state = ButtonBindingBlobState::Invalid;
        return false;
    }

    const uint16_t count = read_u16_le(bytes + 4);
    const uint8_t record_size = legacy ? LEGACY_BLOB_RECORD_SIZE
                                       : BLOB_RECORD_SIZE;
    const size_t expected = BLOB_HEADER_SIZE +
                            static_cast<size_t>(count) * record_size;
    if (expected != length) {
        config.state = ButtonBindingBlobState::Invalid;
        return false;
    }

    config.overrides.reserve(count);
    size_t offset = BLOB_HEADER_SIZE;
    for (uint16_t i = 0; i < count; ++i) {
        const uint16_t first_button_key = read_u16_le(bytes + offset);
        const uint16_t second_button_key =
            legacy ? 0 : read_u16_le(bytes + offset + 2);
        const uint8_t gesture = bytes[offset + (legacy ? 2 : 4)];
        const uint16_t action =
            read_u16_le(bytes + offset + (legacy ? 3 : 5));
        const ButtonInput input =
            button_input(first_button_key, second_button_key);
        if (!valid_input(input) || !valid_gesture_value(gesture)) {
            config.overrides.clear();
            config.state = ButtonBindingBlobState::Invalid;
            return false;
        }

        config.overrides.push_back(
            {input, static_cast<ButtonGesture>(gesture),
             static_cast<LocalActionId>(action)});
        offset += record_size;
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
        const ButtonInput input = button_input(
            binding.input.first_button_key,
            binding.input.second_button_key);
        if (!valid_input(input) ||
            !valid_gesture_value(static_cast<uint8_t>(binding.gesture))) {
            bytes.clear();
            return false;
        }

        append_u16_le(bytes, input.first_button_key);
        append_u16_le(bytes, input.second_button_key);
        bytes.push_back(static_cast<uint8_t>(binding.gesture));
        append_u16_le(bytes, static_cast<uint16_t>(binding.action));
    }
    return true;
}

bool button_binding_set_override(ButtonBindingConfig &config,
                                 ButtonInput input,
                                 ButtonGesture gesture,
                                 LocalActionId action) {
    input = button_input(input.first_button_key, input.second_button_key);
    if (config.state != ButtonBindingBlobState::Valid ||
        !valid_input(input)) {
        return false;
    }

    for (ButtonBinding &binding : config.overrides) {
        if (!(binding.input == input) ||
            binding.gesture != gesture) {
            continue;
        }
        binding.action = action;
        return true;
    }

    config.overrides.push_back({input, gesture, action});
    return true;
}

bool button_binding_remove_override(ButtonBindingConfig &config,
                                    ButtonInput input,
                                    ButtonGesture gesture) {
    if (config.state != ButtonBindingBlobState::Valid) return false;
    input = button_input(input.first_button_key, input.second_button_key);

    for (auto it = config.overrides.begin();
         it != config.overrides.end(); ++it) {
        if (it->input == input && it->gesture == gesture) {
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

    resolved.reserve(button_count * button_count + button_count);
    for (size_t i = 0; i < button_count; ++i) {
        const BoardButtonDefinition &button = buttons[i];
        if (board_button_supports_gesture(
                button, ButtonGesture::ShortPress)) {
            resolved.push_back({button_input(button.key),
                                ButtonGesture::ShortPress,
                                button.short_action});
        }
        if (board_button_supports_gesture(
                button, ButtonGesture::LongPress)) {
            resolved.push_back({button_input(button.key),
                                ButtonGesture::LongPress,
                                button.long_action});
        }

        for (size_t other = i + 1; other < button_count; ++other) {
            const ButtonInput input =
                button_input(button.key, buttons[other].key);
            resolved.push_back({input, ButtonGesture::ShortPress,
                                LocalActionId::None});
            resolved.push_back({input, ButtonGesture::LongPress,
                                LocalActionId::None});
        }
    }

    if (config.state != ButtonBindingBlobState::Valid) return true;
    for (const ButtonBinding &override : config.overrides) {
        for (ButtonBinding &binding : resolved) {
            if (binding.input == override.input &&
                binding.gesture == override.gesture) {
                binding.action = override.action;
                break;
            }
        }
    }
    return true;
}

}  // namespace aircannect
