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
    wxSpinCtrl*            m_chamber{nullptr};
    wxSpinCtrl*            m_speed_up{nullptr};
    wxSpinCtrl*            m_slow_down{nullptr};
    wxSpinCtrlDouble*      m_max_volumetric{nullptr};
    wxCheckBox*            m_keep_outer{nullptr};
};

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
    void     init(const wxString& headline);
    void     add_line(const wxString& text, bool bold = false);
    void     finish(bool offer_optimize);
    wxSizer* m_sizer{nullptr};
};

}} // namespace Slic3r::GUI

#endif // slic3r_ThermalOptimizationDialog_hpp_
