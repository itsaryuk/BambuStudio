#include <catch2/catch.hpp>

#include <cmath>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/GCode/ThermalQuality.hpp"

using namespace Slic3r;

namespace {

// A print with two objects: a 40x40 mm infill slab (≈ 50 s layers) and a 4x4 mm column (≈ 2 s layers).
struct Scene
{
    GCodeProcessorResult result;
    ThermalGCodeText     text;
    std::vector<int>     move_layers;
    int                  n_layers{10};

    Scene(bool with_slab = true, bool with_column = true)
    {
        float now = 0.f;
        auto add_line = [&](const std::string& l) { text.lines.push_back(l); };
        auto add_move = [&](EMoveType type, ExtrusionRole role, Vec3f pos, float speed, float len) {
            GCodeProcessorResult::MoveVertex m;
            m.type           = type;
            m.extrusion_role = role;
            m.position       = pos;
            m.feedrate       = speed;
            m.width          = 0.42f;
            m.height         = 0.2f;
            m.mm3_per_mm     = 0.084f;
            m.fan_speed      = 100.f;
            m.temperature    = 220.f;
            now += len / speed;
            m.time     = {now, now};
            m.gcode_id = (unsigned int) text.lines.size() + 1; // line about to be written
            result.moves.push_back(m);
            char buf[128];
            std::snprintf(buf, sizeof(buf), "G1 X%.2f Y%.2f%s F%d", pos.x(), pos.y(), type == EMoveType::Extrude ? " E1" : "", int(speed * 60));
            add_line(buf);
        };
        add_line("; HEADER");
        result.moves.push_back({}); // the processor always starts with a dummy move
        for (int L = 0; L < n_layers; ++L) {
            add_line("; CHANGE_LAYER");
            const float z = 0.2f * (L + 1);
            // slab: 40 lines, 40 mm each, 30 mm/s
            for (int i = 0; with_slab && i < 40; ++i) {
                const float y = float(i);
                add_move(EMoveType::Travel, erNone, Vec3f(0.f, y, z), 200.f, 40.f);
                add_move(EMoveType::Extrude, erInternalInfill, Vec3f(40.f, y, z), 30.f, 40.f);
            }
            // column: 4 x 4 mm perimeter at x = 100, 8 mm/s
            if (!with_column) continue;
            add_move(EMoveType::Travel, erNone, Vec3f(100.f, 0.f, z), 200.f, 60.f);
            add_line("G1 F480"); // F-only line that applies to the next extrusion
            add_move(EMoveType::Extrude, erPerimeter, Vec3f(104.f, 0.f, z), 8.f, 4.f);
            add_move(EMoveType::Extrude, erPerimeter, Vec3f(104.f, 4.f, z), 8.f, 4.f);
            add_move(EMoveType::Extrude, erPerimeter, Vec3f(100.f, 4.f, z), 8.f, 4.f);
            add_move(EMoveType::Extrude, erPerimeter, Vec3f(100.f, 0.f, z), 8.f, 4.f);
        }
        // layers of lines, as ThermalGCodeText::load would compute them
        text.line_layers.assign(1, -1);
        int layer = -1;
        for (const std::string& l : text.lines) {
            if (l.rfind("; CHANGE_LAYER", 0) == 0) ++layer;
            text.line_layers.push_back(layer);
        }
        move_layers = ThermalQuality::layers_of_moves(result, text.line_layers);
    }
};

ThermalModelParams pla_cold_bed()
{
    ThermalModelParams p = ThermalModelParams::for_material("PLA", 45.f);
    p.bed_temp           = 25.f; // keep the bed out of the picture so the slab is clearly cold
    return p;
}

} // namespace

TEST_CASE("Thermal quality: fast thin layers run hot, slow wide layers run cold", "[ThermalQuality]")
{
    auto mean_from_layer_2 = [](const Scene& s, const ThermalReport& rep) {
        double acc = 0.;
        int    n   = 0;
        for (size_t i = 1; i < s.result.moves.size(); ++i)
            if (s.result.moves[i].type == EMoveType::Extrude && s.move_layers[i] >= 2) { acc += rep.moves[i].mean; ++n; }
        return acc / n;
    };
    Scene slab(true, false);
    ThermalReport slab_rep = ThermalQuality::simulate(slab.result, slab.move_layers, pla_cold_bed());
    REQUIRE(slab_rep.moves.size() == slab.result.moves.size());
    REQUIRE(int(slab_rep.layer_mean.size()) == slab.n_layers);
    const double slab_mean = mean_from_layer_2(slab, slab_rep);
    INFO("slab " << slab_mean);
    REQUIRE(slab_mean == Approx(-0.5).margin(0.05)); // ~50 s layers on a cold bed: fully cooled to 25 °C, PLA is fine with that
    REQUIRE(slab_rep.direction == "NONE");
    // The same slab in ABS (comfort window around 100 °C) is a warping risk.
    ThermalModelParams abs = ThermalModelParams::for_material("ABS", 100.f);
    abs.bed_temp           = 25.f;
    ThermalReport abs_rep  = ThermalQuality::simulate(slab.result, slab.move_layers, abs);
    REQUIRE(abs_rep.direction == "OVERCOOLING");
    REQUIRE(abs_rep.outcome == "LIKELY_FAIL");
    REQUIRE(slab_rep.layer_mean[5] == Approx(slab_mean).margin(0.05));

    Scene column(false, true);
    ThermalReport col_rep = ThermalQuality::simulate(column.result, column.move_layers, pla_cold_bed());
    const double col_mean = mean_from_layer_2(column, col_rep);
    INFO("column " << col_mean);
    REQUIRE(col_mean > 0.3);                   // 2 s layers: previous layer still warm, fan keeps it printable
    REQUIRE(col_rep.direction == "NONE");
    // Without part cooling the same column overheats.
    for (auto& m : column.result.moves) m.fan_speed = 0.f;
    ThermalReport nofan_rep = ThermalQuality::simulate(column.result, column.move_layers, pla_cold_bed());
    REQUIRE(mean_from_layer_2(column, nofan_rep) > col_mean);
    REQUIRE(nofan_rep.direction == "OVERHEATING");
    REQUIRE(!nofan_rep.caveats.empty());
    REQUIRE(!nofan_rep.fixes.empty());
    REQUIRE(col_rep.moves[2].min <= col_rep.moves[2].mean);
    REQUIRE(col_rep.moves[2].mean <= col_rep.moves[2].max);
}

TEST_CASE("Thermal quality: first layer sits on the bed", "[ThermalQuality]")
{
    Scene s;
    ThermalModelParams p = ThermalModelParams::for_material("PLA", 45.f);
    p.bed_temp           = 105.f; // (105-45)/40 = 1.5 -> clamped to 1
    ThermalReport rep    = ThermalQuality::simulate(s.result, s.move_layers, p);
    REQUIRE(rep.moves[2].mean == Approx(1.f)); // moves[1] is the travel to the first line
}

TEST_CASE("Thermal quality: optimize speeds up cold layers and slows hot ones within limits", "[ThermalQuality]")
{
    Scene s(true, false);
    ThermalOptimizeOptions o;
    o.max_speed_factor = 1.4f;
    o.min_speed_factor = 0.6f;
    std::vector<float> f = ThermalQuality::optimize(s.result, s.move_layers, pla_cold_bed(), o);
    REQUIRE(int(f.size()) == s.n_layers);
    REQUIRE(f[0] == 1.f); // first layer never touched
    for (int L = 1; L < s.n_layers; ++L) {
        REQUIRE(f[L] >= o.min_speed_factor);
        REQUIRE(f[L] <= o.max_speed_factor);
    }
    // Layer mean is dominated by the slab (most of the time), so cold layers get faster.
    REQUIRE(f[5] > 1.05f);

    // Volumetric cap: 30 mm/s * 0.084 = 2.52 mm3/s; a 2.6 cap leaves < 4 % headroom.
    o.max_volumetric = 2.6f;
    f                = ThermalQuality::optimize(s.result, s.move_layers, pla_cold_bed(), o);
    REQUIRE(f[5] <= 2.6f / 2.52f + 1e-3f);

    // Time factor < 1 when layers got faster.
    o.max_volumetric = -1.f;
    f                = ThermalQuality::optimize(s.result, s.move_layers, pla_cold_bed(), o);
    REQUIRE(ThermalQuality::time_factor(s.result, s.move_layers, f, o) < 1.f);

    // Hot column: slowed down, never below the floor, and the result lands inside the window.
    Scene c(false, true);
    std::vector<float> fc = ThermalQuality::optimize(c.result, c.move_layers, pla_cold_bed(), o);
    REQUIRE(fc[5] < 0.95f);
    REQUIRE(fc[5] >= o.min_speed_factor);
    ThermalReport after = ThermalQuality::simulate(c.result, c.move_layers, pla_cold_bed(), &fc, &o);
    ThermalReport before = ThermalQuality::simulate(c.result, c.move_layers, pla_cold_bed());
    REQUIRE(std::abs(after.layer_mean[5]) < std::abs(before.layer_mean[5]));
    REQUIRE(ThermalQuality::quality_grade(std::abs(before.mean), std::abs(after.mean)) != "LOW");
}

TEST_CASE("Thermal quality: written G-code carries tags and rescaled feedrates", "[ThermalQuality]")
{
    Scene s;
    ThermalOptimizeOptions o;
    o.keep_outer_walls = false;
    std::vector<float> f(s.n_layers, 1.f);
    f[3]              = 1.25f;
    ThermalReport rep = ThermalQuality::simulate(s.result, s.move_layers, pla_cold_bed(), &f, &o);

    boost::filesystem::path out = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("thermal-%%%%.gcode");
    REQUIRE(ThermalQuality::write_gcode(s.text, out.string(), s.result, s.move_layers, rep, &f, &o));

    ThermalGCodeText back;
    REQUIRE(ThermalGCodeText::load(out.string(), back));
    boost::filesystem::remove(out);
    REQUIRE(back.lines.size() == s.text.lines.size());
    REQUIRE(back.line_layers == s.text.line_layers);

    int tagged = 0, scaled_slab = 0, scaled_f_only = 0;
    for (size_t i = 0; i < back.lines.size(); ++i) {
        const std::string& orig = s.text.lines[i];
        const std::string& now  = back.lines[i];
        const int          L    = back.line_layers[i + 1];
        const bool extrusion    = orig.find(" E1") != std::string::npos;
        if (extrusion) {
            REQUIRE(now.find(";thermal=ti.max=") != std::string::npos);
            ++tagged;
        } else {
            REQUIRE(now.find(";thermal=") == std::string::npos);
        }
        if (L == 3 && extrusion && orig.find("F1800") != std::string::npos) { // slab line, 30 mm/s
            REQUIRE(now.find("F2250") != std::string::npos);
            ++scaled_slab;
        }
        if (L == 3 && orig == "G1 F480") { REQUIRE(now == "G1 F600"); ++scaled_f_only; }
        if (L == 3 && orig.find("F12000") != std::string::npos) REQUIRE(now.find("F12000") != std::string::npos); // travel untouched
        if (L != 3 && !extrusion) REQUIRE(now == orig);
    }
    REQUIRE(tagged == s.n_layers * 44);
    REQUIRE(scaled_slab == 40);
    REQUIRE(scaled_f_only == 1);

    // The processor's tag parser reads our tag.
    bool is_thermal = false;
    auto ti = GCodeProcessor::parse_helioadditive_comment("G1 X1 Y1 E1 ;thermal=ti.max=0.50,ti.min=-0.25,ti.mean=0.10", is_thermal);
    REQUIRE(is_thermal);
    REQUIRE(ti.max == Approx(50.f));
    REQUIRE(ti.min == Approx(-25.f));
    REQUIRE(ti.mean == Approx(10.f));
    REQUIRE(!ti.isNull);
}

TEST_CASE("Thermal quality: material classes, per-filament models, profile floor and M73 rewrite", "[ThermalQuality]")
{
    // Class lookup: exact, base type before '-', trailing digits stripped, fillers cool faster, unknown falls back.
    ThermalModelParams abs_gf = ThermalModelParams::for_material("ABS-GF", -1.f);
    REQUIRE(abs_gf.material == "ABS");
    REQUIRE(abs_gf.ideal_substrate_temp == Approx(100.f));
    REQUIRE(abs_gf.warp_prone);
    REQUIRE(abs_gf.cooling_time_const == Approx(6.f * 0.85f));
    ThermalModelParams pa6 = ThermalModelParams::for_material("pa6-cf", -1.f);
    REQUIRE(pa6.material == "PA");
    REQUIRE(pa6.cooling_time_const == Approx(6.f * 1.2f * 0.85f));
    REQUIRE(ThermalModelParams::for_material("PLA-AERO", -1.f).material == "PLA");
    REQUIRE(ThermalModelParams::for_material("TPU-AMS", -1.f).window_half_width == Approx(50.f));
    ThermalModelParams unknown = ThermalModelParams::for_material("MYSTERY", -1.f);
    REQUIRE(unknown.ideal_substrate_temp == Approx(70.f));
    REQUIRE(unknown.material == "MYSTERY");
    REQUIRE(ThermalModelParams::for_material("ABS", 108.f).ideal_substrate_temp == Approx(108.f)); // profile wins

    // Two filament slots: the column prints with slot 1. Same toolpaths, different comfort windows.
    Scene s(true, true);
    for (auto& m : s.result.moves)
        if (m.type == EMoveType::Extrude && m.extrusion_role == erPerimeter) m.extruder_id = 1;
    ThermalMaterials mats = {pla_cold_bed(), ThermalModelParams::for_material("ABS", 100.f)};
    mats[1].bed_temp = 25.f;
    ThermalReport two = ThermalQuality::simulate(s.result, s.move_layers, mats);
    ThermalReport one = ThermalQuality::simulate(s.result, s.move_layers, pla_cold_bed());
    size_t col_move = 0;
    for (size_t i = 1; i < s.result.moves.size(); ++i)
        if (s.result.moves[i].extruder_id == 1 && s.move_layers[i] == 5) { col_move = i; break; }
    REQUIRE(col_move > 0);
    REQUIRE(two.moves[col_move].mean < one.moves[col_move].mean); // ABS wants a 100 °C substrate: the warm column reads colder
    REQUIRE(two.assumptions.size() == 2);
    REQUIRE(two.assumptions[1].find("ABS") != std::string::npos);
    REQUIRE(two.assumptions[0].find("PLA") != std::string::npos);
    bool warp_advice = false;
    for (const auto& f : two.fixes) warp_advice |= f.second.find("enclosure") != std::string::npos || f.second.find("warps") != std::string::npos;
    if (two.direction != "NONE") REQUIRE(warp_advice);

    // Profile slow-down floor and max volumetric cap, without dialog overrides.
    Scene c(false, true); // hot column at 8 mm/s gets slowed down
    ThermalOptimizeOptions o;
    o.keep_outer_walls = false;
    ThermalMaterials floor_mats = {pla_cold_bed()};
    floor_mats[0].min_speed    = 7.f; // 7/8 = 0.875 floor
    std::vector<float> f = ThermalQuality::optimize(c.result, c.move_layers, floor_mats, o);
    REQUIRE(f[5] >= 0.875f - 1e-4f);
    REQUIRE(f[5] < 0.95f);
    Scene slab(true, false);
    ThermalMaterials cap_mats = {pla_cold_bed()};
    cap_mats[0].max_volumetric = 2.6f; // 30 mm/s * 0.084 = 2.52 mm3/s
    f = ThermalQuality::optimize(slab.result, slab.move_layers, cap_mats, o);
    REQUIRE(f[5] <= 2.6f / 2.52f + 1e-3f);

    // M73 lines are recomputed from the rescaled timeline.
    Scene m;
    m.text.lines[0] = "M73 P0 R999";       // replaces "; HEADER", line ids unchanged
    m.text.lines.push_back("M73 P100 R0");  // appended, ids unchanged
    m.text.line_layers.push_back(m.text.line_layers.back());
    std::vector<float> fx(m.n_layers, 1.f);
    fx[3] = 2.f;
    ThermalReport rep = ThermalQuality::simulate(m.result, m.move_layers, pla_cold_bed(), &fx, &o);
    double total = 0.;
    for (size_t i = 1; i < m.result.moves.size(); ++i) {
        double dt = m.result.moves[i].time[0] - m.result.moves[i - 1].time[0];
        if (m.move_layers[i] == 3 && ThermalQuality::eligible(m.result.moves[i], 3, o)) dt /= 2.;
        total += dt;
    }
    boost::filesystem::path out = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("thermal-%%%%.gcode");
    REQUIRE(ThermalQuality::write_gcode(m.text, out.string(), m.result, m.move_layers, rep, &fx, &o));
    ThermalGCodeText back;
    REQUIRE(ThermalGCodeText::load(out.string(), back));
    boost::filesystem::remove(out);
    char expect[48];
    std::snprintf(expect, sizeof(expect), "M73 P0 R%d", int((total + 30.) / 60.));
    REQUIRE(back.lines.front() == expect);
    REQUIRE(back.lines.back() == "M73 P100 R0");
    REQUIRE(total < m.result.moves.back().time[0]); // the rescaled timeline really is shorter
}
