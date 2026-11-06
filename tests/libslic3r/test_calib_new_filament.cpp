#include <catch2/catch.hpp>

#include "libslic3r/Calib.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

TEST_CASE("New filament plate: heights map to values", "[Calib]")
{
    NewFilamentCalib c(false, 20.);
    CHECK(c.pa_at(1.0) == 0.);              // below base: preset values
    CHECK(c.pa_at(NewFilamentCalib::base + 0.5) == 0.);
    CHECK(c.pa_at(NewFilamentCalib::base + 3.2) == Approx(0.1 * 3 / 15));
    CHECK(c.retract_at(c.height() + 5) == Approx(1.5)); // clamped at the top
    CHECK(c.mvs.front() == 16.);
    CHECK(c.mvs.back() == 36.);
    CHECK(NewFilamentCalib(true, 20.).pa_end == 1.0);
}

TEST_CASE("New filament plate: everything fits, nothing overlaps", "[Calib]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats({20.}));
    const NewFilamentCalib c(false, 20.);
    Model       m;
    std::string err = build_new_filament_plate(m, c, cfg, std::string(TEST_DATA_DIR) + "/../../resources", Vec2d(0, 0), Vec2d(256, 256));
    REQUIRE(err.empty());
    REQUIRE(m.objects.size() == c.mvs.size() + 2 + 9);

    std::vector<BoundingBoxf3> boxes;
    for (const ModelObject *o : m.objects) {
        BoundingBoxf3 bb = o->instance_bounding_box(0);
        CHECK(bb.min.x() >= 0.);
        CHECK(bb.min.y() >= 0.);
        CHECK(bb.max.x() <= 256.);
        CHECK(bb.max.y() <= 256.);
        CHECK(bb.min.z() == Approx(0.).margin(1e-6));
        boxes.push_back(bb);
    }
    // Pads come from one file and sit together; every other object stands alone.
    for (size_t i = 0; i < c.mvs.size() + 2; ++i)
        for (size_t j = 0; j < boxes.size(); ++j)
            if (i != j) {
                const bool apart = boxes[i].max.x() <= boxes[j].min.x() || boxes[j].max.x() <= boxes[i].min.x() ||
                                   boxes[i].max.y() <= boxes[j].min.y() || boxes[j].max.y() <= boxes[i].min.y();
                CHECK(apart);
            }

    // Towers are cut to the sweep height, the flow pads stay below where the sweep starts.
    CHECK(m.objects[c.mvs.size()]->instance_bounding_box(0).size().z() == Approx(c.height()).epsilon(0.02));
    CHECK(m.objects.back()->instance_bounding_box(0).size().z() < NewFilamentCalib::base);

    // Cylinders get faster left to right; everything else stays under the old flow cap.
    double last = 0.;
    for (size_t i = 0; i < c.mvs.size(); ++i) {
        const double s = dynamic_cast<const ConfigOptionVector<double> *>(m.objects[i]->config.option("outer_wall_speed"))->get_at(0);
        CHECK(s > last);
        last = s;
    }
    for (size_t i = c.mvs.size(); i < m.objects.size(); ++i)
        CHECK(dynamic_cast<const ConfigOptionVector<double> *>(m.objects[i]->config.option("outer_wall_speed"))->get_at(0) < dynamic_cast<const ConfigOptionVector<double> *>(m.objects[0]->config.option("outer_wall_speed"))->get_at(0));
}

TEST_CASE("New filament plate: too small a bed is reported", "[Calib]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    Model m;
    CHECK_FALSE(build_new_filament_plate(m, NewFilamentCalib(false, 20.), cfg, std::string(TEST_DATA_DIR) + "/../../resources", Vec2d(0, 0), Vec2d(120, 120)).empty());
}
