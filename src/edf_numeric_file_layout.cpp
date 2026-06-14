#include "edf_numeric_file_layout.h"

#include <string.h>

namespace aircannect {
namespace {

struct NumericSignalMap {
    EdfFileKind kind = EdfFileKind::Brp;
    const char *short_tag = "";
    uint8_t source_index = 0;
};

const NumericSignalMap EDF_NUMERIC_SIGNAL_MAP[] = {
    {EdfFileKind::Brp, "_RFL", 0},
    {EdfFileKind::Brp, "_MKP", 1},
    {EdfFileKind::Pld, "_MKF", 0},
    {EdfFileKind::Pld, "_MKI", 1},
    {EdfFileKind::Pld, "_MKE", 2},
    {EdfFileKind::Pld, "_LKF", 3},
    {EdfFileKind::Pld, "_RR2", 4},
    {EdfFileKind::Pld, "_TD2", 5},
    {EdfFileKind::Pld, "_MV2", 6},
    {EdfFileKind::Pld, "_TGT", 7},
    {EdfFileKind::Pld, "_IE2", 8},
    {EdfFileKind::Pld, "_SNI", 9},
    {EdfFileKind::Pld, "_FFL", 10},
    {EdfFileKind::Pld, "_INT", 11},
    {EdfFileKind::Sa2, "_HRT", 0},
    {EdfFileKind::Sa2, "_SAO", 1},
};

bool token_matches(const char *begin,
                   const char *end,
                   const char *needle,
                   size_t needle_len) {
    while (begin < end && (*begin == ' ' || *begin == '\t')) begin++;
    while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) end--;
    return static_cast<size_t>(end - begin) == needle_len &&
           strncmp(begin, needle, needle_len) == 0;
}

}  // namespace

void edf_reset_numeric_file_layout(EdfNumericFileLayout &layout) {
    EdfNumericFileLayout empty;
    layout = empty;
}

void edf_reset_numeric_file_layout_set(EdfNumericFileLayoutSet &layouts) {
    edf_reset_numeric_file_layout(layouts.brp);
    edf_reset_numeric_file_layout(layouts.pld);
    edf_reset_numeric_file_layout(layouts.sa2);
}

bool edf_accepted_data_ids_contain(const char *accepted_data_ids_csv,
                                   const char *data_id) {
    if (!accepted_data_ids_csv || !data_id || !data_id[0]) return false;

    const size_t data_id_len = strlen(data_id);
    const char *token = accepted_data_ids_csv;
    while (*token) {
        const char *end = token;
        while (*end && *end != ',') end++;
        if (token_matches(token, end, data_id, data_id_len)) return true;
        token = *end == ',' ? end + 1 : end;
    }
    return false;
}

bool edf_short_tag_is_accepted(const char *accepted_data_ids_csv,
                               const char *short_tag) {
    if (!short_tag || !short_tag[0]) return false;
    if (edf_accepted_data_ids_contain(accepted_data_ids_csv, short_tag)) {
        return true;
    }
    if (short_tag[0] == '_' && short_tag[1]) {
        return edf_accepted_data_ids_contain(accepted_data_ids_csv,
                                             short_tag + 1);
    }
    return false;
}

bool edf_build_numeric_file_layout(EdfFileKind kind,
                                   const char *accepted_data_ids_csv,
                                   EdfNumericFileLayout &layout) {
    edf_reset_numeric_file_layout(layout);

    const EdfFileSchema &base = edf_numeric_schema(kind);
    if (!base.signals || base.source_signal_count > AC_EDF_NUMERIC_SIGNAL_MAX) {
        return false;
    }

    size_t count = 0;
    for (size_t i = 0; i < sizeof(EDF_NUMERIC_SIGNAL_MAP) /
                               sizeof(EDF_NUMERIC_SIGNAL_MAP[0]);
         ++i) {
        const NumericSignalMap &entry = EDF_NUMERIC_SIGNAL_MAP[i];
        if (entry.kind != kind ||
            !edf_short_tag_is_accepted(accepted_data_ids_csv,
                                       entry.short_tag)) {
            continue;
        }
        if (entry.source_index >= base.source_signal_count ||
            count >= AC_EDF_NUMERIC_SIGNAL_MAX) {
            return false;
        }
        layout.signals[count] = base.signals[entry.source_index];
        layout.source_indices[count] = entry.source_index;
        count++;
    }

    if (count == 0) return true;

    layout.signals[count] = base.signals[base.source_signal_count];
    layout.schema = base;
    layout.schema.signals = layout.signals;
    layout.schema.source_signal_indices = layout.source_indices;
    layout.schema.signal_count = count + 1;
    layout.schema.source_signal_count = count;
    layout.enabled = true;
    return true;
}

bool edf_build_numeric_file_layout_set(const char *accepted_data_ids_csv,
                                       EdfNumericFileLayoutSet &layouts) {
    edf_reset_numeric_file_layout_set(layouts);
    return edf_build_numeric_file_layout(EdfFileKind::Brp,
                                         accepted_data_ids_csv,
                                         layouts.brp) &&
           edf_build_numeric_file_layout(EdfFileKind::Pld,
                                         accepted_data_ids_csv,
                                         layouts.pld) &&
           edf_build_numeric_file_layout(EdfFileKind::Sa2,
                                         accepted_data_ids_csv,
                                         layouts.sa2);
}

bool edf_numeric_file_layout_set_enabled(
    const EdfNumericFileLayoutSet &layouts) {
    return layouts.brp.enabled || layouts.pld.enabled || layouts.sa2.enabled;
}

}  // namespace aircannect
