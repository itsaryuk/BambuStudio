#include "ThermalQuality.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/nowide/fstream.hpp>

#include "libslic3r/ExtrusionEntity.hpp"

namespace Slic3r {

using MoveVertex = GCodeProcessorResult::MoveVertex;

namespace {

struct Deposit
{
    float t{-1.f};     // deposit time
    float temp{0.f};   // temperature right after deposit
    float tau{1.f};    // cooling time constant at deposit
    int   layer{-1};
};

// The latest deposit and the one from the layer before it: a second wall laid beside the first in the same cell
// must not pass for the substrate underneath.
struct Cell
{
    Deposit cur, prev;
    void    put(const Deposit& d)
    {
        if (cur.layer != d.layer) prev = cur;
        cur = d;
    }
    const Deposit* below(int L) const
    {
        if (L < 1) return nullptr; // the bed is handled by the caller
        return cur.layer == L ? (prev.layer == L - 1 ? &prev : nullptr) : (cur.layer == L - 1 ? &cur : nullptr);
    }
    const Deposit* beside(int L) const { return cur.layer >= 0 && (cur.layer == L || cur.layer == L - 1) ? &cur : nullptr; }
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
    bool   has_e{false};
    bool   has_x{false}, has_y{false}, has_z{false};
    float  x{0.f}, y{0.f}, z{0.f}, e{0.f};
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
        if (ch == 'X' || ch == 'Y' || ch == 'Z' || ch == 'E') {
            const float v = std::atof(line.c_str() + i + 1);
            if (ch == 'X') { info.has_x = true; info.x = v; }
            if (ch == 'Y') { info.has_y = true; info.y = v; }
            if (ch == 'Z') { info.has_z = true; info.z = v; }
            if (ch == 'E') { info.has_e = true; info.e = v; }
        }
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
    // Compact on purpose: printer firmware has a line buffer, and an arc move plus a long comment can exceed it.
    std::snprintf(buf, sizeof(buf), " %s%.2f,%.2f,%.2f", ThermalQuality::Tag, t.max, t.min, t.mean);
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
    auto value_of = [](const std::string& l, const char* key) -> const char* { // "; key = value" -> value, or nullptr
        const std::string prefix = std::string("; ") + key + " = ";
        return boost::starts_with(l, prefix) ? l.c_str() + prefix.size() : nullptr;
    };
    auto first_number = [](const char* v, float& dst) { // "3,3" or "nil" (per-filament lists)
        char* end = nullptr;
        const float f = std::strtof(v, &end);
        if (end != v) dst = f;
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (boost::starts_with(line, "; CHANGE_LAYER")) ++layer;
        if (line.size() > 2 && line[0] == ';' && line[1] == ' ') {
            if (const char* v = value_of(line, "printable_area")) {
                out.printable_area.clear();
                std::vector<std::string> points;
                boost::split(points, std::string(v), boost::is_any_of(","));
                for (const std::string& pt : points) { // "0x0,340x0,340x320,0x320"; strtof would read "0x0" as hex, so split first
                    const size_t x_at = pt.find('x');
                    if (x_at == std::string::npos) continue;
                    out.printable_area.emplace_back(float(std::atof(pt.substr(0, x_at).c_str())), float(std::atof(pt.substr(x_at + 1).c_str())));
                }
            } else if (const char* v = value_of(line, "use_relative_e_distances")) out.relative_e = v[0] == '1';
            else if (const char* v = value_of(line, "retraction_length")) first_number(v, out.retraction_length);
            else if (const char* v = value_of(line, "filament_retraction_length")) first_number(v, out.retraction_length); // overrides when numeric
            else if (const char* v = value_of(line, "retraction_speed")) first_number(v, out.retraction_speed);
            else if (const char* v = value_of(line, "travel_speed")) first_number(v, out.travel_speed);
        }
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

std::vector<double> ThermalQuality::move_durations(const GCodeProcessorResult& result, const std::vector<int>& move_layers)
{
    const auto&         moves = result.moves;
    std::vector<double> g(moves.size(), 0.);
    int                 n_layers = 0;
    for (int L : move_layers) n_layers = std::max(n_layers, L + 1);
    std::vector<double> geo(n_layers, 0.), dur(n_layers, 0.);
    for (size_t i = 1; i < moves.size(); ++i) {
        const MoveVertex& m   = moves[i];
        float             len = 0.f;
        Vec3f             prev = moves[i - 1].position;
        for (const Vec3f& p : m.interpolation_points) { len += (p - prev).norm(); prev = p; }
        len += (m.position - prev).norm();
        if (m.feedrate > 0.f) g[i] = len / m.feedrate;
        const int L = move_layers[i];
        if (L >= 0) { geo[L] += g[i]; dur[L] = std::max(dur[L], double(m.layer_duration)); }
    }
    double last_t = 0.;
    for (size_t i = 1; i < moves.size(); ++i) {
        const int    L = move_layers[i];
        const double t = moves[i].time[0] > 0.f ? double(moves[i].time[0]) : last_t;
        const double clock_dt = std::max(0., t - last_t);
        last_t                = t;
        if (L >= 0 && dur[L] > 0. && geo[L] > 0.) g[i] *= dur[L] / geo[L];
        else g[i] = clock_dt;
    }
    return g;
}

ThermalReport ThermalQuality::simulate(const GCodeProcessorResult&   result,
                                       const std::vector<int>&       move_layers,
                                       const ThermalMaterials&       materials,
                                       const std::vector<float>*     layer_speed_factors,
                                       const ThermalOptimizeOptions* eligibility,
                                       const std::vector<float>*     layer_pauses)
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
    std::vector<double> layer_tau_sum(n_layers, 0.), layer_env_sum(n_layers, 0.), layer_edge_sum(n_layers, 0.);
    std::vector<float>  layer_zmax(n_layers, 0.f);
    int                 prev_L = -1;
    double sum = 0., sum2 = 0., total = 0., hot = 0., cold = 0.;

    const std::vector<double> durations = move_durations(result, move_layers);
    double now = 0.;
    for (size_t i = 1; i < moves.size(); ++i) {
        const MoveVertex& m = moves[i];
        const int L = move_layers[i];
        double dt = durations[i];
        if (layer_speed_factors != nullptr && eligibility != nullptr && L >= 0 && L < int(layer_speed_factors->size()) &&
            eligible(m, L, *eligibility))
            dt /= std::max((*layer_speed_factors)[L], 0.05f);
        if (L != prev_L) { // a planned wait after the previous layer: the part cools, nothing is laid
            if (prev_L >= 0 && layer_pauses != nullptr && prev_L < int(layer_pauses->size())) now += (*layer_pauses)[prev_L];
            prev_L = L;
        }
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
        float       mn = 1e9f, mx = -1e9f, mn_all = 1e9f, mx_all = -1e9f;
        double      acc = 0., acc_temp = 0., acc_all = 0.;
        int         cnt = 0, cnt_all = 0; // cnt: samples with a substrate (previous layer or bed); air under a crossing or bridge is not rated
        auto        decayed   = [&](const Deposit& k, float t_s) { return t_env + (k.temp - t_env) * std::exp(-std::max(t_s - k.t, 0.f) / k.tau); };
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
                bool  substrate = true;
                if (const Deposit* under = c.below(L); under != nullptr) {
                    // Lateral conduction: a neighbour counts once heat laid there has had time to cross the distance.
                    double acc_t = 2. * decayed(*under, t_s); // the spot itself counts double
                    int    cnt_t = 2;
                    for (int dy = -2; dy <= 2; ++dy)
                        for (int dx = -2; dx <= 2; ++dx) {
                            if (dx == 0 && dy == 0) continue;
                            const int x = cx + dx, y = cy + dy;
                            if (x < 0 || y < 0 || x >= nx || y >= ny) continue;
                            const Deposit* k = cells[size_t(y) * nx + x].beside(L);
                            if (k == nullptr) continue;
                            if (std::sqrt(mat.diffusivity * std::max(t_s - k->t, 0.f)) < std::hypot(float(dx), float(dy)) * cell) continue;
                            acc_t += decayed(*k, t_s);
                            ++cnt_t;
                        }
                    t_sub = float(acc_t / cnt_t);
                } else {
                    t_sub     = (L == 0) ? params.bed_temp : t_env; // bed, or air under an overhang, bridge or sparse-infill crossing
                    substrate = L == 0;
                }
                const float idx = std::clamp((t_sub - mid) / half, -1.f, 1.f);
                mn_all = std::min(mn_all, idx); mx_all = std::max(mx_all, idx); acc_all += idx; ++cnt_all;
                if (substrate) { mn = std::min(mn, idx); mx = std::max(mx, idx); acc += idx; acc_temp += t_sub; ++cnt; }
                c.put(Deposit{t_s, mat.deposit_mix * t_noz + (1.f - mat.deposit_mix) * t_sub, tau, L});
            }
            walked += len;
        }
        ThermalMoveIndex& out = rep.moves[i];
        if (cnt == 0) { // nothing underneath: tag it for the preview, but it carries no weight in the statistics
            out.min = mn_all; out.max = mx_all; out.mean = float(acc_all / std::max(cnt_all, 1));
            continue;
        }
        out.min = mn; out.max = mx; out.mean = float(acc / cnt);
        layer_sum[L] += out.mean * dt; layer_time[L] += dt; layer_temp[L] += acc_temp / cnt * dt;
        layer_tau_sum[L] += tau * dt; layer_env_sum[L] += t_env * dt; layer_edge_sum[L] += (mid + 0.8f * half) * dt;
        layer_zmax[L] = std::max(layer_zmax[L], m.position.z());
        sum += out.mean * dt; sum2 += double(out.mean) * out.mean * dt; total += dt;
        if (out.mean > 0.8f) hot += dt;
        if (out.mean < -0.8f) cold += dt;
    }

    rep.layer_mean.resize(n_layers);
    rep.layer_substrate_temp.resize(n_layers);
    rep.layer_z.resize(n_layers);
    rep.layer_tau.resize(n_layers);
    rep.layer_env.resize(n_layers);
    rep.layer_hot_edge.resize(n_layers);
    for (int L = 0; L < n_layers; ++L) {
        const bool have = layer_time[L] > 0.;
        rep.layer_mean[L]           = have ? float(layer_sum[L] / layer_time[L]) : 0.f;
        rep.layer_substrate_temp[L] = have ? float(layer_temp[L] / layer_time[L]) : params.room_temp;
        rep.layer_z[L]              = layer_zmax[L];
        rep.layer_tau[L]            = have ? float(layer_tau_sum[L] / layer_time[L]) : params.cooling_time_const;
        rep.layer_env[L]            = have ? float(layer_env_sum[L] / layer_time[L]) : params.room_temp;
        rep.layer_hot_edge[L]       = have ? float(layer_edge_sum[L] / layer_time[L]) : params.ideal_substrate_temp + 0.8f * params.window_half_width;
        if (have && rep.layer_mean[L] > 0.6f) {
            if (rep.hot_bottom_z < 0.f) rep.hot_bottom_z = layer_zmax[L];
            rep.hot_top_z = layer_zmax[L];
        }
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
        rep.fixes.emplace_back("ADVANCED", "Let Optimize pause away from the part on layers that cannot cool in time, or add a cooling tower next to the part so every small layer gets more time.");
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
                                            const ThermalOptimizeOptions& options,
                                            std::vector<float>*           layer_pauses)
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
        // No profile slow-down floor: the slicer already holds that speed on short layers, and a layer still hot there
        // has to go slower (the user's slow-down limit bounds it). Layers that are not hot are never slowed at all.
    }
    for (int L = 0; L < n_layers; ++L) cap[L] = std::max(cap[L], floor[L]);

    // Aim every layer just below the hot edge (index 0.8): cooler layers print faster until they warm into the band or hit
    // the speed/flow cap, hotter ones slow down. Fully cooled layers stay cool however fast they go, so they reach the cap.
    constexpr float band_lo = 0.3f, band_hi = 0.5f; // index 0.3 (≈12 °C for PLA) of margin under the hot edge
    for (int pass = 0; pass < std::max(options.passes, 1); ++pass) {
        ThermalReport rep = simulate(result, move_layers, materials, &factors, &options);
        for (int L = 0; L < n_layers; ++L) {
            if (!touched[L]) continue;
            // Unclamped temperature in index units: the index saturates at +1, the excess heat does not.
            const float m = 0.8f + (rep.layer_substrate_temp[L] - rep.layer_hot_edge[L]) / std::max(material_for(materials, 0).window_half_width, 1.f);
            if (m < band_lo)
                factors[L] *= 1.f + std::max(0.15f, band_lo - m);
            else if (m > band_hi)
                factors[L] *= 1.f - std::min(0.5f, std::max(0.1f, 0.5f * (m - band_hi)));
            factors[L] = std::clamp(factors[L], floor[L], cap[L]);
        }
    }
    if (layer_pauses != nullptr) {
        layer_pauses->assign(n_layers, 0.f);
        if (options.max_pause > 0.f)
            for (int pass = 0; pass < 3; ++pass) { // the model is not quite exponential (conduction, bed air), so refine
                ThermalReport rep = simulate(result, move_layers, materials, &factors, &options, layer_pauses);
                for (int L = 1; L < n_layers; ++L) {
                    if (rep.layer_mean[L] <= 0.3f) continue; // still hot after slowing down: wait after the layer below
                    const float T = rep.layer_substrate_temp[L], target = rep.layer_hot_edge[L], env = rep.layer_env[L - 1], tau = rep.layer_tau[L - 1];
                    if (T <= target || target <= env || tau <= 0.f) continue;
                    const float extra        = tau * std::log((T - env) / (target - env)); // Newton cooling from T down to the hot edge
                    (*layer_pauses)[L - 1]   = std::clamp((*layer_pauses)[L - 1] + extra, 0.f, options.max_pause);
                }
            }
        // The park trip itself (retract, lift, travel out and back) takes about a second: a shorter wait is not worth a mark.
        for (float& p : *layer_pauses)
            if (p < 0.5f) p = 0.f;
    }
    return factors;
}

float ThermalQuality::time_factor(const GCodeProcessorResult& result, const std::vector<int>& move_layers,
                                  const std::vector<float>& layer_speed_factors, const ThermalOptimizeOptions& options,
                                  const std::vector<float>* layer_pauses)
{
    double before = 0., after = 0.;
    if (layer_pauses != nullptr) for (float p : *layer_pauses) after += p;
    const std::vector<double> durations = move_durations(result, move_layers);
    for (size_t i = 1; i < result.moves.size(); ++i) {
        const auto& m = result.moves[i];
        const int   L = move_layers[i];
        if (L < 0 || L >= int(layer_speed_factors.size()) || !eligible(m, L, options)) continue;
        const double dt = durations[i];
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
                                 const ThermalOptimizeOptions* options,
                                 const std::vector<float>*     layer_pauses)
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
    const std::vector<double> durations = move_durations(result, move_layers);
    int                       prev_L    = -1;
    for (size_t i = 1; i < result.moves.size(); ++i) {
        const auto& m  = result.moves[i];
        const int   L  = move_layers[i];
        double      dt = durations[i];
        if (L != prev_L) {
            if (prev_L >= 0 && layer_pauses != nullptr && prev_L < int(layer_pauses->size())) dt += (*layer_pauses)[prev_L];
            prev_L = L;
        }
        if (rescaled && L >= 0 && L < int(layer_speed_factors->size()) && eligible(m, L, *options)) dt /= std::max((*layer_speed_factors)[L], 0.05f);
        scaled[i] = scaled[i - 1] + dt;
        if (m.gcode_id < line_any_move.size()) line_any_move[m.gcode_id] = int(i);
    }
    const double total_time = scaled.empty() ? 0. : scaled.back();
    int          last_move  = 0;

    // Cooling pauses: where the head parks. Outside the footprint of everything printed, inside the bed.
    float bb_min_x = 1e9f, bb_min_y = 1e9f, bb_max_x = -1e9f, bb_max_y = -1e9f;
    for (const LineInfo& li : infos)
        if (li.motion && li.has_xy && li.has_e && li.e > 0.f && li.has_x && li.has_y) {
            bb_min_x = std::min(bb_min_x, li.x); bb_max_x = std::max(bb_max_x, li.x);
            bb_min_y = std::min(bb_min_y, li.y); bb_max_y = std::max(bb_max_y, li.y);
        }
    auto inside_bed = [&](float x, float y) { // even-odd test with a 3 mm margin, true when the outline is unknown
        if (text.printable_area.size() < 3) return false;
        for (const Vec2f d : {Vec2f(3.f, 0.f), Vec2f(-3.f, 0.f), Vec2f(0.f, 3.f), Vec2f(0.f, -3.f)}) {
            const float px = x + d.x(), py = y + d.y();
            bool in = false;
            for (size_t a = 0, b = text.printable_area.size() - 1; a < text.printable_area.size(); b = a++) {
                const Vec2f& p = text.printable_area[a];
                const Vec2f& q = text.printable_area[b];
                if ((p.y() > py) != (q.y() > py) && px < (q.x() - p.x()) * (py - p.y()) / (q.y() - p.y()) + p.x()) in = !in;
            }
            if (!in) return false;
        }
        return true;
    };
    auto park_point = [&](float cx, float cy, float& px, float& py) {
        if (bb_min_x > bb_max_x) return false;
        const float gap = 20.f;
        const float cand[4][2] = {{cx, bb_min_y - gap}, {cx, bb_max_y + gap}, {bb_min_x - gap, cy}, {bb_max_x + gap, cy}};
        for (const auto& c : cand)
            if (inside_bed(c[0], c[1])) { px = c[0]; py = c[1]; return true; }
        return false;
    };
    float cur_x = 0.f, cur_y = 0.f, cur_z = -1.f;
    bool  have_xy = false;
    const float retract = text.relative_e ? std::min(text.retraction_length, 2.f) : 0.f;

    boost::nowide::ofstream out(dst.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out.good()) return false;
    for (size_t i = 0; i < text.lines.size(); ++i) {
        const size_t    idx1 = i + 1;
        const LineInfo& info = infos[i];
        std::string     line = text.lines[i];
        if (layer_pauses != nullptr && boost::starts_with(line, "; CHANGE_LAYER")) {
            const int prev = text.line_layers[idx1] - 1; // this line opens the next layer
            if (prev >= 0 && prev < int(layer_pauses->size()) && (*layer_pauses)[prev] > 0.05f) {
                const float sec = (*layer_pauses)[prev];
                char        buf[160];
                std::snprintf(buf, sizeof(buf), "; THERMAL_PAUSE %.1f s: park away from the part so layer %d can cool\n", sec, prev + 1);
                out << buf;
                float px = 0.f, py = 0.f;
                const bool park = have_xy && park_point(cur_x, cur_y, px, py);
                const float lift = park ? 2.f : 5.f; // no safe spot beside the part: at least get the hot nozzle away from it
                if (retract > 0.f) { std::snprintf(buf, sizeof(buf), "G1 E-%.3f F%d\n", retract, int(text.retraction_speed * 60.f)); out << buf; }
                if (cur_z >= 0.f) { std::snprintf(buf, sizeof(buf), "G1 Z%.3f F600\n", cur_z + lift); out << buf; }
                if (park) { std::snprintf(buf, sizeof(buf), "G1 X%.3f Y%.3f F%d\n", px, py, int(text.travel_speed * 60.f)); out << buf; }
                std::snprintf(buf, sizeof(buf), "G4 P%d\n", int(std::lround(sec * 1000.f))); out << buf;
                if (park) { std::snprintf(buf, sizeof(buf), "G1 X%.3f Y%.3f F%d\n", cur_x, cur_y, int(text.travel_speed * 60.f)); out << buf; }
                if (cur_z >= 0.f) { std::snprintf(buf, sizeof(buf), "G1 Z%.3f F600\n", cur_z); out << buf; }
                if (retract > 0.f) { std::snprintf(buf, sizeof(buf), "G1 E%.3f F%d\n", retract, int(text.retraction_speed * 60.f)); out << buf; }
            }
        }
        if (info.motion) {
            if (info.has_x) cur_x = info.x;
            if (info.has_y) cur_y = info.y;
            if (info.has_z) cur_z = info.z;
            if (info.has_x && info.has_y) have_xy = true;
        }
        if (const size_t old_tag = line.find(std::string(" ") + Tag); old_tag != std::string::npos) line.erase(old_tag); // re-run: replace, never stack
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
            if (!info.has_xy && info.has_e) {
                target = 0; // retract / deretract speed: leave alone
            } else if (!info.has_xy) {
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

float ThermalQuality::fit_cooling_constant(const GCodeProcessorResult& result, const std::vector<int>& move_layers,
                                           ThermalMaterials materials, int slot, int layer, float target_temp, bool* bounded)
{
    if (materials.empty()) materials.emplace_back();
    ThermalModelParams& mat  = materials[std::clamp<size_t>(size_t(std::max(slot, 0)), 0, materials.size() - 1)];
    auto                temp = [&](float tau) {
        mat.cooling_time_const = tau;
        const ThermalReport rep = simulate(result, move_layers, materials);
        return layer >= 0 && layer < int(rep.layer_substrate_temp.size()) ? rep.layer_substrate_temp[layer] : target_temp;
    };
    float lo = 0.5f, hi = 60.f; // longer constant = hotter substrate
    if (bounded != nullptr) *bounded = false;
    if (temp(lo) >= target_temp) { if (bounded != nullptr) *bounded = true; return lo; }
    if (temp(hi) <= target_temp) { if (bounded != nullptr) *bounded = true; return hi; }
    for (int it = 0; it < 14; ++it) { // ponytail: plain bisection, ~0.004 s resolution, a dozen simulations
        const float mid = 0.5f * (lo + hi);
        (temp(mid) < target_temp ? lo : hi) = mid;
    }
    return 0.5f * (lo + hi);
}

std::string ThermalQuality::quality_grade(float before, float after)
{
    const float r = (before - after) / std::max(before, 1e-3f);
    return r > 0.3f ? "HIGH" : (r > 0.1f ? "MEDIUM" : "LOW");
}

} // namespace Slic3r
