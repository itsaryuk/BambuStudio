#ifndef slic3r_ThermalQuality_hpp_
#define slic3r_ThermalQuality_hpp_

#include <string>
#include <utility>
#include <vector>

#include "GCodeProcessor.hpp"

namespace Slic3r {

// Local layer-stack cooling model. It walks the processed toolpaths in print order, keeps a coarse
// 2D grid of "what was laid down here, when, and how hot", and rates every extrusion by the
// temperature of the material it lands on:
//   index -1 : substrate fully cooled (weak bonding / warping risk for engineering materials)
//   index  0 : substrate at the material's comfort temperature
//   index +1 : substrate still soft (sagging, loss of detail, stringing)
// Temperatures in °C, times in s, lengths in mm.
struct ThermalModelParams
{
    std::string material;              // material class the defaults came from (shown in the report)
    float room_temp{25.f};
    float chamber_temp{-1.f};          // <= 0: unheated chamber
    float bed_temp{60.f};
    float bed_air_decay_height{20.f};  // bed-warmed air fades to room/chamber temperature over this height
    float ideal_substrate_temp{55.f};  // comfort window midpoint (≈ vitrification temperature)
    float window_half_width{40.f};     // index reaches ±1 at midpoint ± this
    float cooling_time_const{6.f};     // still air, 0.2 mm layer
    float fan_cooling_gain{1.5f};      // time constant divisor at 100 % part fan
    float deposit_mix{0.5f};           // share of nozzle temperature in a freshly laid line
    float cell_size{1.f};
    float diffusivity{0.1f};           // mm²/s, lateral conduction: heat reaches sqrt(diffusivity * age) sideways
    float max_volumetric{-1.f};        // filament profile limit for the optimizer (mm³/s), <= 0: none
    float min_speed{-1.f};             // filament profile slow-down floor (mm/s), <= 0: none
    bool  warp_prone{false};           // engineering materials: advice favours enclosure and layer time over the part fan

    // Defaults for a filament type (exact, then base type before '-', then without trailing digits: PA6-CF -> PA6 -> PA);
    // vitrification_temp <= 0 falls back to the class midpoint.
    static ThermalModelParams for_material(const std::string& filament_type, float vitrification_temp);

    // Derives cooling_time_const from the preset's tuned minimum layer time: a layer that short, with the
    // preset's maximum part fan, must have cooled to the edge of the comfort window when the next one lands.
    void calibrate_cooling_from_preset(float slow_down_layer_time, float fan_max_pct, float nozzle_temp, float env_temp);
};

// One entry per filament slot, indexed by MoveVertex::extruder_id; the first entry is used for missing slots.
using ThermalMaterials = std::vector<ThermalModelParams>;

struct ThermalOptimizeOptions
{
    float min_speed_factor{0.6f};
    float max_speed_factor{1.4f};
    float max_speed{-1.f};        // mm/s, <= 0: no cap
    float max_volumetric{-1.f};   // mm³/s, <= 0: no cap
    bool  keep_outer_walls{false};
    int   first_layer{1};         // 0-based, inclusive. Layer 0 is never touched.
    int   last_layer{-1};         // -1: to the end
    int   passes{3};
};

struct ThermalMoveIndex { float min{0.f}, max{0.f}, mean{0.f}; };

struct ThermalReport
{
    std::vector<ThermalMoveIndex> moves;   // parallel to GCodeProcessorResult::moves
    std::vector<float>            layer_mean;
    std::vector<float>            layer_substrate_temp; // time-weighted mean substrate temperature per layer (°C, unclamped)
    float mean{0.f}, stddev{0.f};
    float hot_fraction{0.f}, cold_fraction{0.f}; // share of extrusion time with |index| > 0.8
    std::string outcome;    // WILL_PRINT / MAY_PRINT / LIKELY_FAIL
    std::string direction;  // NONE / OVERCOOLING / OVERHEATING
    std::vector<std::string> caveats;
    std::vector<std::string> assumptions; // material parameters the model used, one line per filament
    std::vector<std::pair<std::string, std::string>> fixes; // {QUICK|ADVANCED|EXPERT, text}
    float suggested_speed_factor{1.f}; // time multiplier for the optimizable sections, < 1 = faster
};

// The sliced G-code text plus the layer every line belongs to.
struct ThermalGCodeText
{
    std::vector<std::string> lines;       // without line terminators
    std::vector<int>         line_layers; // 1-based line -> 0-based layer, -1 before the first layer
    static bool load(const std::string& path, ThermalGCodeText& out);
};

class ThermalQuality
{
public:
    static std::vector<int> layers_of_moves(const GCodeProcessorResult& result, const std::vector<int>& line_layers);

    static ThermalReport simulate(const GCodeProcessorResult&    result,
                                  const std::vector<int>&        move_layers,
                                  const ThermalMaterials&        materials,
                                  const std::vector<float>*      layer_speed_factors = nullptr,
                                  const ThermalOptimizeOptions*  eligibility         = nullptr);
    static ThermalReport simulate(const GCodeProcessorResult& result, const std::vector<int>& move_layers, const ThermalModelParams& params,
                                  const std::vector<float>* layer_speed_factors = nullptr, const ThermalOptimizeOptions* eligibility = nullptr)
    {
        return simulate(result, move_layers, ThermalMaterials{params}, layer_speed_factors, eligibility);
    }

    // Per-layer feedrate multipliers that pull every layer towards the comfort window, within the
    // options and the filament profile limits (max volumetric speed, slow-down floor).
    static std::vector<float> optimize(const GCodeProcessorResult& result,
                                       const std::vector<int>&     move_layers,
                                       const ThermalMaterials&     materials,
                                       const ThermalOptimizeOptions& options);
    static std::vector<float> optimize(const GCodeProcessorResult& result, const std::vector<int>& move_layers, const ThermalModelParams& params,
                                       const ThermalOptimizeOptions& options)
    {
        return optimize(result, move_layers, ThermalMaterials{params}, options);
    }

    // Print-time multiplier of the optimizable sections implied by layer_speed_factors (< 1 = faster).
    static float time_factor(const GCodeProcessorResult& result, const std::vector<int>& move_layers,
                             const std::vector<float>& layer_speed_factors, const ThermalOptimizeOptions& options);

    // Writes text to dst with a thermal tag on every extrusion line and, when layer_speed_factors
    // is given, feedrates of eligible extrusions rescaled and M73 progress lines recomputed.
    static bool write_gcode(const ThermalGCodeText&       text,
                            const std::string&            dst,
                            const GCodeProcessorResult&   result,
                            const std::vector<int>&       move_layers,
                            const ThermalReport&          report,
                            const std::vector<float>*     layer_speed_factors = nullptr,
                            const ThermalOptimizeOptions* options             = nullptr);

    static bool eligible(const GCodeProcessorResult::MoveVertex& m, int layer, const ThermalOptimizeOptions& o);

    // HIGH / MEDIUM / LOW from a before/after magnitude.
    static std::string quality_grade(float before, float after);

    static constexpr const char* Tag = ";thermal=";
};

} // namespace Slic3r

#endif // slic3r_ThermalQuality_hpp_
