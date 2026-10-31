#ifndef slic3r_ThermalOptimizationDialog_hpp_
#define slic3r_ThermalOptimizationDialog_hpp_

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <wx/checkbox.h>
#include <wx/radiobut.h>
#include <wx/spinctrl.h>

#include "GUI_Utils.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/ThermalQuality.hpp"

namespace Slic3r { namespace GUI {

// Mode and limits for the built-in Thermal Optimization (runs locally, no account needed).
class ThermalOptimizationInputDialog : public DPIDialog
{
public:
    ThermalOptimizationInputDialog(wxWindow* parent, const ThermalMaterials& params, const ThermalOptimizeOptions& options, bool preselect_optimize, bool plate_ready = true);

    int                    get_action() const { return m_action; } // 0 analyze, 1 optimize
    ThermalMaterials       get_params() const;
    ThermalOptimizeOptions get_options() const;

    void on_dpi_changed(const wxRect& suggested_rect) override { Fit(); Refresh(); }

private:
    ThermalMaterials       m_params;
    ThermalOptimizeOptions m_options;
    int                    m_action{0};
    wxRadioButton*         m_check{nullptr};
    wxRadioButton*         m_adjust{nullptr};
    wxSpinCtrl*            m_chamber{nullptr};
    wxSpinCtrl*            m_speed_up{nullptr};
    wxSpinCtrl*            m_slow_down{nullptr};
    wxSpinCtrlDouble*      m_max_volumetric{nullptr};
    wxCheckBox*            m_keep_outer{nullptr};
    wxCheckBox*            m_pause{nullptr};
    wxSpinCtrl*            m_pause_cap{nullptr};
};

// Tower calibrations are stored per filament type and printer model: printers differ in airflow, which the model cannot
// see. Layer height and fan are scaled by the model, so they are only recorded in the note.
struct ThermalCalibration
{
    std::string key;                 // app config key in section "thermal", empty when not calibrated
    bool        other_printer{false}; // found only for another printer model
    float       layer_height{0.f};   // of the calibration tower, 0 when not recorded
};
std::string        thermal_calibration_key(const std::string& filament_type); // where a calibration on this printer goes
ThermalCalibration thermal_calibration_find(const std::string& filament_type);
std::string        thermal_setup_note(); // printer, nozzle and layer height of the loaded presets
// One line per calibrated filament type whose calibration came from another printer or another layer height.
wxString           thermal_calibration_warnings();

// Three steps from a printed tower to a calibrated model: create and print the tower, read it, enter the height.
class ThermalCalibrationDialog : public DPIDialog
{
public:
    struct State
    {
        wxString material;              // filament type(s) loaded
        wxString stored;                // current calibration, empty when none
        bool     plate_ready{false};    // a sliced, valid plate is open
        bool     plate_is_tower{false}; // the open project contains the calibration tower
        int      slow_down_now{-1};     // loaded filament's slow-down layer time, -1 unknown
    };
    ThermalCalibrationDialog(wxWindow* parent, const State& state, std::function<void()> create_tower, std::function<wxString(float)> calibrate);
    void on_dpi_changed(const wxRect& suggested_rect) override { Fit(); Refresh(); }

private:
    wxSpinCtrlDouble* m_height{nullptr};
    Label*            m_result{nullptr};
};

class ThermalOptimizationResultDialog : public DPIDialog
{
public:
    // Analysis summary. roles_times is used to estimate the time Optimize could save.
    ThermalOptimizationResultDialog(wxWindow* parent, const ThermalReport& report, int original_time_seconds,
                                    const std::vector<std::pair<ExtrusionRole, float>>& roles_times);
    // Optimization summary.
    ThermalOptimizationResultDialog(wxWindow* parent, int original_time_seconds, int optimized_time_seconds,
                                    const std::string& balance_grade, const std::string& consistency_grade,
                                    const ThermalOptimizeSummary& summary = ThermalOptimizeSummary());

    void on_dpi_changed(const wxRect& suggested_rect) override { Fit(); Refresh(); }

private:
    void     init(const wxString& headline, const wxColour& tone);
    void     add_line(const wxString& text, bool bold = false);
    void     add_scale();                                                    // cold / comfortable / hot colour bar, as in the preview
    void     add_details(const std::vector<wxString>& lines);                // folded "Details" section
    void     finish(bool offer_optimize, float cooling_tower_height = 0.f);
    wxSizer* m_sizer{nullptr};
};

}} // namespace Slic3r::GUI

#endif // slic3r_ThermalOptimizationDialog_hpp_
