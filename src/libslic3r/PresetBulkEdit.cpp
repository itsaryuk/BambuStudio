#include "PresetBulkEdit.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

#include <boost/algorithm/string.hpp>

namespace Slic3r { namespace PresetBulkEdit {

std::string apply(ConfigOption &opt, const std::string &input)
{
    std::string expr = boost::algorithm::trim_copy(input);
    if (expr.empty()) return "Enter a value.";

    if (expr[0] == '+' || expr[0] == '-') {
        const bool scale = expr.back() == '%';
        double     amount;
        try {
            size_t used = 0;
            amount      = std::stod(expr.substr(0, expr.size() - (scale ? 1 : 0)), &used);
            if (used != expr.size() - (scale ? 1 : 0)) throw std::invalid_argument(expr);
        } catch (...) {
            return "\"" + expr + "\" is not a number.";
        }
        // Rounded so 0.2 * 1.1 is stored as 0.22, not 0.22000000000000003.
        auto change = [&](double v) { return std::round((scale ? v * (1. + amount / 100.) : v + amount) * 1e4) / 1e4; };
        switch (opt.type()) {
        case coFloat:
        case coPercent:
        case coFloatOrPercent: { auto &o = static_cast<ConfigOptionFloat &>(opt); o.value = change(o.value); return {}; }
        case coInt: { auto &o = static_cast<ConfigOptionInt &>(opt); o.value = int(std::lround(change(o.value))); return {}; }
        case coFloats:
        case coPercents: {
            auto &o = static_cast<ConfigOptionVector<double> &>(opt);
            for (size_t i = 0; i < o.values.size(); ++i)
                if (!o.is_nil(i)) o.values[i] = change(o.values[i]);
            return {};
        }
        case coInts: {
            auto &o = static_cast<ConfigOptionVector<int> &>(opt);
            for (size_t i = 0; i < o.values.size(); ++i)
                if (!o.is_nil(i)) o.values[i] = int(std::lround(change(o.values[i])));
            return {};
        }
        case coFloatsOrPercents: {
            auto &o = static_cast<ConfigOptionVector<FloatOrPercent> &>(opt);
            for (size_t i = 0; i < o.values.size(); ++i)
                if (!o.is_nil(i)) o.values[i].value = change(o.values[i].value);
            return {};
        }
        default: return "This setting is not a number, so it can only be set, not changed by an amount.";
        }
    }

    if (expr[0] == '=') expr.erase(0, 1);
    std::unique_ptr<ConfigOption> value(opt.clone());
    if (!value->deserialize(expr)) return "\"" + expr + "\" is not a valid value for this setting.";
    if (auto *vec = dynamic_cast<ConfigOptionVectorBase *>(&opt); vec != nullptr && value->is_vector()) {
        const size_t n = static_cast<ConfigOptionVectorBase *>(value.get())->size();
        if (n == 1)
            for (size_t i = 0; i < vec->size(); ++i) vec->set_at(value.get(), i, 0); // one value for every extruder
        else if (n == vec->size())
            opt.set(value.get());
        else
            return "Give one value, or one per extruder (" + std::to_string(vec->size()) + ").";
        return {};
    }
    opt.set(value.get());
    return {};
}

t_config_option_keys settings_diff(const DynamicPrintConfig &a, const DynamicPrintConfig &b)
{
    t_config_option_keys keys = a.diff(b);
    keys.erase(std::remove_if(keys.begin(), keys.end(),
                              [](const std::string &k) { return k == "inherits" || boost::ends_with(k, "_settings_id"); }),
               keys.end());
    return keys;
}

}} // namespace Slic3r::PresetBulkEdit
