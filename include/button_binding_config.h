#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "local_input.h"

namespace aircannect {

enum class ButtonBindingBlobState : uint8_t {
    Valid,
    Invalid,
    UnsupportedVersion,
};

struct ButtonBindingConfig {
    ButtonBindingBlobState state = ButtonBindingBlobState::Valid;
    std::vector<ButtonBinding> overrides;
    std::vector<uint8_t> preserved_blob;
};

bool operator==(const ButtonBinding &left, const ButtonBinding &right);
bool operator==(const ButtonBindingConfig &left,
                const ButtonBindingConfig &right);
inline bool operator!=(const ButtonBindingConfig &left,
                       const ButtonBindingConfig &right) {
    return !(left == right);
}

const char *button_binding_blob_state_name(ButtonBindingBlobState state);
bool button_binding_config_decode(const uint8_t *bytes,
                                  size_t length,
                                  ButtonBindingConfig &config);
bool button_binding_config_encode(const ButtonBindingConfig &config,
                                  std::vector<uint8_t> &bytes);

bool button_binding_set_override(ButtonBindingConfig &config,
                                 uint16_t button_key,
                                 ButtonGesture gesture,
                                 LocalActionId action);
bool button_binding_remove_override(ButtonBindingConfig &config,
                                    uint16_t button_key,
                                    ButtonGesture gesture);
void button_binding_reset(ButtonBindingConfig &config);

bool resolve_button_bindings(const BoardButtonDefinition *buttons,
                             size_t button_count,
                             const ButtonBindingConfig &config,
                             std::vector<ButtonBinding> &resolved);

}  // namespace aircannect
