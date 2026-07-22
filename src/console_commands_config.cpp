#include "console_commands.h"

#include <ctype.h>

#include "app_config_registry.h"
#include "config_service.h"
#include "management_console_format.h"
#include "management_console_utils.h"
#include "string_util.h"
#include "wifi_manager.h"

namespace aircannect {
namespace {

void print_config(Print &out, const AppConfigData &config) {
    out.println("[CONFIG]");

    size_t count = 0;
    const AppConfigFieldDescriptor *fields = app_config_fields(count);
    AppConfigGroup last_group = AppConfigGroup::Device;
    bool have_group = false;
    for (size_t i = 0; i < count; ++i) {
        const AppConfigFieldDescriptor &field = fields[i];
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
    if (!field) return false;

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
    if (!field) return false;

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

    if (handle_config_key(out, rest, config)) return;

    print_unknown_command(out, "CONFIG",
                          "config, config KEY [VALUE], reset, factory-reset");
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
