#include "ThermalOptimizationDialog.hpp"

#include <algorithm>
#include <cmath>

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/statbmp.h>

#include "Event.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/Utils.hpp"

namespace Slic3r { namespace GUI {

bool& get_helio_pre_select_optimization_flag();

namespace {

const wxString TITLE = _L("Thermal Optimization");

Button* make_button(wxWindow* parent, const wxString& text, bool primary)
{
    auto* btn = new Button(parent, text);
    if (primary) {
        StateColor bg(std::pair<wxColour, int>(wxColour(27, 136, 68), StateColor::Pressed),
                      std::pair<wxColour, int>(wxColour(61, 203, 115), StateColor::Hovered),
                      std::pair<wxColour, int>(wxColour(0, 174, 66), StateColor::Normal));
        btn->SetBackgroundColor(bg);
        btn->SetBorderColor(wxColour(0, 174, 66));
        btn->SetTextColor(StateColor(std::pair<wxColour, int>(*wxWHITE, StateColor::Normal)));
    }
    btn->SetFont(Label::Body_12);
    btn->SetMinSize(wxSize(parent->FromDIP(96), parent->FromDIP(28)));
    return btn;
}

wxString seconds_text(int s) { return wxString::FromUTF8(short_time(get_time_dhms(float(s)))); }

} // namespace

// ---------------------------------------------------------------------------------------------------------------------

ThermalOptimizationInputDialog::ThermalOptimizationInputDialog(wxWindow* parent, const ThermalMaterials& params,
                                                               const ThermalOptimizeOptions& options, bool preselect_optimize)
    : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, TITLE, wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_params(params)
    , m_options(options)
{
    SetBackgroundColour(*wxWHITE);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    const int pad = FromDIP(12);

    auto* intro = new Label(this, Label::Body_13,
        _L("Simulates how warm each layer still is when the next one lands and rates every toolpath from cold (-1) to hot (+1). Runs on this computer."), LB_AUTO_WRAP);
    intro->SetMinSize(wxSize(FromDIP(440), -1));
    intro->Wrap(FromDIP(440));
    sizer->Add(intro, 0, wxALL | wxEXPAND, pad);

    m_analyze  = new wxRadioButton(this, wxID_ANY, _L("Analyze: rate layer temperatures, keep the G-code as sliced"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    m_optimize = new wxRadioButton(this, wxID_ANY, _L("Optimize: rescale layer speeds so every layer lands in the comfort window"));
    (preselect_optimize ? m_optimize : m_analyze)->SetValue(true);
    sizer->Add(m_analyze, 0, wxLEFT | wxRIGHT, pad);
    sizer->Add(m_optimize, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(6));
    m_calibrate = new wxRadioButton(this, wxID_ANY, _L("Calibrate: the sliced plate is a printed calibration tower"));
    sizer->Add(m_calibrate, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(6));
    auto* calib_row = new wxBoxSizer(wxHORIZONTAL);
    calib_row->Add(new Label(this, Label::Body_13, _L("Height where corners rounded off or layers sagged (mm)")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    m_calib_height = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(90), -1), wxSP_ARROW_KEYS, 0., 300., 40., 0.5);
    calib_row->Add(m_calib_height, 0, wxALIGN_CENTER_VERTICAL);
    auto* tower = make_button(this, _L("Add calibration tower"), false);
    tower->SetToolTip(_L("Also in the Calibration menu as \"Thermal tower\", which works before anything is sliced. Opens a new project with a 30 mm cone: layers get shorter towards the top, so the material eventually runs hot. Print it with this filament, note the height where corners round off or the surface sags, then slice it again, open this dialog and enter that height."));
    tower->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        EndModal(wxID_CANCEL);
        wxGetApp().plater()->CallAfter([]() { wxGetApp().plater()->add_thermal_calibration_tower(); });
    });
    calib_row->Add(tower, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    auto* guide = make_button(this, _L("How to read it"), false);
    guide->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { ThermalCalibrationGuideDialog(this).ShowModal(); });
    calib_row->Add(guide, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    sizer->Add(calib_row, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(6));
    sizer->AddSpacer(pad);
    wxString materials;
    for (size_t i = 0; i < params.size(); ++i) {
        if (params[i].material.empty()) continue;
        if (!materials.empty()) materials += ", ";
        materials += wxString::Format("%s (%.0f ± %.0f °C)", wxString::FromUTF8(params[i].material), params[i].ideal_substrate_temp, params[i].window_half_width);
    }
    auto* mats = new Label(this, Label::Body_12, wxString::Format(_L("Material model from the filament presets: %s. Chamber air is prefilled from the printer and filament presets (heated chamber, or the bed warming an enclosure)."), materials), LB_AUTO_WRAP);
    mats->SetMinSize(wxSize(FromDIP(440), -1));
    mats->Wrap(FromDIP(440));
    sizer->Add(mats, 0, wxLEFT | wxRIGHT, pad);

    auto* grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
    grid->AddGrowableCol(1);
    auto row = [&](const wxString& name, wxWindow* ctrl) {
        grid->Add(new Label(this, Label::Body_13, name), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL);
    };
    m_chamber = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(90), -1), wxSP_ARROW_KEYS, 0, 120,
                               int(std::max(0.f, params.empty() ? 0.f : params.front().chamber_temp)));
    row(_L("Chamber air temperature (°C, 0 = open frame)"), m_chamber);
    m_speed_up = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(90), -1), wxSP_ARROW_KEYS, 0, 100,
                                int(std::lround((options.max_speed_factor - 1.f) * 100.f)));
    row(_L("Maximum speed-up on cold layers (%)"), m_speed_up);
    m_slow_down = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(90), -1), wxSP_ARROW_KEYS, 0, 90,
                                 int(std::lround((1.f - options.min_speed_factor) * 100.f)));
    row(_L("Maximum slow-down on hot layers (%)"), m_slow_down);
    m_max_volumetric = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(90), -1), wxSP_ARROW_KEYS, 0., 100.,
                                            std::max(0.f, options.max_volumetric), 0.5);
    row(_L("Maximum volumetric speed (mm³/s, 0 = no limit)"), m_max_volumetric);
    sizer->Add(grid, 0, wxALL | wxEXPAND, pad);

    m_keep_outer = new wxCheckBox(this, wxID_ANY, _L("Keep outer wall speeds unchanged (preserve surface finish)"));
    m_keep_outer->SetValue(options.keep_outer_walls);
    sizer->Add(m_keep_outer, 0, wxLEFT | wxRIGHT, pad);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer();
    auto* cancel = make_button(this, _L("Cancel"), false);
    auto* ok     = make_button(this, _L("Start"), true);
    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
    buttons->Add(cancel, 0, wxRIGHT, FromDIP(8));
    buttons->Add(ok, 0);
    sizer->Add(buttons, 0, wxALL | wxEXPAND, pad);

    SetSizer(sizer);
    Layout();
    Fit();
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

float ThermalOptimizationInputDialog::get_calibration_height() const { return float(m_calib_height->GetValue()); }

ThermalMaterials ThermalOptimizationInputDialog::get_params() const
{
    ThermalMaterials p = m_params;
    if (p.empty()) p.emplace_back();
    for (ThermalModelParams& m : p) m.chamber_temp = float(m_chamber->GetValue());
    return p;
}

ThermalOptimizeOptions ThermalOptimizationInputDialog::get_options() const
{
    ThermalOptimizeOptions o = m_options;
    o.max_speed_factor       = 1.f + m_speed_up->GetValue() / 100.f;
    o.min_speed_factor       = 1.f - m_slow_down->GetValue() / 100.f;
    o.max_volumetric         = m_max_volumetric->GetValue() > 0. ? float(m_max_volumetric->GetValue()) : -1.f;
    o.keep_outer_walls       = m_keep_outer->GetValue();
    return o;
}

// ---------------------------------------------------------------------------------------------------------------------

ThermalCalibrationGuideDialog::ThermalCalibrationGuideDialog(wxWindow* parent)
    : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, _L("Thermal calibration tower"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    SetBackgroundColour(*wxWHITE);
    auto* outer = new wxBoxSizer(wxVERTICAL);
    auto* body  = new wxBoxSizer(wxHORIZONTAL);

    wxImage photo(from_u8(resources_dir() + "/images/thermal_calibration_tower.jpg"), wxBITMAP_TYPE_JPEG);
    if (photo.IsOk()) {
        const int h = FromDIP(380);
        photo.Rescale(photo.GetWidth() * h / photo.GetHeight(), h, wxIMAGE_QUALITY_HIGH);
        body->Add(new wxStaticBitmap(this, wxID_ANY, wxBitmap(photo)), 0, wxRIGHT, FromDIP(16));
    }

    auto* text = new wxBoxSizer(wxVERTICAL);
    auto  para = [&](const wxString& t, bool bold = false) {
        auto* l = new Label(this, bold ? Label::Head_13 : Label::Body_13, t, LB_AUTO_WRAP);
        l->SetMinSize(wxSize(FromDIP(430), -1));
        l->Wrap(FromDIP(430));
        text->Add(l, 0, wxBOTTOM, FromDIP(bold ? 4 : 10));
    };
    para(_L("1. Print it"), true);
    para(_L("Slice this cone with the filament you want to calibrate and print it. Layer time shrinks towards the tip, so at some height the material stops cooling enough before the next layer lands. The slow-down layer time of the filament preset was set to 0 for this test; discard that change afterwards."));
    para(_L("2. Read it"), true);
    para(_L("The lower part has crisp, even layer lines. Higher up the surface turns rough, pebbled or glossy, and near the tip the layers sag or the cone bends. The rough band is where each layer was laid on material that was still soft. The bent tip is the same effect at its extreme and is not the number to use."));
    para(_L("3. Measure it"), true);
    para(_L("With a caliper, measure from the bottom of the cone (not the brim) to the lowest height where the surface is rough all the way around, as marked by the line in the photo. If the change is gradual, take the height where it is clearly rough, not the first isolated bump."));
    para(_L("4. Enter it"), true);
    para(_L("With the sliced tower still open, press the Thermal Optimization button, choose Calibrate, enter that height and press Start. The comfort window is then stored for this filament type and used automatically. Running the calibration again overwrites it."));
    body->Add(text, 1, wxEXPAND);

    outer->Add(body, 1, wxEXPAND | wxALL, FromDIP(20));
    auto* close = make_button(this, _L("Close"), true);
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
    outer->Add(close, 0, wxALIGN_RIGHT | wxRIGHT | wxBOTTOM, FromDIP(20));
    SetSizerAndFit(outer);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

ThermalOptimizationResultDialog::ThermalOptimizationResultDialog(wxWindow* parent, const ThermalReport& report, int original_time_seconds,
                                                                 const std::vector<std::pair<ExtrusionRole, float>>& roles_times)
    : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, TITLE, wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    wxString outcome = report.outcome == "WILL_PRINT" ? _L("Thermal conditions look good") :
                       report.outcome == "MAY_PRINT"  ? _L("Thermal conditions are marginal") :
                                                        _L("Thermal conditions are likely to cause defects");
    init(outcome);

    if (report.direction == "OVERCOOLING")
        add_line(_L("Material cools too far before the next layer is applied. This can cause weak layer bonding, warping, or cracking."));
    else if (report.direction == "OVERHEATING")
        add_line(_L("Heat builds up in small layers faster than it can dissipate. This may cause sagging or loss of detail in fine features."));
    else
        add_line(_L("Layers land within the comfort window of this material."));

    add_line(wxString::Format(_L("Mean thermal index %.2f, spread %.2f. Hot toolpaths %d%%, cold toolpaths %d%% of print time."),
                              report.mean, report.stddev, int(std::lround(report.hot_fraction * 100)), int(std::lround(report.cold_fraction * 100))));

    for (const std::string& c : report.caveats) add_line(wxString::FromUTF8("- " + c));
    for (const std::string& a : report.assumptions) add_line(wxString::FromUTF8(a));

    if (!report.fixes.empty()) {
        add_line(_L("Suggestions"), true);
        for (const auto& f : report.fixes) {
            wxString cat = f.first == "QUICK" ? _L("Quick") : f.first == "ADVANCED" ? _L("Advanced") : _L("Expert");
            add_line(wxString::Format("- %s: %s", cat, wxString::FromUTF8(f.second)));
        }
    }

    bool offer = false;
    if (report.suggested_speed_factor < 0.999f && original_time_seconds > 0) {
        float optimizable = 0.f;
        for (const auto& rt : roles_times)
            if (rt.first == erPerimeter || rt.first == erExternalPerimeter || rt.first == erInternalInfill || rt.first == erSolidInfill)
                optimizable += rt.second;
        const int saved = int(optimizable * (1.f - report.suggested_speed_factor));
        if (saved > 0) {
            offer = true;
            add_line(_L("Potential saving"), true);
            add_line(wxString::Format(_L("Optimize could save about %s (%s → %s) without leaving the comfort window."),
                                      seconds_text(saved), seconds_text(original_time_seconds), seconds_text(original_time_seconds - saved)));
        }
    }
    finish(offer);
}

ThermalOptimizationResultDialog::ThermalOptimizationResultDialog(wxWindow* parent, int original_time_seconds, int optimized_time_seconds,
                                                                 const std::string& balance_grade, const std::string& consistency_grade)
    : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, TITLE, wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    init(_L("Layer speeds rebalanced"));
    const int diff = original_time_seconds - optimized_time_seconds;
    if (diff > 0)
        add_line(wxString::Format(_L("Print time %s → %s (saves %s)."), seconds_text(original_time_seconds), seconds_text(optimized_time_seconds), seconds_text(diff)));
    else
        add_line(wxString::Format(_L("Print time %s → %s (slowed down to protect hot layers)."), seconds_text(original_time_seconds), seconds_text(optimized_time_seconds)));
    auto grade = [](const std::string& g) { return g == "HIGH" ? _L("High") : g == "MEDIUM" ? _L("Medium") : _L("Low"); };
    add_line(wxString::Format(_L("Thermal balance improvement: %s"), grade(balance_grade)));
    add_line(wxString::Format(_L("Thermal consistency improvement: %s"), grade(consistency_grade)));
    add_line(_L("The preview now shows the optimized G-code with its thermal index colour scheme."));
    finish(false);
}

void ThermalOptimizationResultDialog::init(const wxString& headline)
{
    SetBackgroundColour(*wxWHITE);
    m_sizer = new wxBoxSizer(wxVERTICAL);
    m_sizer->AddSpacer(FromDIP(12));
    add_line(headline, true);
}

void ThermalOptimizationResultDialog::add_line(const wxString& text, bool bold)
{
    auto* label = new Label(this, bold ? Label::Head_14 : Label::Body_13, text, LB_AUTO_WRAP);
    label->SetMinSize(wxSize(FromDIP(460), -1));
    label->Wrap(FromDIP(460));
    m_sizer->Add(label, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(bold ? 8 : 6));
}

void ThermalOptimizationResultDialog::finish(bool offer_optimize)
{
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer();
    if (offer_optimize) {
        auto* optimize = make_button(this, _L("Optimize now"), true);
        optimize->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            EndModal(wxID_OK);
            wxGetApp().plater()->CallAfter([]() {
                get_helio_pre_select_optimization_flag() = true;
                wxQueueEvent(wxGetApp().plater(), new SimpleEvent(EVT_HELIO_INPUT_DLG));
            });
        });
        buttons->Add(optimize, 0, wxRIGHT, FromDIP(8));
    }
    auto* close = make_button(this, _L("Close"), !offer_optimize);
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    buttons->Add(close, 0);
    m_sizer->Add(buttons, 0, wxALL | wxEXPAND, FromDIP(12));

    SetSizer(m_sizer);
    Layout();
    Fit();
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

}} // namespace Slic3r::GUI
