#include "local_input_controller.h"

#include <vector>

#include "board_button.h"
#include "button_binding_config.h"
#include "debug_log.h"

namespace aircannect {

bool LocalInputController::begin() {
    size_t count = 0;
    const BoardButtonDefinition *catalog = board_button_catalog(count);
    if (!buttons_.begin(catalog, count)) return false;

    std::vector<ButtonBinding> defaults;
    const ButtonBindingConfig no_overrides;
    size_t default_count = 0;
    const ButtonBinding *binding_defaults =
        board_button_binding_defaults(default_count);
    if (!resolve_button_bindings(catalog, count, no_overrides, defaults,
                                 binding_defaults, default_count)) {
        return false;
    }

    buttons_.set_event_handler(handle_button_event, this);
    apply_bindings(defaults.data(), defaults.size(), millis());
    return true;
}

void LocalInputController::poll(uint32_t now_ms) {
    buttons_.poll(now_ms);
}

bool LocalInputController::register_action(
    LocalActionId action,
    LocalActionRouter::Handler handler,
    void *context) {
    return actions_.register_handler(action, handler, context);
}

bool LocalInputController::apply_config(const ButtonBindingConfig &config,
                                        uint32_t now_ms) {
    size_t count = 0;
    const BoardButtonDefinition *catalog = board_button_catalog(count);
    size_t default_count = 0;
    const ButtonBinding *binding_defaults =
        board_button_binding_defaults(default_count);
    std::vector<ButtonBinding> resolved;
    if (!resolve_button_bindings(catalog, count, config, resolved,
                                 binding_defaults, default_count)) {
        return false;
    }

    apply_bindings(resolved.data(), resolved.size(), now_ms);
    return true;
}

void LocalInputController::apply_bindings(const ButtonBinding *bindings,
                                          size_t count,
                                          uint32_t now_ms) {
    std::vector<ButtonChordDefinition> chords;
    chords.reserve(count / 2);
    for (size_t i = 0; i < count; ++i) {
        if (!bindings[i].input.chord() ||
            bindings[i].action == LocalActionId::None ||
            !local_action_find(bindings[i].action)) {
            continue;
        }

        ButtonChordDefinition *chord = nullptr;
        for (ButtonChordDefinition &candidate : chords) {
            if (candidate.input == bindings[i].input) {
                chord = &candidate;
                break;
            }
        }
        if (!chord) {
            chords.push_back({bindings[i].input, BUTTON_GESTURE_NONE});
            chord = &chords.back();
        }

        chord->gestures |=
            bindings[i].gesture == ButtonGesture::ShortPress
                ? BUTTON_GESTURE_SHORT
                : BUTTON_GESTURE_LONG;
    }

    actions_.apply_bindings(bindings, count);
    buttons_.set_chords(chords.data(), chords.size(), now_ms);
}

void LocalInputController::handle_button_event(void *context,
                                               ButtonInput input,
                                               ButtonGesture gesture,
                                               uint32_t now_ms) {
    static_cast<LocalInputController *>(context)->dispatch(
        input, gesture, now_ms);
}

void LocalInputController::dispatch(ButtonInput input,
                                    ButtonGesture gesture,
                                    uint32_t now_ms) {
    const LocalActionId action =
        actions_.effective_action(input, gesture);
    const LocalActionDefinition *definition = local_action_find(action);
    const bool accepted = actions_.dispatch(input, gesture, now_ms);

    if (input.chord()) {
        Log::logf(CAT_GENERAL, accepted ? LOG_DEBUG : LOG_WARN,
                  "[INPUT] buttons=%u+%u gesture=%s action=%s %s\n",
                  static_cast<unsigned>(input.first_button_key),
                  static_cast<unsigned>(input.second_button_key),
                  button_gesture_name(gesture),
                  definition ? definition->name : "unknown",
                  accepted ? "accepted" : "rejected");
    } else {
        Log::logf(CAT_GENERAL, accepted ? LOG_DEBUG : LOG_WARN,
                  "[INPUT] button=%u gesture=%s action=%s %s\n",
                  static_cast<unsigned>(input.first_button_key),
                  button_gesture_name(gesture),
                  definition ? definition->name : "unknown",
                  accepted ? "accepted" : "rejected");
    }
}

}  // namespace aircannect
