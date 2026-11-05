#ifndef slic3r_PresetBulkEdit_hpp_
#define slic3r_PresetBulkEdit_hpp_

#include <string>

#include "PrintConfig.hpp"

namespace Slic3r { namespace PresetBulkEdit {

// Sets one option from what the user typed. "+5" / "-5" change a number by that amount (percent
// points for percent options), "+10%" / "-10%" scale it, "=-5" sets a negative value, anything else is
// the new value. Vectors (one value per extruder or nozzle) get every element changed.
// Returns an error message, empty on success.
std::string apply(ConfigOption &opt, const std::string &expr);

// Keys whose values differ, without the keys that only name or bind a preset (inherits, *_settings_id).
t_config_option_keys settings_diff(const DynamicPrintConfig &a, const DynamicPrintConfig &b);

}} // namespace Slic3r::PresetBulkEdit

#endif
