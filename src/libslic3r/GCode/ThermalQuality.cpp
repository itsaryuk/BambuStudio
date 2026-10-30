#include "ThermalQuality.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/ExtrusionEntity.hpp"

namespace Slic3r {

using MoveVertex = GCodeProcessorResult::MoveVertex;

namespace {

struct Cell
{
    float t{-1.f};     // deposit time
    float temp{0.f};   // temperature right after deposit
    float tau{1.f};    // cooling time constant at deposit
    int   layer{-1};
};

float env_temp(const ThermalModelParams& p, float z)
{
    float base = p.chamber_temp > 0.f ? std::max(p.chamber_temp, p.room_temp) : p.room_temp;
    return base + (p.bed_temp - base) * std::exp(-std::max(z, 0.f) / std::max(p.bed_air_decay_height, 1.f));
}

float cooling_tau(const ThermalModelParams& p, const MoveVertex& m)
{
    float h   = std::clamp(m.height > 0.f ? m.height : 0.2f, 0.05f, 1.f);
    float fan = std::clamp(m.fan_speed, 0.f, 100.f) / 100.f + 0.5f * std::clamp(m.additional_fan_speed, 0.f, 100.f) / 100.f;
    return p.cooling_time_const * (h / 0.2f) / (1.f + p.fan_cooling_gain * fan);
}

bool optimizable_role(ExtrusionRole r, bool keep_outer_walls)
{
    switch (r) {
    case erPerimeter:
    case erInternalInfill:
    case erSolidInfill: return true;
    case erExternalPerimeter: return !keep_outer_walls;
    default: return false;
    }
}

// Parses the code part of a G-code line (before any comment). Returns the position/length of the
// F parameter value, whether X/Y are present and the F value.
struct LineInfo
{
    bool   motion{false};  // G0/G1/G2/G3
    bool   has_xy{false};
    size_t f_pos{std::string::npos}, f_len{0};
    double f{0.};
};

LineInfo parse_line(const std::string& line)
{
    LineInfo info;
    size_t end = line.find(';');
    if (end == std::string::npos) end = line.size();
    size_t i = 0;
    while (i < end && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i + 1 >= end || line[i] != 'G') return info;
    char c = line[i + 1];
    if (c < '0' || c > '3') return info;
    if (i + 2 < end && line[i + 2] >= '0' && line[i + 2] <= '9') return info; // G10, G28, ...
    info.motion = true;
    i += 2;
    while (i < end) {
        char ch = line[i];
        if (ch == 'X' || ch == 'Y') info.has_xy = true;
        if (ch == 'F') {
            size_t s = i + 1;
            size_t e = s;
            while (e < end && line[e] != ' ' && line[e] != '\t') ++e;
            info.f_pos = s;
            info.f_len = e - s;
            info.f     = std::atof(line.substr(s, e - s).c_str());
        }
        ++i;
    }
    return info;
}

std::string format_tag(const ThermalMoveIndex& t)
{
    char buf[96];
    std::snprintf(buf, sizeof(buf), " %sti.max=%.2f,ti.min=%.2f,ti.mean=%.2f", ThermalQuality::Tag, t.max, t.min, t.mean);
    return buf;
}

} // namespace

namespace {

struct MaterialClass
{
    const char* name;
    float       midpoint;      // comfort midpoint when the profile has no vitrification temperature
    float       half_width;    // narrower for materials that warp or sag easily
    float       cooling_mult;  // > 1: semi-crystalline (latent heat keeps the layer warm longer)
    float       fan_gain;      // how much the part fan shortens the cooling time
    bool        warp_prone;
};

const std::map<std::string, MaterialClass>& material_classes()
{
    static const std::map<std::string, MaterialClass> classes = {
        {"PLA",  {"PLA", 45.f, 40.f, 1.0f, 1.5f, false}},
        {"PHA",  {"PLA", 45.f, 40.f, 1.0f, 1.5f, false}},
        {"PETG", {"PETG", 70.f, 35.f, 1.0f, 1.3f, false}},
        {"PET",  {"PETG", 70.f, 35.f, 1.0f, 1.3f, false}},
        {"PCTG", {"PETG", 75.f, 35.f, 1.0f, 1.3f, false}},
        {"ABS",  {"ABS", 100.f, 30.f, 1.0f, 1.5f, true}},
        {"HIPS", {"ABS", 95.f, 30.f, 1.0f, 1.5f, true}},
        {"ASA",  {"ASA", 100.f, 30.f, 1.0f, 1.5f, true}},
        {"PC",   {"PC", 125.f, 30.f, 1.0f, 1.5f, true}},
        {"PA",   {"PA", 110.f, 35.f, 1.2f, 1.4f, true}},
        {"PAHT", {"PAHT", 125.f, 30.f, 1.2f, 1.4f, true}},
        {"PPA",  {"PPA", 130.f, 30.f, 1.2f, 1.4f, true}},
        {"PPS",  {"PPS", 130.f, 30.f, 1.2f, 1.4f, true}},
        {"PP",   {"PP", 90.f, 25.f, 1.2f, 1.3f, true}},
        {"PE",   {"PE", 85.f, 25.f, 1.2f, 1.3f, true}},
        {"TPU",  {"TPU", 60.f, 50.f, 0.9f, 1.5f, false}},
        {"EVA",  {"TPU", 55.f, 50.f, 0.9f, 1.5f, false}},
        {"PVA",  {"PVA", 60.f, 40.f, 1.0f, 1.5f, false}},
        {"BVOH", {"PVA", 60.f, 40.f, 1.0f, 1.5f, false}},
        {"PEEK", {"PEEK", 180.f, 25.f, 1.2f, 1.2f, true}},
        {"PEKK", {"PEEK", 175.f, 25.f, 1.2f, 1.2f, true}},
        {"PEI",  {"PEI", 190.f, 25.f, 1.0f, 1.2f, true}},
    };
    return classes;
}

const MaterialClass* find_material_class(std::string type)
{
    boost::to_upper(type);
    const auto& classes = material_classes();
    auto        it      = classes.find(type);
    if (it != classes.end()) return &it->second;
    if (size_t dash = type.find('-'); dash != std::string::npos) { // PLA-CF, ASA-AERO, TPU-AMS
        type = type.substr(0, dash);
        if (it = classes.find(type); it != classes.end()) return &it->second;
    }
    while (!type.empty() && std::isdigit((unsigned char) type.back())) type.pop_back(); // PA6, PA12
    it = classes.find(type);
    return it != classes.end() ? &it->second : nullptr;
}

const ThermalModelParams& material_for(const ThermalMaterials& mats, unsigned char extruder_id)
{
    static const ThermalModelParams fallback;
    if (mats.empty()) return fallback;
    return extruder_id < mats.size() ? mats[extruder_id] : mats.front();
}

} // namespace

ThermalModelParams ThermalModelParams::for_material(const std::string& filament_type, float vitrification_temp)
{
    ThermalModelParams p;
    const MaterialClass* c = find_material_class(filament_type);
    p.material             = c != nullptr ? c->name : (filament_type.empty() ? "unknown" : filament_type);
    p.ideal_substrate_temp = vitrification_temp > 0.f ? vitrification_temp : (c != nullptr ? c->midpoint : 70.f);
    if (c != nullptr) {
        p.window_half_width  = c->half_width;
        p.cooling_time_const = 6.f * c->cooling_mult;
        p.fan_cooling_gain   = c->fan_gain;
        p.warp_prone         = c->warp_prone;
    }
    std::string upper = boost::to_upper_copy(filament_type);
    if (boost::contains(upper, "-CF") || boost::contains(upper, "-GF")) p.cooling_time_const *= 0.85f; // fillers conduct heat away faster
    return p;
}

void ThermalModelParams::calibrate_cooling_from_preset(float slow_down_layer_time, float fan_max_pct, float nozzle_temp, float env_temp)
{
    if (slow_down_layer_time <= 0.f || nozzle_temp <= 0.f) return;
    const float t0     = deposit_mix * nozzle_temp + (1.f - deposit_mix) * ideal_substrate_temp; // fresh line on a comfortable substrate
    const float target = ideal_substrate_temp + 0.8f * window_half_width;                      // just below "hot" when the next layer lands
    if (t0 <= target + 1.f || target <= env_temp + 1.f) return;
    const float tau_fan = slow_down_layer_time / std::log((t0 - env_temp) / (target - env_temp));
    const float fan     = std::clamp(fan_max_pct, 0.f, 100.f) / 100.f;
    cooling_time_const  = std::clamp(tau_fan * (1.f + fan_cooling_gain * fan), 1.f, 60.f);
}

bool ThermalGCodeText::load(const std::string& path, ThermalGCodeText& out)
{
    boost::nowide::ifstream in(path.c_str(), std::ios::in | std::ios::binary);
    if (!in.good()) return false;
    out.lines.clear();
    out.line_layers.clear();
    out.line_layers.push_back(-1); // 1-based
    int layer = -1;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (boost::starts_with(line, "; CHANGE_LAYER")) ++layer;
        out.lines.push_back(line);
        out.line_layers.push_back(layer);
    }
    return true;
}

std::vector<int> ThermalQuality::layers_of_moves(const GCodeProcessorResult& result, const std::vector<int>& line_layers)
{
    std::vector<int> layers(result.moves.size(), -1);
    for (size_t i = 0; i < result.moves.size(); ++i) {
        unsigned int id = result.moves[i].gcode_id;
        layers[i] = id < line_layers.size() ? line_layers[id] : -1;
    }
    return layers;
}

bool ThermalQuality::eligible(const MoveVertex& m, int layer, const ThermalOptimizeOptions& o)
{
    return m.type == EMoveType::Extrude && layer >= std::max(1, o.first_layer) && (o.last_layer < 0 || layer <= o.last_layer) &&
           optimizable_role(m.extrusion_role, o.keep_outer_walls);
}

ThermalReport ThermalQuality::simulate(const GCodeProcessorResult&   result,
                                       const std::vector<int>&       move_layers,
                                       const ThermalMaterials&       materials,
                                       const std::vector<float>*     layer_speed_factors,
                                       const ThermalOptimizeOptions* eligibility)
{
    ThermalReport             rep;
    const ThermalModelParams& params = material_for(materials, 0); // bed, chamber and grid settings are shared
    std::vector<bool>         used_slot(std::max<size_t>(materials.size(), 1), false);
    const auto& moves = result.moves;
    rep.moves.assign(moves.size(), {});

    // Grid over the printed footprint: an extrusion spans from the previous move's end to its own.
    float min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;
    int   n_layers = 0;
    auto  grow = [&](const Vec3f& p) {
        min_x = std::min(min_x, p.x()); max_x = std::max(max_x, p.x());
        min_y = std::min(min_y, p.y()); max_y = std::max(max_y, p.y());
    };
    for (size_t i = 1; i < moves.size(); ++i) {
        if (moves[i].type != EMoveType::Extrude) continue;
        grow(moves[i - 1].position);
        grow(moves[i].position);
        for (const Vec3f& p : moves[i].interpolation_points) grow(p);
        n_layers = std::max(n_layers, move_layers[i] + 1);
    }
    if (n_layers == 0) { rep.outcome = "WILL_PRINT"; rep.direction = "NONE"; return rep; }

    const float cell = std::max(params.cell_size, 0.25f);
    min_x -= 2 * cell; min_y -= 2 * cell;
    const int nx = int((max_x - min_x) / cell) + 4;
    const int ny = int((max_y - min_y) / cell) + 4;
    std::vector<Cell> cells(size_t(nx) * size_t(ny));
    auto cell_index = [&](float x, float y, int& cx, int& cy) {
        cx = std::clamp(int((x - min_x) / cell), 0, nx - 1);
        cy = std::clamp(int((y - min_y) / cell), 0, ny - 1);
    };

    std::vector<double> layer_sum(n_layers, 0.), layer_time(n_layers, 0.), layer_temp(n_layers, 0.);
    double sum = 0., sum2 = 0., total = 0., hot = 0., cold = 0.;

    double now = 0.;
    for (size_t i = 1; i < moves.size(); ++i) {
        const MoveVertex& m = moves[i];
        const int L = move_layers[i];
        double dt = std::max(0.f, m.time[0] - moves[i - 1].time[0]);
        if (layer_speed_factors != nullptr && eligibility != nullptr && L >= 0 && L < int(layer_speed_factors->size()) &&
            eligible(m, L, *eligibility))
            dt /= std::max((*layer_speed_factors)[L], 0.05f);
        const double t_start = now;
        now += dt;
        if (m.type != EMoveType::Extrude || L < 0) continue;

        // Polyline of this move.
        std::vector<Vec3f> pts;
        pts.push_back(moves[i - 1].position);
        if (m.is_arc_move_with_interpolation_points())
            pts.insert(pts.end(), m.interpolation_points.begin(), m.interpolation_points.end());
        pts.push_back(m.position);
        float total_len = 0.f;
        for (size_t k = 1; k < pts.size(); ++k) total_len += (pts[k].head<2>() - pts[k - 1].head<2>()).norm();

        const ThermalModelParams& mat = material_for(materials, m.extruder_id);
        if (m.extruder_id < used_slot.size()) used_slot[m.extruder_id] = true;
        const float mid = mat.ideal_substrate_temp, half = std::max(mat.window_half_width, 1.f);
        const float t_env   = env_temp(params, m.position.z());
        const float tau     = cooling_tau(mat, m);
        const float t_noz   = m.temperature > 0.f ? m.temperature : 200.f;
        float       mn = 1e9f, mx = -1e9f;
        double      acc = 0., acc_temp = 0.;
        int         cnt = 0;
        auto        decayed   = [&](const Cell& k, float t_s) { return t_env + (k.temp - t_env) * std::exp(-std::max(t_s - k.t, 0.f) / k.tau); };
        auto        deposited = [&](const Cell& k) { return k.layer >= 0 && (k.layer == L || k.layer == L - 1); };
        float       walked = 0.f;
        for (size_t k = 1; k < pts.size(); ++k) {
            const Vec2f a = pts[k - 1].head<2>(), b = pts[k].head<2>();
            const float len = (b - a).norm();
            const int   n   = std::max(1, int(std::ceil(len / cell)));
            for (int s = 1; s <= n; ++s) {
                const float frac = float(s) / float(n);
                const Vec2f p    = a + (b - a) * frac;
                const float t_s  = float(t_start + (total_len > 0.f ? (walked + len * frac) / total_len : 1.f) * dt);
                int cx, cy;
                cell_index(p.x(), p.y(), cx, cy);
                Cell& c = cells[size_t(cy) * nx + cx];
                float t_sub;
                if (deposited(c)) {
                    // Lateral conduction: a neighbour counts once heat laid there has had time to cross the distance.
                    double acc_t = 2. * decayed(c, t_s); // the spot itself counts double
                    int    cnt_t = 2;
                    for (int dy = -2; dy <= 2; ++dy)
                        for (int dx = -2; dx <= 2; ++dx) {
                            if (dx == 0 && dy == 0) continue;
                            const int x = cx + dx, y = cy + dy;
                            if (x < 0 || y < 0 || x >= nx || y >= ny) continue;
                            const Cell& k = cells[size_t(y) * nx + x];
                            if (!deposited(k)) continue;
                            if (std::sqrt(mat.diffusivity * std::max(t_s - k.t, 0.f)) < std::hypot(float(dx), float(dy)) * cell) continue;
                            acc_t += decayed(k, t_s);
                            ++cnt_t;
                        }
                    t_sub = float(acc_t / cnt_t);
                } else
                    t_sub = (L == 0) ? params.bed_temp : t_env; // bed, or air under an overhang/bridge
                const float idx = std::clamp((t_sub - mid) / half, -1.f, 1.f);
                mn = std::min(mn, idx); mx = std::max(mx, idx); acc += idx; acc_temp += t_sub; ++cnt;
                c.t = t_s; c.temp = mat.deposit_mix * t_noz + (1.f - mat.deposit_mix) * t_sub; c.tau = tau; c.layer = L;
            }
            walked += len;
        }
        ThermalMoveIndex& out = rep.moves[i];
        out.min = mn; out.max = mx; out.mean = float(acc / std::max(cnt, 1));
        layer_sum[L] += out.mean * dt; layer_time[L] += dt; layer_temp[L] += acc_temp / std::max(cnt, 1) * dt;
        sum += out.mean * dt; sum2 += double(out.mean) * out.mean * dt; total += dt;
        if (out.mean > 0.8f) hot += dt;
        if (out.mean < -0.8f) cold += dt;
    }

    rep.layer_mean.resize(n_layers);
    rep.layer_substrate_temp.resize(n_layers);
    for (int L = 0; L < n_layers; ++L) {
        rep.layer_mean[L]           = layer_time[L] > 0. ? float(layer_sum[L] / layer_time[L]) : 0.f;
        rep.layer_substrate_temp[L] = layer_time[L] > 0. ? float(layer_temp[L] / layer_time[L]) : params.room_temp;
    }
    if (total > 0.) {
        rep.mean          = float(sum / total);
        rep.stddev        = float(std::sqrt(std::max(sum2 / total - double(rep.mean) * rep.mean, 0.)));
        rep.hot_fraction  = float(hot / total);
        rep.cold_fraction = float(cold / total);
    }

    const float bad = rep.hot_fraction + rep.cold_fraction;
    rep.outcome   = bad < 0.05f ? "WILL_PRINT" : (bad < 0.25f ? "MAY_PRINT" : "LIKELY_FAIL");
    rep.direction = bad < 0.05f ? "NONE" : (rep.cold_fraction > rep.hot_fraction ? "OVERCOOLING" : "OVERHEATING");

    // Contiguous runs of hot / cold layers.
    auto runs = [&](bool hot_runs) {
        int start = -1;
        for (int L = 0; L <= n_layers; ++L) {
            bool in = L < n_layers && layer_time[L] > 0. && (hot_runs ? rep.layer_mean[L] > 0.6f : rep.layer_mean[L] < -0.6f);
            if (in && start < 0) start = L;
            if (!in && start >= 0) {
                char buf[256];
                if (hot_runs)
                    std::snprintf(buf, sizeof(buf), "Layers %d-%d run hot: previous layer is still soft when the next one lands (sagging, loss of detail, stringing).", start + 1, L);
                else
                    std::snprintf(buf, sizeof(buf), "Layers %d-%d run cold: previous layer has fully cooled before the next one lands (weak bonding, warping).", start + 1, L);
                rep.caveats.emplace_back(buf);
                start = -1;
            }
        }
    };
    runs(true);
    runs(false);

    {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "Environment: bed %.0f °C, chamber air %s, bed-warmed air fades over %.0f mm.", params.bed_temp,
                      params.chamber_temp > 0.f ? (std::to_string(int(std::lround(params.chamber_temp))) + " °C").c_str() : "at room temperature",
                      params.bed_air_decay_height);
        rep.assumptions.emplace_back(buf);
    }
    bool warp = false;
    for (size_t slot = 0; slot < used_slot.size(); ++slot) {
        if (!used_slot[slot]) continue;
        const ThermalModelParams& mat = material_for(materials, (unsigned char) slot);
        warp |= mat.warp_prone;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "Filament %zu: %s model, comfort window %.0f ± %.0f °C, cooling constant %.1f s.", slot + 1,
                      mat.material.c_str(), mat.ideal_substrate_temp, mat.window_half_width, mat.cooling_time_const);
        rep.assumptions.emplace_back(buf);
    }

    if (rep.direction == "OVERHEATING") {
        if (warp)
            rep.fixes.emplace_back("QUICK", "Raise the minimum layer time rather than the part fan: this material warps when fan-cooled, it only needs time to set.");
        else
            rep.fixes.emplace_back("QUICK", "Raise the part cooling fan or the minimum layer time so small layers can cool before the next one lands.");
        rep.fixes.emplace_back("ADVANCED", "Print several parts at once or add a sacrificial tower so every layer gets more time to cool.");
        rep.fixes.emplace_back("EXPERT", "Lower the nozzle temperature a few degrees, or reduce wall count on fine features to shed heat faster.");
    } else if (rep.direction == "OVERCOOLING") {
        if (warp) {
            rep.fixes.emplace_back("QUICK", "Close the enclosure, raise the chamber temperature and turn the part fan down: this material needs a warm substrate to bond and stay flat.");
            rep.fixes.emplace_back("ADVANCED", "Raise the bed temperature, add a brim and avoid drafts; large cold layers are where warping starts.");
        } else {
            rep.fixes.emplace_back("QUICK", "Reduce the part cooling fan so layers stay warm a little longer.");
            rep.fixes.emplace_back("ADVANCED", "Raise the bed temperature a few degrees; this material tolerates cold layers but bonds better when warm.");
        }
        rep.fixes.emplace_back("EXPERT", "Run Optimize: faster large layers lose less heat between passes.");
    } else {
        rep.fixes.emplace_back("QUICK", "Thermal conditions look balanced. Optimize can trade the spare cooling margin for print time.");
    }
    return rep;
}

std::vector<float> ThermalQuality::optimize(const GCodeProcessorResult&   result,
                                            const std::vector<int>&       move_layers,
                                            const ThermalMaterials&       materials,
                                            const ThermalOptimizeOptions& options)
{
    int n_layers = 0;
    for (int L : move_layers) n_layers = std::max(n_layers, L + 1);
    std::vector<float> factors(n_layers, 1.f), cap(n_layers, options.max_speed_factor), floor(n_layers, options.min_speed_factor);
    std::vector<bool>  touched(n_layers, false);
    for (size_t i = 0; i < result.moves.size(); ++i) {
        const auto& m = result.moves[i];
        const int   L = move_layers[i];
        if (L < 0 || !eligible(m, L, options)) continue;
        touched[L] = true;
        const ThermalModelParams& mat     = material_for(materials, m.extruder_id);
        const float max_vol = options.max_volumetric > 0.f ? options.max_volumetric : mat.max_volumetric; // dialog overrides the profile
        if (options.max_speed > 0.f && m.feedrate > 0.f) cap[L] = std::min(cap[L], options.max_speed / m.feedrate);
        if (max_vol > 0.f && m.volumetric_rate() > 0.f) cap[L] = std::min(cap[L], max_vol / m.volumetric_rate());
        if (mat.min_speed > 0.f && m.feedrate > 0.f) floor[L] = std::max(floor[L], std::min(1.f, mat.min_speed / m.feedrate)); // never below the profile slow-down floor
    }
    for (int L = 0; L < n_layers; ++L) cap[L] = std::max(cap[L], floor[L]);

    for (int pass = 0; pass < std::max(options.passes, 1); ++pass) {
        ThermalReport rep = simulate(result, move_layers, materials, &factors, &options);
        for (int L = 0; L < n_layers; ++L) {
            if (!touched[L]) continue;
            const float m = rep.layer_mean[L];
            if (m < -0.3f)
                factors[L] *= 1.f + 0.5f * (-0.3f - m);
            else if (m > 0.3f)
                factors[L] *= 1.f - 0.5f * (m - 0.3f);
            factors[L] = std::clamp(factors[L], floor[L], cap[L]);
        }
    }
    return factors;
}

float ThermalQuality::time_factor(const GCodeProcessorResult& result, const std::vector<int>& move_layers,
                                  const std::vector<float>& layer_speed_factors, const ThermalOptimizeOptions& options)
{
    double before = 0., after = 0.;
    for (size_t i = 1; i < result.moves.size(); ++i) {
        const auto& m = result.moves[i];
        const int   L = move_layers[i];
        if (L < 0 || L >= int(layer_speed_factors.size()) || !eligible(m, L, options)) continue;
        const double dt = std::max(0.f, m.time[0] - result.moves[i - 1].time[0]);
        before += dt;
        after += dt / std::max(layer_speed_factors[L], 0.05f);
    }
    return before > 0. ? float(after / before) : 1.f;
}

bool ThermalQuality::write_gcode(const ThermalGCodeText&       text,
                                 const std::string&            dst,
                                 const GCodeProcessorResult&   result,
                                 const std::vector<int>&       move_layers,
                                 const ThermalReport&          report,
                                 const std::vector<float>*     layer_speed_factors,
                                 const ThermalOptimizeOptions* options)
{
    // line -> extrusion move producing it
    std::vector<int> line_move(text.lines.size() + 1, -1);
    for (size_t i = 0; i < result.moves.size(); ++i) {
        const auto& m = result.moves[i];
        if (m.type == EMoveType::Extrude && m.gcode_id < line_move.size()) line_move[m.gcode_id] = int(i);
    }
    std::vector<LineInfo> infos(text.lines.size());
    for (size_t i = 0; i < text.lines.size(); ++i) infos[i] = parse_line(text.lines[i]);

    auto factor_for_line = [&](size_t idx1) -> float { // 1-based line; 1 = unchanged
        if (layer_speed_factors == nullptr || options == nullptr) return 1.f;
        int mv = line_move[idx1];
        if (mv < 0) return 1.f;
        int L = move_layers[mv];
        if (L < 0 || L >= int(layer_speed_factors->size()) || !eligible(result.moves[mv], L, *options)) return 1.f;
        return (*layer_speed_factors)[L];
    };

    // Rescaled cumulative time per move, so M73 progress lines can be recomputed after optimization.
    const bool          rescaled = layer_speed_factors != nullptr && options != nullptr;
    std::vector<double> scaled(result.moves.size(), 0.);
    std::vector<int>    line_any_move(text.lines.size() + 1, -1);
    for (size_t i = 1; i < result.moves.size(); ++i) {
        const auto& m  = result.moves[i];
        const int   L  = move_layers[i];
        double      dt = std::max(0.f, m.time[0] - result.moves[i - 1].time[0]);
        if (rescaled && L >= 0 && L < int(layer_speed_factors->size()) && eligible(m, L, *options)) dt /= std::max((*layer_speed_factors)[L], 0.05f);
        scaled[i] = scaled[i - 1] + dt;
        if (m.gcode_id < line_any_move.size()) line_any_move[m.gcode_id] = int(i);
    }
    const double total_time = scaled.empty() ? 0. : scaled.back();
    int          last_move  = 0;

    boost::nowide::ofstream out(dst.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out.good()) return false;
    for (size_t i = 0; i < text.lines.size(); ++i) {
        const size_t    idx1 = i + 1;
        const LineInfo& info = infos[i];
        std::string     line = text.lines[i];
        if (line_any_move[idx1] > last_move) last_move = line_any_move[idx1];
        if (rescaled && total_time > 0. && boost::starts_with(line, "M73 P") && line.find(" R") != std::string::npos) {
            const double elapsed = scaled[last_move];
            char         buf[48];
            std::snprintf(buf, sizeof(buf), "M73 P%d R%d", int(std::clamp(elapsed / total_time * 100., 0., 100.)), int((total_time - elapsed + 30.) / 60.));
            line = buf;
        }
        if (info.motion && info.f_pos != std::string::npos && info.f > 0.) {
            // An F-only line applies to the next XY move; find it.
            size_t target = idx1;
            if (!info.has_xy) {
                target = 0;
                for (size_t j = i + 1; j < text.lines.size() && j < i + 64; ++j)
                    if (infos[j].motion && infos[j].has_xy) { target = j + 1; break; }
            }
            const float f = target > 0 ? factor_for_line(target) : 1.f;
            if (std::abs(f - 1.f) > 1e-4f) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%ld", std::lround(info.f * f));
                line.replace(info.f_pos, info.f_len, buf);
            }
        }
        if (line_move[idx1] >= 0 && line_move[idx1] < int(report.moves.size())) line += format_tag(report.moves[line_move[idx1]]);
        out << line << '\n';
    }
    return out.good();
}

std::string ThermalQuality::quality_grade(float before, float after)
{
    const float r = (before - after) / std::max(before, 1e-3f);
    return r > 0.3f ? "HIGH" : (r > 0.1f ? "MEDIUM" : "LOW");
}

} // namespace Slic3r
