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
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/format.hpp>

namespace Slic3r { namespace GUI {

bool& get_helio_pre_select_optimization_flag();

std::string thermal_calibration_key(const std::string& filament_type)
{
    const auto* model = wxGetApp().preset_bundle->full_config().option<ConfigOptionString>("printer_model");
    return "cooling_" + boost::to_upper_copy(filament_type) + "@" + (model != nullptr ? model->value : std::string());
}

ThermalCalibration thermal_calibration_find(const std::string& filament_type)
{
    const AppConfig*  cfg = wxGetApp().app_config;
    ThermalCalibration c;
    const std::string own = thermal_calibration_key(filament_type);
    const std::string old = "cooling_" + boost::to_upper_copy(filament_type); // before printers were recorded
    if (cfg->has("thermal", own)) c.key = own;
    else if (cfg->has("thermal", old)) c.key = old;
    else if (cfg->has_section("thermal"))
        for (const auto& [k, v] : cfg->get_section("thermal"))
            if (boost::starts_with(k, old + "@") && !boost::ends_with(k, "_note") && !boost::ends_with(k, "_layer")) { c.key = k; c.other_printer = true; break; }
    if (!c.key.empty()) c.layer_height = float(std::atof(cfg->get("thermal", c.key + "_layer").c_str()));
    return c;
}

std::string thermal_setup_note()
{
    const DynamicPrintConfig& cfg    = wxGetApp().preset_bundle->full_config();
    const auto*               model  = cfg.option<ConfigOptionString>("printer_model");
    const auto*               nozzle = cfg.option<ConfigOptionFloats>("nozzle_diameter");
    const auto*               volume = cfg.option<ConfigOptionEnumsGeneric>("nozzle_volume_type");
    const auto*               layer  = cfg.option<ConfigOptionFloat>("layer_height");
    std::string s = model != nullptr ? model->value : std::string();
    if (nozzle != nullptr && !nozzle->values.empty())
        s += (boost::format(", %.1f mm%s nozzle") % nozzle->values.front() % (volume != nullptr && !volume->values.empty() && volume->values.front() == nvtHighFlow ? " high flow" : "")).str();
    if (layer != nullptr) s += (boost::format(", %.2f mm layers") % layer->value).str();
    return s;
}

wxString thermal_calibration_warnings()
{
    const DynamicPrintConfig& cfg   = wxGetApp().preset_bundle->full_config();
    const auto*               types = cfg.option<ConfigOptionStrings>("filament_type");
    const auto*               layer = cfg.option<ConfigOptionFloat>("layer_height");
    std::vector<std::string>  seen;
    wxString                  out;
    for (const std::string& t : types != nullptr ? types->values : std::vector<std::string>()) {
        if (std::find(seen.begin(), seen.end(), t) != seen.end()) continue;
        seen.push_back(t);
        const ThermalCalibration c    = thermal_calibration_find(t);
        const wxString           note = wxString::FromUTF8(wxGetApp().app_config->get("thermal", c.key + "_note"));
        wxString                 line;
        if (c.key.empty()) continue;
        if (c.other_printer)
            line = wxString::Format(_L("%s was calibrated on another printer (%s). Airflow differs between printers, so calibrate on this one."), wxString::FromUTF8(t), note);
        else if (layer != nullptr && c.layer_height > 0.f && (layer->value > c.layer_height * 1.5f || layer->value < c.layer_height / 1.5f))
            line = wxString::Format(_L("%s was calibrated at %.2f mm layers and this print uses %.2f mm. The model scales cooling with layer height; a tower at this height is more exact."),
                                    wxString::FromUTF8(t), c.layer_height, layer->value);
        if (!line.empty()) out += (out.empty() ? "" : "\n") + line;
    }
    return out;
}

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
                                                               const ThermalOptimizeOptions& options, bool preselect_optimize, bool plate_ready)
    : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, TITLE, wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_params(params)
    , m_options(options)
{
    SetBackgroundColour(*wxWHITE);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    const int pad = FromDIP(12), width = FromDIP(480);
    auto text = [&](const wxString& t, const wxFont& font = Label::Body_13) {
        auto* l = new Label(this, font, t, LB_AUTO_WRAP);
        l->SetMinSize(wxSize(width, -1));
        l->Wrap(width);
        return l;
    };

    sizer->Add(text(_L("Simulates how warm each layer still is when the next one lands and rates every toolpath from cold (-1) to hot (+1). Runs on this computer.")), 0, wxALL, pad);

    // What the model is working with, and whether a printed tower has tuned it.
    bool     any_calibrated = false;
    wxString mats;
    for (const ThermalModelParams& m : params) {
        if (m.material.empty()) continue;
        if (!mats.empty()) mats += "\n";
        mats += wxString::Format(m.calibrated ? _L("%s: comfort window %.0f ± %.0f °C, cooling constant %.1f s from your calibration tower.") :
                                                _L("%s: comfort window %.0f ± %.0f °C, cooling constant %.1f s from material defaults (not calibrated)."),
                                 wxString::FromUTF8(m.material), m.ideal_substrate_temp, m.window_half_width, m.cooling_time_const);
        any_calibrated |= m.calibrated;
    }
    sizer->Add(text(_L("Material model"), Label::Head_13), 0, wxLEFT | wxRIGHT, pad);
    sizer->Add(text(mats), 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(4));
    if (const wxString warn = thermal_calibration_warnings(); !warn.empty()) {
        auto* w = text(warn, Label::Body_12);
        w->SetForegroundColour(wxColour("#E07B00"));
        sizer->Add(w, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(4));
    }
    auto* calib_row = new wxBoxSizer(wxHORIZONTAL);
    calib_row->Add(text(any_calibrated ? _L("Recalibrate after changing filament, printer or fan setup.") :
                                         _L("A 15-minute tower print makes the model match this filament. Without it the ratings are estimates."), Label::Body_12), 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    auto* calibrate = make_button(this, any_calibrated ? _L("Recalibrate…") : _L("Calibrate…"), !any_calibrated);
    calibrate->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        EndModal(wxID_CANCEL);
        wxGetApp().plater()->CallAfter([]() { wxGetApp().plater()->open_thermal_calibration(); });
    });
    calib_row->Add(calibrate, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(calib_row, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, FromDIP(6));

    sizer->Add(text(_L("Environment and limits"), Label::Head_13), 0, wxLEFT | wxRIGHT | wxTOP, pad);
    auto* grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
    grid->AddGrowableCol(1);
    auto row = [&](const wxString& name, wxWindow* ctrl) {
        grid->Add(new Label(this, Label::Body_13, name), 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL);
    };
    m_chamber = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(90), -1), wxSP_ARROW_KEYS, 0, 120,
                               int(std::max(0.f, params.empty() ? 0.f : params.front().chamber_temp)));
    m_chamber->SetToolTip(_L("Prefilled from the printer and filament presets: a heated chamber, or the bed warming an enclosure."));
    row(_L("Chamber air temperature (°C, 0 = open frame)"), m_chamber);
    m_speed_up = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(90), -1), wxSP_ARROW_KEYS, 0, 100,
                                int(std::lround((options.max_speed_factor - 1.f) * 100.f)));
    row(_L("Optimize: maximum speed-up on cold layers (%)"), m_speed_up);
    m_slow_down = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(90), -1), wxSP_ARROW_KEYS, 0, 90,
                                 int(std::lround((1.f - options.min_speed_factor) * 100.f)));
    row(_L("Optimize: maximum slow-down on hot layers (%)"), m_slow_down);
    m_max_volumetric = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(90), -1), wxSP_ARROW_KEYS, 0., 100.,
                                            std::max(0.f, options.max_volumetric), 0.5);
    row(_L("Optimize: maximum volumetric speed (mm³/s, 0 = filament limit)"), m_max_volumetric);
    sizer->Add(grid, 0, wxLEFT | wxRIGHT | wxTOP | wxEXPAND, FromDIP(6));
    m_keep_outer = new wxCheckBox(this, wxID_ANY, _L("Optimize: keep outer wall speeds unchanged (preserve surface finish)"));
    m_keep_outer->SetValue(options.keep_outer_walls);
    sizer->Add(m_keep_outer, 0, wxLEFT | wxRIGHT | wxTOP, pad);
    auto* pause_row = new wxBoxSizer(wxHORIZONTAL);
    m_pause = new wxCheckBox(this, wxID_ANY, _L("Optimize: pause away from the part on layers that cannot cool in time, up to"));
    m_pause->SetValue(options.max_pause > 0.f);
    m_pause->SetToolTip(_L("Where even the slowest allowed speed leaves a layer too hot (small tips, pins), the head retracts, lifts, parks beside the part and waits the seconds the model asks for, then returns. Adds print time; a small mark can show where printing resumes."));
    pause_row->Add(m_pause, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    m_pause_cap = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(70), -1), wxSP_ARROW_KEYS, 1, 60, options.max_pause > 0.f ? int(options.max_pause) : 15);
    m_pause_cap->Enable(m_pause->GetValue());
    m_pause->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { m_pause_cap->Enable(m_pause->GetValue()); });
    pause_row->Add(m_pause_cap, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    pause_row->Add(new Label(this, Label::Body_13, _L("s per layer")), 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(pause_row, 0, wxLEFT | wxRIGHT | wxTOP, pad);

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer();
    auto* cancel   = make_button(this, _L("Cancel"), false);
    auto* analyze  = make_button(this, _L("Analyze"), !preselect_optimize);
    auto* optimize = make_button(this, _L("Optimize"), preselect_optimize);
    analyze->SetToolTip(plate_ready ? _L("Rate layer temperatures and show them in the preview. The G-code stays as sliced.") : _L("Slice the plate first."));
    optimize->SetToolTip(plate_ready ? _L("Rescale layer speeds within the limits above so every layer lands in the comfort window, then load the result.") : _L("Slice the plate first."));
    analyze->Enable(plate_ready);
    optimize->Enable(plate_ready);
    if (!plate_ready) sizer->Add(text(_L("Slice the plate to analyze or optimize it. Calibration is available any time."), Label::Body_12), 0, wxLEFT | wxRIGHT | wxTOP, pad);
    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    analyze->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_action = 0; EndModal(wxID_OK); });
    optimize->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_action = 1; EndModal(wxID_OK); });
    buttons->Add(cancel, 0, wxRIGHT, FromDIP(8));
    buttons->Add(analyze, 0, wxRIGHT, FromDIP(8));
    buttons->Add(optimize, 0);
    sizer->Add(buttons, 0, wxALL | wxEXPAND, pad);

    SetSizer(sizer);
    Layout();
    Fit();
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

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
    o.max_pause              = m_pause->GetValue() ? float(m_pause_cap->GetValue()) : 0.f;
    return o;
}

// ---------------------------------------------------------------------------------------------------------------------

ThermalCalibrationDialog::ThermalCalibrationDialog(wxWindow* parent, const State& state, std::function<void()> create_tower, std::function<wxString(float)> calibrate)
    : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, _L("Thermal calibration"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    SetBackgroundColour(*wxWHITE);
    auto* outer = new wxBoxSizer(wxVERTICAL);
    auto* body  = new wxBoxSizer(wxHORIZONTAL);
    const int width = FromDIP(460);

    wxImage photo(from_u8(resources_dir() + "/images/thermal_calibration_tower.jpg"), wxBITMAP_TYPE_JPEG);
    if (photo.IsOk()) {
        const int h = FromDIP(420);
        photo.Rescale(photo.GetWidth() * h / photo.GetHeight(), h, wxIMAGE_QUALITY_HIGH);
        body->Add(new wxStaticBitmap(this, wxID_ANY, wxBitmap(photo)), 0, wxRIGHT | wxALIGN_TOP, FromDIP(16));
    }

    auto* col  = new wxBoxSizer(wxVERTICAL);
    auto  para = [&](const wxString& t, const wxFont& font = Label::Body_13, int below = 8) {
        auto* l = new Label(this, font, t, LB_AUTO_WRAP);
        l->SetMinSize(wxSize(width, -1));
        l->Wrap(width);
        col->Add(l, 0, wxBOTTOM, FromDIP(below));
        return l;
    };
    para(wxString::Format(_L("Calibrate the thermal model for %s"), state.material), Label::Head_14, 4);
    para(state.stored.empty() ? _L("Not calibrated yet: the material defaults are in use.") : state.stored, Label::Body_12, 12);

    para(_L("1. Print the tower"), Label::Head_13, 4);
    para(_L("A cone 30 mm wide and 60 mm tall, about 15 minutes. Layer time shrinks towards the tip, so at some height the material stops cooling enough before the next layer lands."));
    if (state.slow_down_now > 0)
        para(wxString::Format(_L("Slow down layer time (Filament → Cooling) is %d s on the loaded filament. Creating the tower sets it to 0; keep it at 0 when you slice, or the slicer throttles the small layers and hides the effect."), state.slow_down_now), Label::Body_12);
    else if (state.slow_down_now == 0)
        para(_L("Slow down layer time (Filament → Cooling) is 0 on the loaded filament, as the tower needs."), Label::Body_12);
    auto* step1 = new wxBoxSizer(wxHORIZONTAL);
    step1->Add(new Label(this, Label::Body_12, state.plate_is_tower && state.plate_ready ? _L("The tower project is open and sliced.") :
                                               state.plate_is_tower ? _L("The tower project is open. Slice it, then print it.") :
                                                                      _L("Create the tower project, slice it and print it.")), 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    auto* tower = make_button(this, _L("Create tower project"), !state.plate_is_tower);
    tower->Bind(wxEVT_BUTTON, [this, create_tower](wxCommandEvent&) { EndModal(wxID_CANCEL); if (create_tower) create_tower(); });
    step1->Add(tower, 0, wxALIGN_CENTER_VERTICAL);
    col->Add(step1, 0, wxEXPAND | wxBOTTOM, FromDIP(12));

    para(_L("2. Read the print"), Label::Head_13, 4);
    para(_L("Low down the layer lines are crisp. Higher up they turn wavy and ridged, the surface loses its gloss, and the tip slumps into a blob. With a caliper, measure from the bottom of the cone (not the brim) to the lowest height where the ridges go all the way round, as the line in the photo shows. Ignore the blob at the tip."), Label::Body_13, 12);

    para(_L("3. Enter the height"), Label::Head_13, 4);
    para(_L("Open the sliced tower project (the one you printed from), enter the height and press Calibrate. The model is tuned so that layer sits at the hot edge of the comfort window, and the result is stored for this filament type."), Label::Body_13, 6);
    auto* step3 = new wxBoxSizer(wxHORIZONTAL);
    step3->Add(new Label(this, Label::Body_13, _L("Height where ridges start (mm)")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    m_height = new wxSpinCtrlDouble(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(90), -1), wxSP_ARROW_KEYS, 0., 60., 40., 0.1); // the tower is 60 mm tall
    step3->Add(m_height, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    auto* go = make_button(this, _L("Calibrate"), state.plate_ready);
    go->Enable(state.plate_ready);
    if (!state.plate_ready) go->SetToolTip(_L("Open and slice the tower project first."));
    step3->Add(go, 0, wxALIGN_CENTER_VERTICAL);
    col->Add(step3, 0, wxBOTTOM, FromDIP(8));
    m_result = para(state.plate_ready && !state.plate_is_tower ? _L("Note: the open project does not look like the calibration tower; the result is only meaningful for the tower.") : wxString(), Label::Body_12, 0);
    go->Bind(wxEVT_BUTTON, [this, calibrate](wxCommandEvent&) {
        if (!calibrate) return;
        m_result->SetLabel(calibrate(float(m_height->GetValue())));
        m_result->Wrap(FromDIP(460));
        Layout();
        Fit();
    });
    body->Add(col, 1, wxEXPAND);
    outer->Add(body, 1, wxEXPAND | wxALL, FromDIP(20));

    auto* close = make_button(this, _L("Close"), false);
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
    finish(offer, report.hot_top_z > 0.f ? report.hot_top_z : 0.f);
}

ThermalOptimizationResultDialog::ThermalOptimizationResultDialog(wxWindow* parent, int original_time_seconds, int optimized_time_seconds,
                                                                 const std::string& balance_grade, const std::string& consistency_grade,
                                                                 const ThermalOptimizeSummary& summary)
    : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, TITLE, wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    init(_L("Layer speeds rebalanced"));
    const int diff = original_time_seconds - optimized_time_seconds;
    if (diff > 0)
        add_line(wxString::Format(_L("Print time %s → %s (saves %s)."), seconds_text(original_time_seconds), seconds_text(optimized_time_seconds), seconds_text(diff)));
    else if (diff == 0)
        add_line(wxString::Format(_L("Print time unchanged (%s)."), seconds_text(original_time_seconds)));
    else
        add_line(wxString::Format(_L("Print time %s → %s (slowed down to protect hot layers)."), seconds_text(original_time_seconds), seconds_text(optimized_time_seconds)));
    auto grade = [](const std::string& g) { return g == "HIGH" ? _L("High") : g == "MEDIUM" ? _L("Medium") : _L("Low"); };
    add_line(wxString::Format(_L("Thermal balance improvement: %s"), grade(balance_grade)));
    add_line(wxString::Format(_L("Thermal consistency improvement: %s"), grade(consistency_grade)));
    if (summary.layers > 0) {
        add_line(wxString::Format(_L("Mean thermal index %.2f → %.2f, spread %.2f → %.2f. Layers slowed down: %d of %d, sped up: %d."),
                                  summary.mean_before, summary.mean_after, summary.std_before, summary.std_after, summary.slowed, summary.layers, summary.sped_up));
        if (summary.paused > 0)
            add_line(wxString::Format(_L("Cooling pauses: %d layers, %s in total, the head parked beside the part."), summary.paused, seconds_text(int(std::lround(summary.pause_total)))));
        if (std::abs(summary.mean_before) < 0.2f && summary.std_before < 0.3f)
            add_line(_L("The plate was already close to the comfort window, so there was little to rebalance; a low improvement grade is expected."));
        else if (summary.slowed == summary.layers - 1 && summary.sped_up == 0)
            add_line(_L("Every layer read hot, so all speeds were reduced as far as the limits and the preset minimum speed allow. If the plate was already slowed down by the filament's layer-time setting, little or nothing changes."));
    }
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

void ThermalOptimizationResultDialog::finish(bool offer_optimize, float cooling_tower_height)
{
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    if (cooling_tower_height > 0.f) {
        const float h     = std::ceil(cooling_tower_height + 2.f);
        auto*       tower = make_button(this, wxString::Format(_L("Add cooling tower (%.0f mm)"), h), false);
        tower->SetToolTip(_L("Adds a thin hollow cylinder beside the part, as tall as the hot region. While it prints, every small layer of the part gets that much more time to cool, with no pause and no mark. Slice again afterwards."));
        tower->Bind(wxEVT_BUTTON, [this, h](wxCommandEvent&) {
            EndModal(wxID_CANCEL);
            wxGetApp().plater()->CallAfter([h]() { wxGetApp().plater()->add_thermal_cooling_tower(h); });
        });
        buttons->Add(tower, 0, wxRIGHT, FromDIP(8));
    }
    buttons->AddStretchSpacer();
    if (offer_optimize) {
        auto* optimize = make_button(this, _L("Optimize now"), true);
        optimize->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            EndModal(wxID_OK);
            wxGetApp().plater()->CallAfter([]() {
                get_helio_pre_select_optimization_flag() = true;
                wxQueueEvent(wxGetApp().plater(), new SimpleEvent(EVT_THERMAL_INPUT_DLG));
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
