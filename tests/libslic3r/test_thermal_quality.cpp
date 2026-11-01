#include <catch2/catch.hpp>

#include <cmath>
#include <map>

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
            REQUIRE(now.find(";thermal=") != std::string::npos);
            REQUIRE(now.rfind(";thermal=") == now.find(";thermal="));
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
    bool is_compact = false;
    auto tc = GCodeProcessor::parse_helioadditive_comment("G1 X1 Y1 E1 ;thermal=0.50,-0.25,0.10", is_compact);
    REQUIRE(is_compact);
    REQUIRE(tc.max == Approx(ti.max));
    REQUIRE(tc.min == Approx(ti.min));
    REQUIRE(tc.mean == Approx(ti.mean));
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
    REQUIRE(two.assumptions.size() == 3); // environment, then one line per filament slot
    REQUIRE(two.assumptions[0].find("Environment") != std::string::npos);
    REQUIRE(two.assumptions[1].find("PLA") != std::string::npos);
    REQUIRE(two.assumptions[2].find("ABS") != std::string::npos);
    bool warp_advice = false;
    for (const auto& f : two.fixes) warp_advice |= f.second.find("enclosure") != std::string::npos || f.second.find("warps") != std::string::npos;
    if (two.direction != "NONE") REQUIRE(warp_advice);

    // A layer still hot at the profile's slow-down floor goes below it, down to the slow-down limit; max volumetric caps speed-ups.
    Scene c(false, true); // hot column at 8 mm/s gets slowed down
    ThermalOptimizeOptions o;
    o.keep_outer_walls = false;
    ThermalMaterials floor_mats = {pla_cold_bed()};
    floor_mats[0].min_speed    = 7.9f; // 7.9/8: the slicer already runs it at its floor
    std::vector<float> f = ThermalQuality::optimize(c.result, c.move_layers, floor_mats, o);
    REQUIRE(f[5] < 7.9f / 8.f);
    REQUIRE(f[5] >= o.min_speed_factor);
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
    m.result.moves[7].time[0] = 0.f; // the processor leaves the clock unset on some moves; that must not count as a jump
    double total = 0., last_t = 0.;
    for (size_t i = 1; i < m.result.moves.size(); ++i) {
        const double t  = m.result.moves[i].time[0] > 0.f ? double(m.result.moves[i].time[0]) : last_t;
        double       dt = std::max(0., t - last_t);
        last_t          = t;
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

TEST_CASE("Thermal quality: tower calibration fits the cooling constant", "[ThermalQuality]")
{
    Scene column(false, true); // 2 s layers only
    ThermalMaterials mats{pla_cold_bed()};
    const float      target = mats[0].ideal_substrate_temp + 0.8f * mats[0].window_half_width;
    bool             bounded = true;
    const float      tau     = ThermalQuality::fit_cooling_constant(column.result, column.move_layers, mats, 0, 5, target, &bounded);
    REQUIRE(!bounded);
    REQUIRE(tau > 0.5f);
    REQUIRE(tau < 60.f);
    mats[0].cooling_time_const = tau;
    const ThermalReport rep = ThermalQuality::simulate(column.result, column.move_layers, mats);
    REQUIRE(rep.layer_substrate_temp[5] == Approx(target).margin(1.f));
    // a longer constant keeps the substrate hotter, so the fit is unique
    mats[0].cooling_time_const = tau * 2.f;
    REQUIRE(ThermalQuality::simulate(column.result, column.move_layers, mats).layer_substrate_temp[5] > target);
    // the bound is reported when no constant can reach the target
    ThermalQuality::fit_cooling_constant(column.result, column.move_layers, mats, 0, 5, 1000.f, &bounded);
    REQUIRE(bounded);
}

TEST_CASE("Thermal quality: cooling pauses for layers that cannot cool in time", "[ThermalQuality]")
{
    Scene column(false, true); // 2 s layers
    for (auto& m : column.result.moves) m.fan_speed = 0.f; // no fan: hot
    ThermalMaterials       mats{pla_cold_bed()};
    ThermalOptimizeOptions o;
    o.min_speed_factor = o.max_speed_factor = 1.f; // speed pinned: only a pause can help
    o.max_pause        = 30.f;
    std::vector<float>       pauses;
    const std::vector<float> f = ThermalQuality::optimize(column.result, column.move_layers, mats, o, &pauses);
    REQUIRE(pauses.size() == f.size());
    float total = 0.f;
    int   paused = 0;
    for (float x : pauses) { REQUIRE(x >= 0.f); REQUIRE(x <= 30.f); total += x; paused += x > 0.05f; }
    REQUIRE(total > 0.f);
    const ThermalReport before = ThermalQuality::simulate(column.result, column.move_layers, mats, &f, &o);
    const ThermalReport after  = ThermalQuality::simulate(column.result, column.move_layers, mats, &f, &o, &pauses);
    REQUIRE(after.mean < before.mean);
    REQUIRE(before.hot_top_z > 0.f);
    REQUIRE(ThermalQuality::time_factor(column.result, column.move_layers, f, o, &pauses) > ThermalQuality::time_factor(column.result, column.move_layers, f, o));
    // one dwell per paused layer in the written file, and the file still loads
    boost::filesystem::path out = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("thermal-%%%%.gcode");
    REQUIRE(ThermalQuality::write_gcode(column.text, out.string(), column.result, column.move_layers, after, &f, &o, &pauses));
    ThermalGCodeText back;
    REQUIRE(ThermalGCodeText::load(out.string(), back));
    boost::filesystem::remove(out);
    int dwells = 0;
    for (const std::string& l : back.lines) dwells += l.rfind("G4 P", 0) == 0;
    REQUIRE(dwells == paused);
    REQUIRE(back.line_layers.size() == back.lines.size() + 1);

    // the bed outline and retraction settings come from the config block; "0x0" is not a hex number
    boost::filesystem::path cfg = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("thermal-cfg-%%%%.gcode");
    {
        boost::nowide::ofstream w(cfg.string().c_str());
        w << "G1 X0 Y0\n; printable_area = 0x0,340x0,340x320,0x320\n; use_relative_e_distances = 1\n; retraction_length = 3,3\n; filament_retraction_length = nil\n; travel_speed = 1000\n";
    }
    ThermalGCodeText parsed;
    REQUIRE(ThermalGCodeText::load(cfg.string(), parsed));
    boost::filesystem::remove(cfg);
    REQUIRE(parsed.printable_area.size() == 4);
    REQUIRE(parsed.printable_area[2] == Vec2f(340.f, 320.f));
    REQUIRE(parsed.relative_e);
    REQUIRE(parsed.retraction_length == Approx(3.f));
    REQUIRE(parsed.travel_speed == Approx(1000.f));
}

TEST_CASE("Thermal quality: lateral conduction and preset-derived cooling", "[ThermalQuality]")
{
    // A cold line laid next to a line that is still hot reads warmer when heat may cross between cells.
    GCodeProcessorResult result;
    std::vector<int>     layers;
    float                now = 0.f;
    auto add = [&](EMoveType type, Vec3f pos, float dt, int layer) {
        GCodeProcessorResult::MoveVertex m;
        m.type = type; m.extrusion_role = type == EMoveType::Extrude ? erInternalInfill : erNone; m.position = pos;
        m.feedrate = 30.f; m.width = 0.42f; m.height = 0.2f; m.mm3_per_mm = 0.084f; m.fan_speed = 0.f; m.temperature = 220.f;
        now += dt; m.time = {now, now}; m.gcode_id = (unsigned int) result.moves.size() + 1;
        result.moves.push_back(m); layers.push_back(layer);
    };
    result.moves.push_back({}); layers.push_back(-1);
    // layer 0: two lines, y = 0 and y = 1, long ago
    add(EMoveType::Travel, Vec3f(0, 0, 0.2f), 1.f, 0); add(EMoveType::Extrude, Vec3f(20, 0, 0.2f), 1.f, 0);
    add(EMoveType::Travel, Vec3f(0, 1, 0.2f), 1.f, 0); add(EMoveType::Extrude, Vec3f(20, 1, 0.2f), 1.f, 0);
    add(EMoveType::Travel, Vec3f(0, 0, 0.4f), 300.f, 1); // everything cold now
    add(EMoveType::Extrude, Vec3f(20, 0, 0.4f), 1.f, 1); // hot line at y = 0
    add(EMoveType::Travel, Vec3f(0, 1, 0.4f), 15.f, 1);  // 15 s: heat has crossed sqrt(0.1 * 15) = 1.2 mm
    add(EMoveType::Extrude, Vec3f(20, 1, 0.4f), 1.f, 1); // laid 1 mm away, on a cold layer-0 cell
    ThermalModelParams p = pla_cold_bed();
    p.cooling_time_const = 30.f; // slow cooling so the first line is still clearly warm after 15 s
    ThermalModelParams no_conduction = p; no_conduction.diffusivity = 0.f;
    const size_t probe = result.moves.size() - 1;
    REQUIRE(ThermalQuality::simulate(result, layers, p).moves[probe].mean > ThermalQuality::simulate(result, layers, no_conduction).moves[probe].mean + 0.05f);

    // Preset calibration: PLA's 4 s minimum layer time at full fan -> cooling constant well above the 6 s class default,
    // longer minimum layer time -> larger constant, disabled slow-down -> untouched.
    ThermalModelParams pla = ThermalModelParams::for_material("PLA", 45.f);
    pla.calibrate_cooling_from_preset(4.f, 100.f, 220.f, 25.f);
    REQUIRE(pla.cooling_time_const > 10.f);
    REQUIRE(pla.cooling_time_const < 20.f);
    ThermalModelParams pla8 = ThermalModelParams::for_material("PLA", 45.f);
    pla8.calibrate_cooling_from_preset(8.f, 100.f, 220.f, 25.f);
    REQUIRE(pla8.cooling_time_const > pla.cooling_time_const);
    ThermalModelParams untouched = ThermalModelParams::for_material("PLA", 45.f);
    untouched.calibrate_cooling_from_preset(0.f, 100.f, 220.f, 25.f);
    REQUIRE(untouched.cooling_time_const == Approx(6.f));

    // Substrate temperatures are reported per layer, unclamped.
    Scene s;
    ThermalModelParams hot_bed = ThermalModelParams::for_material("PLA", 45.f);
    hot_bed.bed_temp           = 105.f;
    ThermalReport rep          = ThermalQuality::simulate(s.result, s.move_layers, hot_bed);
    REQUIRE(rep.layer_substrate_temp.size() == rep.layer_mean.size());
    REQUIRE(rep.layer_substrate_temp[0] == Approx(105.f).margin(1.f));
}

#include <cstdlib>
#include "libslic3r/GCode/GCodeProcessor.hpp"

// Hidden: THERMAL_GCODE=<file> ./thermal_quality_tests "[thermal_file]" runs the engine on a real file and prints what it did.
TEST_CASE("run the engine on a real G-code file", "[.][thermal_file]")
{
    const char* path = std::getenv("THERMAL_GCODE");
    if (path == nullptr) return;
    GCodeProcessor proc;
    proc.process_file(path);
    const GCodeProcessorResult& r = proc.get_result();
    ThermalGCodeText text;
    REQUIRE(ThermalGCodeText::load(path, text));
    const std::vector<int> layers = ThermalQuality::layers_of_moves(r, text.line_layers);
    auto env = [](const char* k, float d) { const char* v = std::getenv(k); return v != nullptr ? float(std::atof(v)) : d; };
    const char* mat_name = std::getenv("THERMAL_MATERIAL");
    ThermalModelParams p = ThermalModelParams::for_material(mat_name != nullptr ? mat_name : "PETG", -1.f);
    p.min_speed = env("THERMAL_MIN_SPEED", 20.f); p.max_volumetric = env("THERMAL_MAX_VOL", 21.f); p.bed_temp = env("THERMAL_BED", 70.f);
    p.cooling_time_const = env("THERMAL_TAU", p.cooling_time_const);
    p.chamber_temp = env("THERMAL_CHAMBER", p.chamber_temp); p.bed_air_decay_height = env("THERMAL_DECAY", p.bed_air_decay_height);
    ThermalMaterials       mats{p};
    ThermalOptimizeOptions o;
    ThermalReport          rep = ThermalQuality::simulate(r, layers, mats);
    std::vector<float>     f   = ThermalQuality::optimize(r, layers, mats, o);
    std::printf("moves=%zu lines=%zu last_time=%.1f s layers=%zu mean=%.2f stddev=%.2f outcome=%s\n", r.moves.size(), text.lines.size(),
                r.moves.empty() ? 0.f : r.moves.back().time[0], f.size(), rep.mean, rep.stddev, rep.outcome.c_str());
    float fmin = 9.f, fmax = 0.f;
    for (float x : f) { fmin = std::min(fmin, x); fmax = std::max(fmax, x); }
    std::printf("factor min %.3f max %.3f; first 6:", fmin, fmax);
    for (size_t i = 0; i < std::min<size_t>(6, f.size()); ++i) std::printf(" %.3f", f[i]);
    std::printf("\n");
    double big = 0; size_t big_i = 0; int neg = 0;
    for (size_t i = 1; i < r.moves.size(); ++i) {
        const double dt = double(r.moves[i].time[0]) - double(r.moves[i - 1].time[0]);
        if (dt < 0) ++neg;
        if (dt > big) { big = dt; big_i = i; }
    }
    std::printf("largest dt %.1f s at move %zu (gcode line %u, type %d), negative dts %d\n", big, big_i, r.moves[big_i].gcode_id, int(r.moves[big_i].type), neg);
    int shown_neg = 0;
    for (size_t i = 1; i < r.moves.size() && shown_neg < 3; ++i)
        if (double(r.moves[i].time[0]) < double(r.moves[i - 1].time[0])) {
            ++shown_neg;
            for (size_t j = (i >= 3 ? i - 3 : 0); j < std::min(i + 3, r.moves.size()); ++j)
                std::printf("  move %zu type %d line %u layer %d t0 %.2f t1 %.2f layer_dur %.2f\n", j, int(r.moves[j].type), r.moves[j].gcode_id, layers[j], r.moves[j].time[0], r.moves[j].time[1], r.moves[j].layer_duration);
            std::printf("  --\n");
        }
    std::printf("time_factor %.3f\n", ThermalQuality::time_factor(r, layers, f, o));
    if (std::getenv("THERMAL_LAYERS") != nullptr) {
        const ThermalReport after = ThermalQuality::simulate(r, layers, mats, &f, &o);
        for (size_t L = 0; L < f.size(); L += 4)
            std::printf("  z %5.1f  mean before %+.2f after %+.2f  factor %.2f\n", rep.layer_z[L], rep.layer_mean[L], after.layer_mean[L], f[L]);
    }
    if (const char* h = std::getenv("THERMAL_CALIB_HEIGHT")) { // emulate Calibrate at that height, then optimize with the fitted constant
        const float height = float(std::atof(h));
        int cal_layer = -1, slot = 0;
        for (size_t i = 1; i < r.moves.size(); ++i)
            if (r.moves[i].type == EMoveType::Extrude && layers[i] >= 0 && r.moves[i].position.z() >= height) { cal_layer = layers[i]; slot = r.moves[i].extruder_id; break; }
        bool bounded = false;
        const float target = mats[0].ideal_substrate_temp + 0.8f * mats[0].window_half_width;
        const float tau    = ThermalQuality::fit_cooling_constant(r, layers, mats, slot, cal_layer, target, &bounded);
        std::printf("calibrate: layer %d target %.1f C -> tau %.2f s (class %.2f) bounded %d\n", cal_layer, target, tau, mats[0].cooling_time_const, int(bounded));
        mats[0].cooling_time_const = tau;
        const ThermalReport before = ThermalQuality::simulate(r, layers, mats);
        if (const char* mp = std::getenv("THERMAL_PAUSE")) o.max_pause = float(std::atof(mp));
        std::vector<float> pauses;
        f = ThermalQuality::optimize(r, layers, mats, o, &pauses);
        const ThermalReport after = ThermalQuality::simulate(r, layers, mats, &f, &o, &pauses);
        float total_pause = 0.f; int paused = 0;
        for (float x : pauses) { total_pause += x; paused += x > 0.05f; }
        std::printf("mean %.2f -> %.2f, stddev %.2f -> %.2f, time_factor %.3f, pauses %d layers %.0f s, hot z %.1f..%.1f\n", before.mean, after.mean, before.stddev, after.stddev,
                    ThermalQuality::time_factor(r, layers, f, o, &pauses), paused, total_pause, before.hot_bottom_z, before.hot_top_z);
        if (o.max_pause > 0.f) REQUIRE(ThermalQuality::write_gcode(text, std::string(path) + ".paused.gcode", r, layers, after, &f, &o, &pauses));
        for (size_t L = 0; L < f.size(); L += 10)
            std::printf("  layer %3zu  mean before %+.2f after %+.2f  factor %.2f\n", L, before.layer_mean[L], after.layer_mean[L], f[L]);
    }
    for (size_t L = f.size() >= 16 ? f.size() - 16 : 0; L < f.size(); ++L) {
        std::map<int, int> roles; int eligible = 0;
        for (size_t i = 0; i < r.moves.size(); ++i)
            if (layers[i] == int(L) && r.moves[i].type == EMoveType::Extrude) { ++roles[int(r.moves[i].extrusion_role)]; eligible += ThermalQuality::eligible(r.moves[i], int(L), o); }
        std::printf("  layer %zu factor %.3f eligible %d roles:", L, f[L], eligible);
        for (auto& [role, n] : roles) std::printf(" %d:%d", role, n);
        std::printf("\n");
    }
    const std::string dst = std::string(path) + ".thermal_out.gcode";
    REQUIRE(ThermalQuality::write_gcode(text, dst, r, layers, rep, &f, &o));
    boost::nowide::ifstream in(dst.c_str());
    std::string line; int shown = 0;
    while (std::getline(in, line) && shown < 3)
        if (line.rfind("M73 P", 0) == 0) { std::printf("%s\n", line.c_str()); ++shown; }
}
