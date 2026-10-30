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

    const ObjectAnalysis cube = named(analyze_mesh(box(20, 20, 20)), "cube", 0);

    SECTION("functional: more walls and infill, plate-wide; nothing that already matches") {
        const auto s = suggest({ cube }, Intent::Functional, cfg);
        REQUIRE(find(s, "wall_loops"));
        CHECK(find(s, "wall_loops")->suggested == "4");
        CHECK(find(s, "sparse_infill_density")->suggested == "30%");
        CHECK_FALSE(find(s, "layer_height")); // 0.5 x 0.4 nozzle = 0.2, already set
        CHECK_FALSE(find(s, "enable_support"));
        for (const Suggestion &x : s)
            CHECK(x.current != x.suggested);
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
