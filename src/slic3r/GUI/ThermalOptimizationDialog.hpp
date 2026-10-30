#ifndef slic3r_ThermalOptimizationDialog_hpp_
#define slic3r_ThermalOptimizationDialog_hpp_

#include <string>
#include <utility>
#include <vector>

#include <wx/checkbox.h>
#include <wx/radiobut.h>
#include <wx/spinctrl.h>

#include "GUI_Utils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/ThermalQuality.hpp"

namespace Slic3r { namespace GUI {

// Mode and limits for the built-in Thermal Optimization (runs locally, no account needed).
class ThermalOptimizationInputDialog : public DPIDialog
{
public:
    ThermalOptimizationInputDialog(wxWindow* parent, const ThermalMaterials& params, const ThermalOptimizeOptions& options, bool preselect_optimize);

    int                    get_action() const { return m_optimize->GetValue() ? 1 : 0; } // 0 analyze, 1 optimize
    ThermalMaterials       get_params() const;
    ThermalOptimizeOptions get_options() const;

    void on_dpi_changed(const wxRect& suggested_rect) override { Fit(); Refresh(); }

private:
    ThermalMaterials       m_params;
    ThermalOptimizeOptions m_options;
    wxRadioButton*         m_analyze{nullptr};
    wxRadioButton*         m_optimize{nullptr};
    wxSpinCtrl*            m_chamber{nullptr};
    wxSpinCtrl*            m_speed_up{nullptr};
    wxSpinCtrl*            m_slow_down{nullptr};
    wxSpinCtrlDouble*      m_max_volumetric{nullptr};
    wxCheckBox*            m_keep_outer{nullptr};
};

class ThermalOptimizationResultDialog : public DPIDialog
{
public:
    // Analysis summary. roles_times is used to estimate the time Optimize could save.
    ThermalOptimizationResultDialog(wxWindow* parent, const ThermalReport& report, int original_time_seconds,
                                    const std::vector<std::pair<ExtrusionRole, float>>& roles_times);
    // Optimization summary.
    ThermalOptimizationResultDialog(wxWindow* parent, int original_time_seconds, int optimized_time_seconds,
                                    const std::string& balance_grade, const std::string& consistency_grade);

    void on_dpi_changed(const wxRect& suggested_rect) override { Fit(); Refresh(); }

private:
    void     init(const wxString& headline);
    void     add_line(const wxString& text, bool bold = false);
    void     finish(bool offer_optimize);
    wxSizer* m_sizer{nullptr};
};

}} // namespace Slic3r::GUI

#endif // slic3r_ThermalOptimizationDialog_hpp_
