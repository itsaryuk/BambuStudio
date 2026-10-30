#include "SettingsAdvisor.hpp"

#include <algorithm>
#include <cmath>
#include <map>

#include <boost/format.hpp>

#include "AABBMesh.hpp"
#include "ClipperUtils.hpp"
#include "Layer.hpp"
#include "Model.hpp"
#include "Print.hpp"

namespace Slic3r {
namespace SettingsAdvisor {

// --- Geometry thresholds -------------------------------------------------------------------------
// Faces this close to the lowest point count as bed contact.
static constexpr double BED_CONTACT_Z_TOL = 0.2;      // mm
// cos(8 deg): faces this close to horizontal count as flat tops / bed contact.
static constexpr double FLAT_COS          = 0.99;
// Downward faces flatter than 45 deg from horizontal print as overhangs, flatter than 30 deg need support.
static const double     OVERHANG_COS      = std::cos(M_PI / 4.);
static const double     STEEP_COS         = std::cos(M_PI / 6.);
// Ray-cast at most this many faces for the thin-wall estimate.
static constexpr size_t MAX_WALL_SAMPLES  = 20000;
// Ignore hits closer than this (coplanar / degenerate faces).
static constexpr double MIN_WALL_HIT      = 0.05;     // mm

// --- Rule thresholds -----------------------------------------------------------------------------
// Unsupported downward area above this needs support.
static constexpr double SUPPORT_AREA_MIN  = 25.;      // mm^2
// Tall objects on a small base tip over / detach: height / sqrt(contact area) above this wants a brim.
static constexpr double TIP_RATIO         = 4.;
static constexpr double SMALL_CONTACT     = 100.;     // mm^2
// Large flat tops show pillowing and benefit from more top layers / ironing.
static constexpr double LARGE_TOP_AREA    = 1000.;    // mm^2
static constexpr double IRONING_TOP_AREA  = 2000.;    // mm^2
// Bases larger than this warp at the corners with shrinking materials (ABS/ASA/PA/PC).
static constexpr double LARGE_FOOTPRINT   = 2500.;    // mm^2 (~50x50 mm)

ObjectAnalysis analyze_mesh(const indexed_triangle_set &its, const std::function<bool()> &canceled)
{
    ObjectAnalysis out;
    if (its.indices.empty())
        return out;

    BoundingBoxf3 bb;
    for (const stl_vertex &v : its.vertices)
        bb.merge(v.cast<double>());
    const double min_z = bb.min.z();
    out.height = bb.size().z();
    out.volume = std::abs(its_volume(its));

    std::vector<Vec3d>  centroids(its.indices.size());
    std::vector<Vec3d>  normals(its.indices.size());
    for (size_t i = 0; i < its.indices.size(); ++i) {
        const Vec3d a = its.vertices[its.indices[i](0)].cast<double>();
        const Vec3d b = its.vertices[its.indices[i](1)].cast<double>();
        const Vec3d c = its.vertices[its.indices[i](2)].cast<double>();
        const Vec3d cross = (b - a).cross(c - a);
        const double area = 0.5 * cross.norm();
        centroids[i] = (a + b + c) / 3.;
        if (area <= 0.)
            continue;
        const Vec3d n = cross.normalized();
        normals[i] = n;
        if (n.z() < -FLAT_COS && centroids[i].z() < min_z + BED_CONTACT_Z_TOL) {
            out.bed_contact_area += area;
        } else if (n.z() < 0. && centroids[i].z() > min_z + BED_CONTACT_Z_TOL) {
            if (-n.z() > OVERHANG_COS) out.overhang_area += area;
            if (-n.z() > STEEP_COS)    out.steep_overhang_area += area;
        } else if (n.z() > FLAT_COS) {
            out.top_flat_area += area;
        }
    }

    // Thin-wall estimate: cast a ray from each sampled face straight inside; the first hit is the local
    // wall thickness. Take a low percentile rather than the minimum to ignore single bad triangles.
    if (canceled && canceled())
        return out;
    AABBMesh mesh(its);
    const size_t stride = std::max<size_t>(1, its.indices.size() / MAX_WALL_SAMPLES);
    std::vector<double> thickness;
    for (size_t i = 0; i < its.indices.size(); i += stride) {
        if ((i / stride) % 1024 == 0 && canceled && canceled())
            return out;
        if (normals[i].squaredNorm() == 0.)
            continue;
        const Vec3d dir = -normals[i];
        const auto  hit = mesh.query_ray_hit(centroids[i] + dir * 1e-4, dir);
        if (hit.is_hit() && hit.distance() > MIN_WALL_HIT)
            thickness.push_back(hit.distance());
    }
    if (!thickness.empty()) {
        const size_t k = thickness.size() / 50; // 2nd percentile
        std::nth_element(thickness.begin(), thickness.begin() + k, thickness.end());
        out.min_wall = thickness[k];
    }
    return out;
}

// Above this many triangles on the plate the pre-slice is skipped and mesh analysis alone is used.
static constexpr size_t PRESLICE_MAX_TRIANGLES = 2000000;

bool preslice(const Model &model, DynamicPrintConfig config, std::vector<ObjectAnalysis> &objects, const std::function<bool()> &canceled)
{
    size_t triangles = 0;
    for (const ModelObject *obj : model.objects)
        for (const ModelVolume *vol : obj->volumes)
            triangles += vol->mesh().its.indices.size();
    if (model.objects.size() != objects.size() || triangles > PRESLICE_MAX_TRIANGLES)
        return false;

    // Only the layer outlines are needed: switch off everything that would add work or global state.
    config.set_deserialize_strict("enable_support", "0");
    config.set_deserialize_strict("raft_layers", "0");
    config.set_deserialize_strict("enable_prime_tower", "0");
    config.set_deserialize_strict("spiral_mode", "0");
    config.set_deserialize_strict("brim_type", "no_brim");

    Print print;
    print.apply(model, config);
    const auto  *nozzles = config.option<ConfigOptionFloats>("nozzle_diameter");
    const double nozzle  = nozzles && !nozzles->values.empty() ? nozzles->values.front() : 0.4;
    // A layer may stick out this far past the one below and still be carried by the wall underneath.
    const float  carried = float(scale_(0.5 * nozzle));
    const double mm2     = SCALING_FACTOR * SCALING_FACTOR;

    for (PrintObject *po : print.objects_mutable()) {
        if (canceled && canceled())
            return false;
        auto it = std::find_if(model.objects.begin(), model.objects.end(),
                               [po](const ModelObject *mo) { return mo->id() == po->model_object()->id(); });
        if (it == model.objects.end())
            continue;
        ObjectAnalysis &out = objects[it - model.objects.begin()];
        po->slice();
        double max_top = 0., unsupported = 0.;
        for (const Layer *layer : po->layers()) {
            if (layer->upper_layer)
                max_top = std::max(max_top, area(diff_ex(layer->lslices, layer->upper_layer->lslices)) * mm2);
            else
                max_top = std::max(max_top, area(layer->lslices) * mm2);
            if (layer->lower_layer)
                unsupported += area(diff_ex(layer->lslices, expand(to_polygons(layer->lower_layer->lslices), carried))) * mm2;
        }
        out.sliced_max_top_area  = max_top;
        out.sliced_overhang_area = unsupported;
    }
    return !(canceled && canceled());
}

namespace {

std::string fmt_mm(double v) { return (boost::format("%.2f") % v).str(); }

// Current value of a key for one object: its own override wins over the plate config.
std::string effective(const DynamicPrintConfig &full, const ObjectAnalysis &obj, const std::string &key)
{
    return obj.overrides.has(key) ? obj.overrides.opt_serialize(key) : full.opt_serialize(key);
}

struct Desired
{
    std::string value;
    std::string reason;
};

// Per-object desired values for the options that can differ between objects.
std::map<std::string, Desired> desired_for_object(const ObjectAnalysis &obj, Intent intent, double layer_height, double nozzle)
{
    std::map<std::string, Desired> d;
    const char *intent_name = intent == Intent::Functional ? "functional" : intent == Intent::Visual ? "visual" : "fast draft";

    int walls = intent == Intent::Functional ? 4 : intent == Intent::Visual ? 3 : 2;
    std::string wall_reason = (boost::format("%1% print: %2% walls") % intent_name % walls).str();
    // A wall thinner than the requested shells is printed solid anyway; say so instead of adding walls.
    const double line = 1.125 * nozzle;
    if (std::isfinite(obj.min_wall) && obj.min_wall < 2. * walls * line)
        wall_reason += (boost::format(" (thinnest wall ~%1% mm is solid either way)") % fmt_mm(obj.min_wall)).str();
    d["wall_loops"] = { std::to_string(walls), wall_reason };

    switch (intent) {
    case Intent::Functional:
        d["sparse_infill_density"] = { "30%", "functional print: 30% infill for strength" };
        d["sparse_infill_pattern"] = { "gyroid", "gyroid is strong in every direction" };
        break;
    case Intent::Visual:
        d["sparse_infill_density"] = { "15%", "visual print: 15% is enough to support the top surfaces" };
        d["sparse_infill_pattern"] = { "gyroid", "gyroid doesn't show through thin walls as lines" };
        break;
    case Intent::FastDraft:
        d["sparse_infill_density"] = { "10%", "fast draft: minimal infill" };
        d["sparse_infill_pattern"] = { "grid", "grid prints fast" };
        break;
    }

    const double top_area = std::max(obj.top_flat_area, obj.sliced_max_top_area);
    double top_mm = intent == Intent::FastDraft ? 0.6 : 1.0;
    std::string top_reason = (boost::format("%1% mm of top shell for a %2% print") % fmt_mm(top_mm) % intent_name).str();
    if (intent != Intent::FastDraft && top_area > LARGE_TOP_AREA) {
        top_mm += 0.2;
        top_reason = (boost::format("large flat top (%1% mm2): thicker top shell avoids pillowing") % int(top_area)).str();
    }
    const double bottom_mm = intent == Intent::Functional ? 0.8 : 0.6;
    d["top_shell_layers"]    = { std::to_string(int(std::ceil(top_mm / layer_height - 1e-6))), top_reason };
    d["bottom_shell_layers"] = { std::to_string(int(std::ceil(bottom_mm / layer_height - 1e-6))),
                                 (boost::format("%1% mm of bottom shell") % fmt_mm(bottom_mm)).str() };
    return d;
}

} // namespace

std::vector<Suggestion> suggest(const std::vector<ObjectAnalysis> &objects, Intent intent, const DynamicPrintConfig &full)
{
    std::vector<Suggestion> out;
    if (objects.empty())
        return out;

    auto plate = [&out, &full](const std::string &key, const std::string &value, const std::string &reason) {
        if (full.has(key) && full.opt_serialize(key) != value)
            out.push_back({ key, -1, full.opt_serialize(key), value, reason });
    };

    const auto  *nozzles = full.option<ConfigOptionFloats>("nozzle_diameter");
    const double nozzle  = nozzles && !nozzles->values.empty() ? nozzles->values.front() : 0.4;

    // Layer height first: shell layer counts depend on it.
    double lh = full.opt_float("layer_height");
    {
        const double base = intent == Intent::Functional ? 0.5 : intent == Intent::Visual ? 0.3 : 0.7; // x nozzle
        double target = std::round(base * nozzle * 100.) / 100.;
        const auto *mn = full.option<ConfigOptionFloats>("min_layer_height");
        const auto *mx = full.option<ConfigOptionFloats>("max_layer_height");
        if (mn && !mn->values.empty()) target = std::max(target, mn->values.front());
        if (mx && !mx->values.empty() && mx->values.front() > 0.) target = std::min(target, mx->values.front());
        if (std::abs(target - lh) > 1e-6) {
            plate("layer_height", fmt_mm(target),
                  (boost::format("%1% layers for a %2% nozzle") %
                   (intent == Intent::Functional ? "balanced" : intent == Intent::Visual ? "fine" : "thick") % fmt_mm(nozzle)).str());
            lh = target;
        }
    }

    // Per-object options: plate-wide when every object wants the same value and none overrides it,
    // otherwise per object.
    std::vector<std::map<std::string, Desired>> desired;
    for (const ObjectAnalysis &obj : objects)
        desired.push_back(desired_for_object(obj, intent, lh, nozzle));
    for (const auto &[key, first] : desired.front()) {
        bool same = true, any_override = false;
        for (size_t i = 0; i < objects.size(); ++i) {
            same = same && desired[i].at(key).value == first.value;
            any_override = any_override || objects[i].overrides.has(key);
        }
        if (same && !any_override) {
            plate(key, first.value, first.reason);
            continue;
        }
        for (size_t i = 0; i < objects.size(); ++i) {
            const std::string cur = effective(full, objects[i], key);
            if (cur != desired[i].at(key).value)
                out.push_back({ key, objects[i].object_idx, cur, desired[i].at(key).value, desired[i].at(key).reason });
        }
    }

    // Supports: plate-wide, needed if any object has a large flat unsupported area.
    const ObjectAnalysis *worst_overhang = nullptr;
    for (const ObjectAnalysis &obj : objects) {
        const double area = std::max(obj.steep_overhang_area, obj.sliced_overhang_area);
        if (area > SUPPORT_AREA_MIN && (!worst_overhang || area > std::max(worst_overhang->steep_overhang_area, worst_overhang->sliced_overhang_area)))
            worst_overhang = &obj;
    }
    if (worst_overhang) {
        plate("enable_support", "1",
              (boost::format("'%1%' has %2% mm2 of unsupported overhang") % worst_overhang->name %
               int(std::max(worst_overhang->steep_overhang_area, worst_overhang->sliced_overhang_area))).str());
        if (intent != Intent::Functional)
            plate("support_type", "tree(auto)", "tree supports leave fewer marks and are easier to remove");
    } else {
        plate("enable_support", "0", "no object has an overhang that needs support");
    }

    // Brim: tall objects on a small footprint.
    const ObjectAnalysis *tippy = nullptr;
    double worst_ratio = 0.;
    for (const ObjectAnalysis &obj : objects) {
        const double contact = std::max(obj.bed_contact_area, 1.);
        const double ratio   = obj.height / std::sqrt(contact);
        if ((ratio > TIP_RATIO || (contact < SMALL_CONTACT && obj.height > 20.)) && ratio > worst_ratio) {
            worst_ratio = ratio;
            tippy       = &obj;
        }
    }
    // Materials that shrink a lot warp at the corners of large footprints.
    std::string warping_material;
    if (const auto *types = full.option<ConfigOptionStrings>("filament_type"); types && !types->values.empty()) {
        for (const char *m : { "ABS", "ASA", "PA", "PC", "NYLON" })
            if (types->values.front().find(m) != std::string::npos)
                warping_material = types->values.front();
    }
    const ObjectAnalysis *large_base = nullptr;
    for (const ObjectAnalysis &obj : objects)
        if (obj.bed_contact_area > LARGE_FOOTPRINT && (!large_base || obj.bed_contact_area > large_base->bed_contact_area))
            large_base = &obj;
    if (full.opt_serialize("brim_type") == "no_brim") {
        std::string why;
        if (tippy)
            why = (boost::format("'%1%' is %2% mm tall on %3% mm2 of bed contact") % tippy->name % int(tippy->height) %
                   int(tippy->bed_contact_area)).str();
        else if (large_base && !warping_material.empty())
            why = (boost::format("%1% warps at the corners of large bases ('%2%': %3% mm2)") % warping_material % large_base->name %
                   int(large_base->bed_contact_area)).str();
        if (!why.empty()) {
            plate("brim_type", "outer_only", why);
            plate("brim_width", "5", why);
        }
    }

    // Walls thinner than two lines: Arachne varies the line width instead of leaving gaps or dropping the wall.
    const double line = 1.125 * nozzle;
    for (const ObjectAnalysis &obj : objects)
        if (std::isfinite(obj.min_wall) && obj.min_wall < 2. * line && full.opt_serialize("wall_generator") == "classic") {
            plate("wall_generator", "arachne",
                  (boost::format("'%1%' has walls down to ~%2% mm, thinner than two lines") % obj.name % fmt_mm(obj.min_wall)).str());
            break;
        }

    // Surface finish for visual prints.
    if (intent == Intent::Visual) {
        const std::string top = full.opt_serialize("top_surface_pattern");
        if (top != "monotonic" && top != "monotonicline")
            plate("top_surface_pattern", "monotonic", "monotonic top lines look even, without ridges");
        const std::string seam = full.opt_serialize("seam_position");
        if (seam == "random" || seam == "back")
            plate("seam_position", "aligned", "aligned seams are placed in hidden spots and inner corners");
        double max_top = 0.;
        for (const ObjectAnalysis &obj : objects)
            max_top = std::max({ max_top, obj.top_flat_area, obj.sliced_max_top_area });
        if (max_top > IRONING_TOP_AREA && full.opt_serialize("ironing_type") == "no ironing")
            plate("ironing_type", "top", (boost::format("large flat top (%1% mm2): ironing makes it smooth") % int(max_top)).str());
    }
    return out;
}

} // namespace SettingsAdvisor
} // namespace Slic3r
