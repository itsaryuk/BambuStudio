#include <catch2/catch.hpp>

#include "libslic3r/SettingsAdvisor.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Model.hpp"

using namespace Slic3r;
using namespace Slic3r::SettingsAdvisor;

static indexed_triangle_set box(double x, double y, double z, const Vec3d &at = Vec3d::Zero())
{
    TriangleMesh mesh(its_make_cube(x, y, z));
    mesh.translate(at.cast<float>());
    return mesh.its;
}

static const Suggestion *find(const std::vector<Suggestion> &s, const std::string &key, int object_idx = -1)
{
    for (const Suggestion &x : s)
        if (x.key == key && x.object_idx == object_idx)
            return &x;
    return nullptr;
}

static ObjectAnalysis named(ObjectAnalysis a, const char *name, int idx)
{
    a.name       = name;
    a.object_idx = idx;
    return a;
}

TEST_CASE("Mesh analysis measures basic geometry", "[SettingsAdvisor]")
{
    SECTION("cube") {
        const ObjectAnalysis a = analyze_mesh(box(20, 20, 20));
        CHECK(a.height == Approx(20.));
        CHECK(a.volume == Approx(8000.).epsilon(1e-3));
        CHECK(a.bed_contact_area == Approx(400.));
        CHECK(a.top_flat_area == Approx(400.));
        CHECK(a.steep_overhang_area == Approx(0.));
        CHECK(a.min_wall == Approx(20.).epsilon(0.01));
    }
    SECTION("T shape: plate on a pillar overhangs") {
        indexed_triangle_set t = box(10, 10, 12, { 15, 15, 0 });
        its_merge(t, box(40, 40, 3, { 0, 0, 12 }));
        const ObjectAnalysis a = analyze_mesh(t);
        CHECK(a.bed_contact_area == Approx(100.));
        CHECK(a.steep_overhang_area > 1000.);
    }
    SECTION("thin vertical wall") {
        const ObjectAnalysis a = analyze_mesh(box(40, 0.8, 20));
        CHECK(a.min_wall == Approx(0.8).epsilon(0.05));
    }
}

TEST_CASE("Pre-slice measures unsupported and top areas from layer outlines", "[SettingsAdvisor]")
{
    indexed_triangle_set t = box(10, 10, 12, { 15, 15, 0 });
    its_merge(t, box(40, 40, 3, { 0, 0, 12 }));
    Model        model;
    ModelObject *obj = model.add_object();
    obj->name        = "tee";
    obj->add_volume(TriangleMesh(t));
    obj->add_instance();
    obj->instances.front()->set_offset(Vec3d(100, 100, 0));

    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    std::vector<ObjectAnalysis> objects(1);
    REQUIRE(preslice(model, cfg, objects));
    // The 40x40 plate overhangs the 10x10 pillar: ~1500 mm2 unsupported on its first layer.
    CHECK(objects[0].sliced_overhang_area == Approx(1500.).epsilon(0.1));
    CHECK(objects[0].sliced_max_top_area == Approx(1600.).epsilon(0.05));

    // A plain cube has nothing unsupported.
    Model        cube_model;
    ModelObject *cube = cube_model.add_object();
    cube->add_volume(TriangleMesh(box(20, 20, 20)));
    cube->add_instance();
    cube->instances.front()->set_offset(Vec3d(100, 100, 0));
    std::vector<ObjectAnalysis> cube_objects(1);
    REQUIRE(preslice(cube_model, cfg, cube_objects));
    CHECK(cube_objects[0].sliced_overhang_area == Approx(0.).margin(1.));
    CHECK_FALSE(objects[0].support_on_part); // the T's overhang has the bed below it

    auto presliced = [&cfg](indexed_triangle_set its) {
        Model        m;
        ModelObject *o = m.add_object();
        o->add_volume(TriangleMesh(its));
        o->add_instance();
        o->instances.front()->set_offset(Vec3d(100, 100, 0));
        std::vector<ObjectAnalysis> a(1);
        REQUIRE(preslice(m, cfg, a));
        return a[0];
    };
    // An 8 mm span between two posts bridges: nothing needs support.
    indexed_triangle_set bridge = box(10, 10, 10);
    its_merge(bridge, box(10, 10, 10, { 18, 0, 0 }));
    its_merge(bridge, box(28, 10, 3, { 0, 0, 10 }));
    CHECK(presliced(bridge).sliced_overhang_area == Approx(0.).margin(1.));
    // An overhang above a wider base needs support standing on the part.
    indexed_triangle_set step = box(40, 40, 5);
    its_merge(step, box(10, 10, 10, { 15, 15, 5 }));
    its_merge(step, box(40, 40, 3, { 0, 0, 15 }));
    const ObjectAnalysis s = presliced(step);
    CHECK(s.sliced_overhang_area > 1000.);
    CHECK(s.support_on_part);
}

TEST_CASE("Rules suggest settings for the intent and geometry", "[SettingsAdvisor]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_deserialize_strict("nozzle_diameter", "0.4");
    cfg.set_deserialize_strict("layer_height", "0.2");
    cfg.set_deserialize_strict("wall_loops", "2");
    cfg.set_deserialize_strict("sparse_infill_density", "15%");
    cfg.set_deserialize_strict("enable_support", "0");
    cfg.set_deserialize_strict("brim_type", "no_brim");
    cfg.set_deserialize_strict("wall_generator", "classic");
    cfg.set_deserialize_strict("filament_max_volumetric_speed", "30");

    const ObjectAnalysis cube = named(analyze_mesh(box(20, 20, 20)), "cube", 0);

    SECTION("functional: thick walls of wider lines, modest infill, plate-wide; nothing that already matches") {
        const auto s = suggest({ cube }, Intent::Functional, cfg);
        CHECK(find(s, "inner_wall_line_width")->suggested == "0.48"); // 1.2 x 0.4
        REQUIRE(find(s, "wall_loops"));
        CHECK(find(s, "wall_loops")->suggested == "4");               // 1.8 mm / 0.48
        CHECK(find(s, "sparse_infill_density")->suggested == "25%");
        REQUIRE(find(s, "layer_height"));                               // boxy: no slopes to show steps
        CHECK(find(s, "layer_height")->suggested == "0.24");
        CHECK_FALSE(find(s, "enable_support"));
        for (const Suggestion &x : s)
            CHECK(x.current != x.suggested);
    }
    SECTION("the plate's geometry changes the advice") {
        const ObjectAnalysis big = named(analyze_mesh(box(100, 100, 30)), "big", 0); // 300 cm3
        auto s = suggest({ big }, Intent::Functional, cfg);
        CHECK(find(s, "sparse_infill_density")->suggested == "20%");
        CHECK(find(s, "wall_loops")->suggested == "4");

        s = suggest({ big }, Intent::Visual, cfg);
        CHECK(find(s, "sparse_infill_pattern")->suggested == "adaptivecubic");
        CHECK_FALSE(find(s, "wall_loops")); // 2 walls, already set

        // A cone is all sloped surface: visual goes to the finest layers.
        const ObjectAnalysis cone = named(analyze_mesh(its_make_cone(20., 30.)), "cone", 0);
        s = suggest({ cone }, Intent::Visual, cfg);
        REQUIRE(find(s, "layer_height"));
        CHECK(find(s, "layer_height")->suggested == "0.08");
        // ...and it wins over a boxy neighbour; the boxy part alone would get 0.16.
        CHECK(find(suggest({ big, cone }, Intent::Visual, cfg), "layer_height")->suggested == "0.08");
        CHECK(find(suggest({ big }, Intent::Visual, cfg), "layer_height")->suggested == "0.16");

        // A slender functional pillar gets an extra wall.
        s = suggest({ named(analyze_mesh(box(5, 5, 60)), "pillar", 0) }, Intent::Functional, cfg);
        CHECK(find(s, "wall_loops")->suggested == "5");
    }
    SECTION("wide lines: same wall thickness, fewer walls") {
        DynamicPrintConfig wide = cfg;
        wide.set_deserialize_strict("nozzle_diameter", "0.8");
        wide.set_deserialize_strict("inner_wall_line_width", "0.82");
        wide.set_deserialize_strict("wall_loops", "3");
        const ObjectAnalysis big = named(analyze_mesh(box(100, 100, 30)), "big", 0);
        auto s = suggest({ big }, Intent::Functional, wide);
        REQUIRE(find(s, "wall_loops"));
        CHECK(find(s, "inner_wall_line_width")->suggested == "0.96"); // 1.2 x 0.8
        CHECK(find(s, "wall_loops")->suggested == "2");                // 1.8 mm / 0.96
        s = suggest({ cube }, Intent::Functional, wide);
        CHECK(find(s, "wall_loops")->suggested == "2");
    }
    SECTION("thicker layers are not suggested when flow already limits the speed") {
        DynamicPrintConfig slow = cfg;
        slow.set_deserialize_strict("filament_max_volumetric_speed", "2");
        CHECK_FALSE(find(suggest({ cube }, Intent::Functional, slow), "layer_height"));
        CHECK(find(suggest({ cube }, Intent::Functional, cfg), "layer_height"));
    }
    SECTION("sparse infill gets a thicker top") {
        const auto s = suggest({ named(analyze_mesh(box(100, 100, 30)), "big", 0) }, Intent::Visual, cfg);
        CHECK(find(s, "top_shell_thickness")->suggested == "1.6"); // 1.0 + 0.2 large top + 0.4 over 15%
    }
    SECTION("warping materials: every object's filament, PCTG doesn't count") {
        const ObjectAnalysis base = named(analyze_mesh(box(80, 80, 10)), "base", 0);
        DynamicPrintConfig two = cfg;
        two.set_deserialize_strict("filament_type", "PCTG;PLA");
        CHECK_FALSE(find(suggest({ base }, Intent::Functional, two), "brim_type"));
        ObjectAnalysis on_second = base;
        on_second.overrides.set_deserialize_strict("extruder", "2");
        two.set_deserialize_strict("filament_type", "PLA;PA-CF");
        CHECK(find(suggest({ on_second }, Intent::Functional, two), "brim_type"));
        CHECK_FALSE(find(suggest({ base }, Intent::Functional, two), "brim_type"));
    }
    SECTION("overhang turns on supports") {
        indexed_triangle_set t = box(10, 10, 12, { 15, 15, 0 });
        its_merge(t, box(40, 40, 3, { 0, 0, 12 }));
        const auto s = suggest({ named(analyze_mesh(t), "tee", 0) }, Intent::Visual, cfg);
        REQUIRE(find(s, "enable_support"));
        CHECK(find(s, "enable_support")->suggested == "1");
    }
    SECTION("tall pillar on a small base gets a brim") {
        const auto s = suggest({ named(analyze_mesh(box(5, 5, 60)), "pillar", 0) }, Intent::FastDraft, cfg);
        REQUIRE(find(s, "brim_type"));
        CHECK(find(s, "brim_type")->suggested == "outer_only");
    }
    SECTION("thin walls switch to Arachne") {
        const auto s = suggest({ named(analyze_mesh(box(40, 0.6, 20)), "fin", 0) }, Intent::Visual, cfg);
        REQUIRE(find(s, "wall_generator"));
        CHECK(find(s, "wall_generator")->suggested == "arachne");
    }
    SECTION("an object override makes the suggestion per-object") {
        ObjectAnalysis overridden = named(analyze_mesh(box(20, 20, 20)), "b", 1);
        overridden.overrides.set_deserialize_strict("wall_loops", "6");
        const auto s = suggest({ cube, overridden }, Intent::Functional, cfg);
        CHECK_FALSE(find(s, "wall_loops", -1));
        REQUIRE(find(s, "wall_loops", 0));
        REQUIRE(find(s, "wall_loops", 1));
        CHECK(find(s, "wall_loops", 1)->current == "6");
    }
    SECTION("suggested values are valid for the config") {
        indexed_triangle_set t = box(10, 10, 12, { 15, 15, 0 });
        its_merge(t, box(40, 40, 3, { 0, 0, 12 }));
        for (Intent intent : { Intent::Functional, Intent::Visual, Intent::FastDraft })
            for (const Suggestion &x : suggest({ named(analyze_mesh(t), "tee", 0), named(analyze_mesh(box(5, 5, 60)), "p", 1) }, intent, cfg)) {
                DynamicPrintConfig probe = cfg;
                CHECK_NOTHROW(probe.set_deserialize_strict(x.key, x.suggested));
            }
    }
}
