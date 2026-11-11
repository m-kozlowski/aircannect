// Read the production descriptor owners; do not duplicate their tables here.
#include "app_config_registry.h"
#include "as11_settings.h"
#include "local_input.h"

#include <ArduinoJson.h>
#include <iostream>
#include <string>

using namespace aircannect;

int main() {
    JsonDocument document;
    size_t count = 0;
    const auto *fields = app_config_all_fields(count);
    for (size_t i = 0; i < count; ++i) {
        const auto &field = fields[i];
        if (!app_config_field_is_user_visible(field)) continue;

        const std::string prefix = "config.field." + std::string(field.key);
        document[prefix + ".label"] = field.label;
        if (field.help && *field.help) document[prefix + ".help"] = field.help;
        document["config.section." + std::string(app_config_group_id(field.group))] =
            app_config_group_label(field.group);

        size_t choices = 0;
        const auto *options = app_config_field_allowed_values(field, choices);
        for (size_t j = 0; j < choices; ++j) {
            if (field.id == AppConfigFieldId::UiLanguage && j > 0) continue;
            document["config.option." + std::string(field.key) + "." +
                     options[j].value] = options[j].label;
        }
    }

    const auto *actions = local_action_catalog(count);
    for (size_t i = 0; i < count; ++i) {
        document["config.action." + std::string(actions[i].name)] =
            actions[i].label;
    }

    for (size_t i = 0; i < as11_setting_count(); ++i) {
        const auto &field = as11_setting(i);
        document["clinical.label." + std::string(field.key)] = field.label;
        document["clinical.group." + std::string(field.group)] = field.group;
        for (size_t j = 0; j < field.option_count; ++j) {
            document["clinical.option." + std::string(field.key) + "." +
                     std::to_string(j)] = field.options[j];
        }
    }

    for (size_t i = 0; i < as11_setting_composite_count(); ++i) {
        const auto &field = as11_setting_composite(i);
        document["clinical.label." + std::string(field.key)] = field.label;
        for (size_t j = 0; j < field.option_count; ++j) {
            document["clinical.option." + std::string(field.key) + "." +
                     std::to_string(j)] = field.options[j].label;
        }
    }
    serializeJson(document, std::cout);
    std::cout << '\n';
}
