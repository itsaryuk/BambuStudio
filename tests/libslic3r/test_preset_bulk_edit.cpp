#include <catch2/catch.hpp>

#include "libslic3r/PresetBulkEdit.hpp"

using namespace Slic3r;

TEST_CASE("Bulk edit values", "[PresetBulkEdit]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    auto set = [&](const char *key, const char *expr) { return PresetBulkEdit::apply(*cfg.option(key), expr); };

    cfg.set_deserialize_strict("sparse_infill_density", "15%");
    REQUIRE(set("sparse_infill_density", "+5").empty());
    CHECK(cfg.opt_serialize("sparse_infill_density") == "20%");
    REQUIRE(set("sparse_infill_density", "-10%").empty());
    CHECK(cfg.opt_serialize("sparse_infill_density") == "18%");

    cfg.set_deserialize_strict("wall_loops", "2");
    REQUIRE(set("wall_loops", "+1").empty());
    CHECK(cfg.opt_int("wall_loops") == 3);
    REQUIRE(set("wall_loops", "5").empty());
    CHECK(cfg.opt_int("wall_loops") == 5);

    cfg.set_deserialize_strict("layer_height", "0.2");
    REQUIRE(set("layer_height", "+10%").empty());
    CHECK(cfg.opt_float("layer_height") == Approx(0.22));
    CHECK(cfg.opt_serialize("layer_height") == "0.22");

    // Vector: one value for every element, or a relative change of each.
    cfg.set_deserialize_strict("nozzle_temperature", "200,210");
    REQUIRE(set("nozzle_temperature", "+5").empty());
    CHECK(cfg.opt_serialize("nozzle_temperature") == "205,215");
    REQUIRE(set("nozzle_temperature", "220").empty());
    CHECK(cfg.opt_serialize("nozzle_temperature") == "220,220");
    CHECK(!set("nozzle_temperature", "1,2,3").empty());

    cfg.set_deserialize_strict("xy_hole_compensation", "0");
    REQUIRE(set("xy_hole_compensation", "=-0.1").empty());
    CHECK(cfg.opt_float("xy_hole_compensation") == Approx(-0.1));

    CHECK(!set("sparse_infill_pattern", "+1").empty()); // enum: set only
    CHECK(!set("sparse_infill_pattern", "nonsense").empty());
    REQUIRE(set("sparse_infill_pattern", "gyroid").empty());
    CHECK(cfg.opt_serialize("sparse_infill_pattern") == "gyroid");
    CHECK(!set("wall_loops", "+x").empty());
}

TEST_CASE("Settings diff ignores identity keys", "[PresetBulkEdit]")
{
    DynamicPrintConfig a = DynamicPrintConfig::full_print_config(), b = a;
    b.set_deserialize_strict("inherits", "Something");
    b.set_deserialize_strict("print_settings_id", "Other name");
    CHECK(PresetBulkEdit::settings_diff(a, b).empty());
    b.set_deserialize_strict("wall_loops", "7");
    CHECK(PresetBulkEdit::settings_diff(a, b) == t_config_option_keys{"wall_loops"});
}
