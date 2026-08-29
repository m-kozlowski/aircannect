#include "console_commands.h"

#include <ctype.h>
#include <string.h>

#include "app_config_registry.h"
#include "board_button.h"
#include "button_binding_config.h"
#include "config_service.h"
#include "local_input.h"
#include "management_console_format.h"
#include "management_console_utils.h"
#include "string_util.h"
#include "wifi_manager.h"

namespace aircannect {
namespace {

void print_binding(Print &out,
                   const ButtonBindingConfig &config,
                   ButtonInput input,
                   ButtonGesture gesture,
                   const ButtonBinding *defaults,
                   size_t default_count) {
    char id[48] = {};
    if (!board_button_input_id(input, id, sizeof(id))) return;

    const ButtonBinding *profile_default = button_binding_find(
        defaults, default_count, input, gesture);
    const LocalActionId default_action =
        profile_default ? profile_default->action : LocalActionId::None;

    const ButtonBinding *override =
        config.state == ButtonBindingBlobState::Valid
            ? button_binding_find(config.overrides.data(),
                                  config.overrides.size(), input, gesture)
            : nullptr;
    const LocalActionId effective =
        override ? override->action : default_action;
    const LocalActionDefinition *default_definition =
        local_action_find(default_action);
    const LocalActionDefinition *effective_definition =
        local_action_find(effective);

    out.print("  ");
    out.print(id);
    out.print(' ');
    out.print(button_gesture_name(gesture));
    out.print(" default=");
    out.print(default_definition ? default_definition->name : "unknown");
    out.print(" override=");
    if (override) {
        const LocalActionDefinition *override_definition =
            local_action_find(override->action);
        out.print(override_definition ? override_definition->name
                                      : "unknown");
    } else {
        out.print("--");
    }
    out.print(" effective=");
    out.println(effective_definition ? effective_definition->name
                                     : "unknown");
}

void print_keybindings(Print &out, const ButtonBindingConfig &config) {
    out.print("[CONFIG] keybindings state=");
    out.println(button_binding_blob_state_name(config.state));

    size_t count = 0;
    const BoardButtonDefinition *buttons = board_button_catalog(count);
    size_t default_count = 0;
    const ButtonBinding *defaults =
        board_button_binding_defaults(default_count);
    std::vector<ButtonBinding> resolved;
    if (!resolve_button_bindings(buttons, count, config, resolved,
                                 defaults, default_count)) {
        out.println("[CONFIG] keybinding catalog invalid");
        return;
    }

    for (const ButtonBinding &binding : resolved) {
        print_binding(out, config, binding.input, binding.gesture,
                      defaults, default_count);
    }
}

bool handle_keybindings(Print &out,
                        String rest,
                        ConfigService &config) {
    if (rest != "keybindings" && !rest.startsWith("keybindings ")) {
        return false;
    }

    rest.remove(0, strlen("keybindings"));
    trim_inplace(rest);
    if (!rest.length()) {
        print_keybindings(out, config.data().keybindings);
        return true;
    }

    if (rest == "reset") {
        ButtonBindingConfig next;
        button_binding_reset(next);
        const ConfigFieldUpdate update = config.set_keybindings(next);
        out.println(update.accepted()
                        ? "[CONFIG] keybindings reset"
                        : "[CONFIG] keybindings reset failed");
        return true;
    }

    int pos = 0;
    String button_id;
    String gesture_text;
    String action_text;
    if (!parse_console_arg(rest, pos, button_id) ||
        !parse_console_arg(rest, pos, gesture_text) ||
        !parse_console_arg(rest, pos, action_text)) {
        out.println("[CONFIG] usage: config keybindings "
                    "BUTTON[+BUTTON] GESTURE ACTION|default");
        return true;
    }

    ButtonInput input;
    ButtonGesture gesture = ButtonGesture::ShortPress;
    if (!board_button_input_find(button_id.c_str(), input) ||
        !parse_button_gesture(gesture_text.c_str(), gesture) ||
        !board_button_input_supported(input, gesture)) {
        out.println("[CONFIG] invalid button or gesture");
        return true;
    }

    ButtonBindingConfig next = config.data().keybindings;
    bool changed = false;
    if (action_text == "default") {
        if (next.state != ButtonBindingBlobState::Valid) {
            out.println("[CONFIG] reset invalid keybindings before editing");
            return true;
        }
        changed = button_binding_remove_override(next, input, gesture);
    } else {
        const LocalActionDefinition *action =
            local_action_find(action_text.c_str());
        if (!action || !button_binding_set_override(
                           next, input, gesture, action->id)) {
            out.println("[CONFIG] invalid local action");
            return true;
        }
        changed = true;
    }

    if (!changed || !config.set_keybindings(next).accepted()) {
        out.println("[CONFIG] failed to update keybindings");
        return true;
    }

    print_keybindings(out, config.data().keybindings);
    return true;
}

void print_config(Print &out, const AppConfigData &config) {
    out.println("[CONFIG]");

    size_t count = 0;
    const AppConfigFieldDescriptor *fields = app_config_fields(count);
    AppConfigGroup last_group = AppConfigGroup::Device;
    bool have_group = false;
    for (size_t i = 0; i < count; ++i) {
        const AppConfigFieldDescriptor &field = fields[i];
        if (!app_config_field_is_user_visible(field)) continue;
        if (!have_group || field.group != last_group) {
            out.print("  [");
            out.print(app_config_group_label(field.group));
            out.println("]");
            last_group = field.group;
            have_group = true;
        }

        String value;
        if (!app_config_field_get_console_value(config, field, value)) {
            continue;
        }
        out.print("  ");
        out.print(field.key);
        out.print(": ");
        out.println(value);
    }
}

bool print_config_value(Print &out,
                        const AppConfigData &config,
                        String key) {
    trim_inplace(key);
    if (!key.length() || key.indexOf(' ') >= 0) return false;

    const AppConfigFieldDescriptor *field =
        app_config_find_field(key.c_str());
    if (!field || !app_config_field_is_user_visible(*field)) return false;

    String value;
    if (!app_config_field_get_console_value(config, *field, value)) {
        return false;
    }

    out.print("[CONFIG] ");
    out.print(field->key);
    out.print("=");
    out.println(value);
    return true;
}

bool split_config_key_value(String rest,
                            String &key,
                            bool &has_value,
                            String &value) {
    key = "";
    value = "";
    has_value = false;

    int pos = 0;
    if (!parse_console_arg(rest, pos, key)) return false;
    trim_inplace(key);
    if (!key.length()) return false;

    while (pos < static_cast<int>(rest.length()) &&
           isspace(static_cast<unsigned char>(rest[pos]))) {
        ++pos;
    }
    if (pos >= static_cast<int>(rest.length())) return true;

    has_value = true;
    String tail = rest.substring(pos);
    trim_inplace(tail);
    if (!tail.length()) return true;

    if (tail[0] == '"' || tail[0] == '\'') {
        int tail_pos = 0;
        String parsed;
        if (parse_console_arg(tail, tail_pos, parsed)) {
            while (tail_pos < static_cast<int>(tail.length()) &&
                   isspace(static_cast<unsigned char>(tail[tail_pos]))) {
                ++tail_pos;
            }
            if (tail_pos >= static_cast<int>(tail.length())) {
                value = parsed;
                return true;
            }
        }
    }

    value = tail;
    return true;
}

bool handle_config_key(Print &out,
                       String rest,
                       ConfigService &config) {
    String key;
    String value;
    bool has_value = false;
    if (!split_config_key_value(rest, key, has_value, value)) return false;

    if (!has_value) return print_config_value(out, config.data(), key);

    const AppConfigFieldDescriptor *field =
        app_config_find_field(key.c_str());
    if (!field || !app_config_field_is_user_visible(*field)) return false;

    ConfigTransactionResult transaction;
    const ConfigFieldUpdate update = config.set_value(
        field->key, value, false, &transaction);
    if (!update.accepted()) {
        out.print("[CONFIG] invalid ");
        out.println(field->key);
        return true;
    }
    if (!transaction.persisted) {
        out.println("[CONFIG] warning: failed to persist value");
    }

    print_config_value(out, config.data(), key);
    return true;
}

void handle_config(Print &out,
                   String rest,
                   ConfigService &config,
                   WifiManager &wifi) {
    trim_inplace(rest);

    if (!rest.length() || rest == "show" || rest == "dump") {
        print_config(out, config.data());
        return;
    }

    if (rest == "factory-reset" || rest == "factory reset") {
        out.println(
            "[CONFIG] factory reset: clearing app config and Wi-Fi credentials");
        const ConfigTransactionResult transaction = config.reset();
        wifi.clear_sta_config();
        out.println(transaction.persisted
                        ? "[CONFIG] factory reset complete"
                        : "[CONFIG] factory reset persistence failed");
        ConsoleFormat::print_wifi_status(out, wifi);
        return;
    }

    if (rest == "reset") {
        out.println("[CONFIG] resetting app config to defaults");
        const ConfigTransactionResult transaction = config.reset();
        wifi.reconnect();
        out.println(transaction.persisted
                        ? "[CONFIG] reset complete"
                        : "[CONFIG] reset persistence failed");
        return;
    }

    if (handle_keybindings(out, rest, config)) return;

    if (handle_config_key(out, rest, config)) return;

    print_unknown_command(out, "CONFIG",
                          "config, config KEY [VALUE], config keybindings "
                          "[reset|BUTTON[+BUTTON] GESTURE "
                          "ACTION|default], reset, "
                          "factory-reset");
}

}  // namespace

ConfigConsoleCommands::ConfigConsoleCommands(ConfigService &config,
                                             WifiManager &wifi)
    : config_(config), wifi_(wifi) {}

bool ConfigConsoleCommands::execute(const String &command,
                                    const String &rest,
                                    Print &out,
                                    ConsoleCommandSession &session) {
    (void)session;
    if (command != "config") return false;

    handle_config(out, rest, config_, wifi_);
    return true;
}

}  // namespace aircannect
