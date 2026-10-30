#ifndef slic3r_SettingsAdvisor_hpp_
#define slic3r_SettingsAdvisor_hpp_

// Settings Advisor: measures the objects on a plate and suggests print settings for an intent
// (functional / visual / fast draft). Pure geometry + rules, no GUI, so it can be unit tested.

#include <functional>
#include <limits>
#include <string>
#include <vector>

#include "PrintConfig.hpp"
#include "TriangleMesh.hpp"

namespace Slic3r {
class Model;

namespace SettingsAdvisor {

enum class Intent { Functional, Visual, FastDraft };

// Geometry of one object in world coordinates (Z up, all instances share the same shape).
struct ObjectAnalysis
{
    std::string name;
    int         object_idx = -1;      // index in Model::objects, used to apply per-object overrides
    DynamicPrintConfig overrides;      // the object's own settings (ModelObject::config)

    double volume              = 0.;  // mm^3
    double height              = 0.;  // mm
    double bed_contact_area    = 0.;  // mm^2, downward faces touching the bed
    double top_flat_area       = 0.;  // mm^2, upward faces within ~8 deg of horizontal
    double overhang_area       = 0.;  // mm^2, downward faces flatter than 45 deg off the bed
    double steep_overhang_area = 0.;  // mm^2, downward faces flatter than 30 deg (needs support)
    double min_wall            = std::numeric_limits<double>::infinity(); // mm, thin-wall estimate

    // Pre-slice results (filled by the GUI job when a pre-slice ran; 0 = not measured).
    double sliced_max_top_area = 0.;  // mm^2, largest top surface on any layer
    double sliced_overhang_area = 0.; // mm^2, total unsupported area over all layers
};

// its must already be transformed to world coordinates. `canceled` is polled between chunks.
ObjectAnalysis analyze_mesh(const indexed_triangle_set &its, const std::function<bool()> &canceled = {});

// Slices `model` (a private copy: objects[i] analyses model.objects[i], one instance each) with `config`
// up to the layer outlines only, on a temporary Print, and fills the sliced_* fields from consecutive
// layers. Returns false if canceled or the model is too large to pre-slice quickly.
bool preslice(const Model &model, DynamicPrintConfig config, std::vector<ObjectAnalysis> &objects,
              const std::function<bool()> &canceled = {});

struct Suggestion
{
    std::string key;            // print config option key
    int         object_idx = -1; // -1: plate-wide (process preset), otherwise ObjectAnalysis::object_idx
    std::string current;         // serialized current value
    std::string suggested;       // serialized suggested value (set_deserialize_strict compatible)
    std::string reason;          // short English explanation (translated by the GUI)
};

// `full` is the full config the plate would slice with (PresetBundle::full_config()).
std::vector<Suggestion> suggest(const std::vector<ObjectAnalysis> &objects, Intent intent, const DynamicPrintConfig &full);

} // namespace SettingsAdvisor
} // namespace Slic3r

#endif // slic3r_SettingsAdvisor_hpp_
