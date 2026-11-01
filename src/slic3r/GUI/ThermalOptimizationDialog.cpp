#include "ThermalOptimizationDialog.hpp"

#include <algorithm>
#include <cmath>

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/statbmp.h>
#include <wx/panel.h>
#include <wx/radiobut.h>

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
    const DynamicPrintConfig cfg   = wxGetApp().preset_bundle->full_config(); // full_config() returns a copy; keep it alive
    const auto*              model = cfg.option<ConfigOptionString>("printer_model");
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
    const int pad = FromDIP(16), width = FromDIP(460);
    auto text = [&](wxWindow* p, const wxString& t, const wxFont& font = Label::Body_13, int w = 0) {
        auto* l = new Label(p, font, t, LB_AUTO_WRAP);
        l->SetMinSize(wxSize(w > 0 ? w : width, -1));
        l->Wrap(w > 0 ? w : width);
        return l;
    };

    // 1. What to do: two choices, each with one sentence.
    sizer->Add(text(this, _L("What do you want to do?"), Label::Head_14), 0, wxLEFT | wxRIGHT | wxTOP, pad);
    auto choice = [&](wxRadioButton* rb, const wxString& line) {
        rb->SetFont(Label::Head_13);
        sizer->Add(rb, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
        sizer->Add(text(this, line, Label::Body_12, width - FromDIP(24)), 0, wxLEFT, pad + FromDIP(24));
    };
    m_check  = new wxRadioButton(this, wxID_ANY, _L("Check temperatures"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    m_adjust = new wxRadioButton(this, wxID_ANY, _L("Adjust layer speeds"));
    choice(m_check, _L("See which layers land on material that is still too warm or already too cold. The G-code stays as sliced."));
    choice(m_adjust, _L("Slow down hot layers and speed up cold ones within safe limits, then load the adjusted G-code."));
    m_check->SetValue(!preselect_optimize);
    m_adjust->SetValue(preselect_optimize);

    // 2. How much to trust the ratings: calibration status per filament, one line each.
    sizer->AddSpacer(FromDIP(14));
    bool     any_calibrated = false, any_default = false;
    wxString status;
    for (const ThermalModelParams& m : params) {
        if (m.material.empty()) continue;
        if (!status.empty()) status += "\n";
        status += wxString::Format(m.calibrated ? _L("%s: calibrated with a printed tower.") : _L("%s: not calibrated, ratings are estimates."), wxString::FromUTF8(m.material));
        any_calibrated |= m.calibrated;
        any_default |= !m.calibrated;
    }
    auto* calib_row = new wxBoxSizer(wxHORIZONTAL);
    calib_row->Add(text(this, status, Label::Body_13, width - FromDIP(130)), 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    auto* calibrate = make_button(this, any_calibrated && !any_default ? _L("Recalibrate…") : _L("Calibrate…"), false);
    calibrate->SetToolTip(_L("A 15-minute tower print tunes the model to this filament on this printer. Recalibrate after changing filament, printer or fan setup."));
    calibrate->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        EndModal(wxID_CANCEL);
        wxGetApp().plater()->CallAfter([]() { wxGetApp().plater()->open_thermal_calibration(); });
    });
    calib_row->Add(calibrate, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(calib_row, 0, wxLEFT | wxRIGHT | wxEXPAND, pad);
    if (const wxString warn = thermal_calibration_warnings(); !warn.empty()) {
        auto* w = text(this, warn, Label::Body_12);
        w->SetForegroundColour(wxColour("#E07B00"));
        sizer->Add(w, 0, wxLEFT | wxRIGHT | wxTOP, pad);
    }

    // 3. Settings, folded: most people never need them.
    auto* toggle = new Label(this, Label::Body_13, wxString::FromUTF8("▸ ") + _L("Settings"));
    toggle->SetForegroundColour(wxColour(0, 174, 66));
    toggle->SetCursor(wxCursor(wxCURSOR_HAND));
    sizer->Add(toggle, 0, wxLEFT | wxRIGHT | wxTOP, pad);
    auto* panel = new wxPanel(this);
    panel->SetBackgroundColour(*wxWHITE);
    auto* psizer = new wxBoxSizer(wxVERTICAL);
    auto* grid   = new wxFlexGridSizer(2, FromDIP(6), FromDIP(10));
    grid->AddGrowableCol(0);
    auto row = [&](const wxString& name, wxWindow* ctrl, const wxString& tip) {
        auto* l = new Label(panel, Label::Body_13, name);
        l->SetToolTip(tip);
        ctrl->SetToolTip(tip);
        grid->Add(l, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(ctrl, 0, wxALIGN_CENTER_VERTICAL);
        return l;
    };
    m_chamber = new wxSpinCtrl(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(80), -1), wxSP_ARROW_KEYS, 0, 120,
                               int(std::max(0.f, params.empty() ? 0.f : params.front().chamber_temp)));
    row(_L("Air around the part (°C, 0 = open frame)"), m_chamber, _L("Prefilled from the printer and filament presets: a heated chamber, or the bed warming an enclosure."));
    m_speed_up = new wxSpinCtrl(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(80), -1), wxSP_ARROW_KEYS, 0, 300,
                                int(std::lround((options.max_speed_factor - 1.f) * 100.f)));
    auto* l_up = row(_L("Speed up cool layers by at most (%)"), m_speed_up, _L("Layers that cool well before the next one lands print faster, up to this much and never past the volumetric limit."));
    m_slow_down = new wxSpinCtrl(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(80), -1), wxSP_ARROW_KEYS, 0, 90,
                                 int(std::lround((1.f - options.min_speed_factor) * 100.f)));
    auto* l_down = row(_L("Slow down hot layers by at most (%)"), m_slow_down, _L("Hot layers print slower, down to this much, even below the filament's minimum speed: a layer still hot at that speed needs more time. Layers that are not hot are never slowed."));
    m_max_volumetric = new wxSpinCtrlDouble(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(80), -1), wxSP_ARROW_KEYS, 0., 100.,
                                            std::max(0.f, options.max_volumetric), 0.5);
    auto* l_vol = row(_L("Volumetric speed limit (mm³/s, 0 = filament's)"), m_max_volumetric, _L("Upper flow limit for sped-up layers. 0 uses the filament profile's maximum volumetric speed."));
    psizer->Add(grid, 0, wxEXPAND | wxTOP, FromDIP(6));
    m_keep_outer = new wxCheckBox(panel, wxID_ANY, _L("Keep outer wall speeds (surface finish stays as sliced)"));
    m_keep_outer->SetValue(options.keep_outer_walls);
    psizer->Add(m_keep_outer, 0, wxTOP, FromDIP(10));
    auto* pause_row = new wxBoxSizer(wxHORIZONTAL);
    m_pause = new wxCheckBox(panel, wxID_ANY, _L("Pause beside the part when a layer cannot cool in time, up to"));
    m_pause->SetValue(options.max_pause > 0.f);
    m_pause->SetToolTip(_L("For small tips and pins that stay hot even at the slowest speed: the head lifts, parks beside the part and waits, then returns. Adds print time; a small mark can show where printing resumes."));
    pause_row->Add(m_pause, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    m_pause_cap = new wxSpinCtrl(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(64), -1), wxSP_ARROW_KEYS, 1, 60, options.max_pause > 0.f ? int(options.max_pause) : 15);
    pause_row->Add(m_pause_cap, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    pause_row->Add(new Label(panel, Label::Body_13, _L("s per layer")), 0, wxALIGN_CENTER_VERTICAL);
    psizer->Add(pause_row, 0, wxTOP, FromDIP(8));
    // Model constants for the curious.
    wxString model;
    for (const ThermalModelParams& m : params)
        if (!m.material.empty())
            model += (model.empty() ? "" : "\n") + wxString::Format(_L("%s: comfortable at %.0f ± %.0f °C, cooling constant %.1f s."),
                                                                     wxString::FromUTF8(m.material), m.ideal_substrate_temp, m.window_half_width, m.cooling_time_const);
    auto* model_text = text(panel, model, Label::Body_12);
    model_text->SetForegroundColour(wxColour(120, 120, 120));
    psizer->Add(model_text, 0, wxTOP, FromDIP(10));
    panel->SetSizer(psizer);
    panel->Hide();
    sizer->Add(panel, 0, wxLEFT | wxRIGHT | wxEXPAND, pad);

    // Speed settings only matter when adjusting.
    auto sync = [=]() {
        const bool adjust = m_adjust->GetValue();
        for (wxWindow* w : std::initializer_list<wxWindow*>{l_up, m_speed_up, l_down, m_slow_down, l_vol, m_max_volumetric, m_keep_outer, m_pause}) w->Enable(adjust);
        m_pause_cap->Enable(adjust && m_pause->GetValue());
    };
    m_check->Bind(wxEVT_RADIOBUTTON, [=](wxCommandEvent&) { sync(); });
    m_adjust->Bind(wxEVT_RADIOBUTTON, [=](wxCommandEvent&) { sync(); });
    m_pause->Bind(wxEVT_CHECKBOX, [=](wxCommandEvent&) { sync(); });
    sync();
    toggle->Bind(wxEVT_LEFT_DOWN, [this, toggle, panel](wxMouseEvent&) {
        panel->Show(!panel->IsShown());
        toggle->SetLabel(wxString::FromUTF8(panel->IsShown() ? "▾ " : "▸ ") + _L("Settings"));
        Layout();
        Fit();
    });

    // 4. Why the buttons are disabled, when they are.
    if (!plate_ready) {
        auto* why = text(this, _L("Slice the plate first. Calibration works without a slice."), Label::Body_12);
        why->SetForegroundColour(wxColour("#E07B00"));
        sizer->Add(why, 0, wxLEFT | wxRIGHT | wxTOP, pad);
    }

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer();
    auto* cancel = make_button(this, _L("Cancel"), false);
    auto* run    = make_button(this, _L("Start"), true);
    run->Enable(plate_ready);
    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    run->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { m_action = m_adjust->GetValue() ? 1 : 0; EndModal(wxID_OK); });
    buttons->Add(cancel, 0, wxRIGHT, FromDIP(8));
    buttons->Add(run, 0);
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

namespace {
const wxColour TONE_GOOD(0, 174, 66), TONE_WARN(224, 123, 0), TONE_BAD(214, 54, 40);

wxString z_range(float lo, float hi)
{
    return lo >= 0.f && hi > lo + 0.05f ? wxString::Format(_L("between %.1f and %.1f mm"), lo, hi) : wxString::Format(_L("around %.1f mm"), std::max(lo, hi));
}
} // namespace

ThermalOptimizationResultDialog::ThermalOptimizationResultDialog(wxWindow* parent, const ThermalReport& report, int original_time_seconds,
                                                                 const std::vector<std::pair<ExtrusionRole, float>>& roles_times)
    : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, TITLE, wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    const bool good = report.outcome == "WILL_PRINT", marginal = report.outcome == "MAY_PRINT";
    init(good ? _L("Temperatures look good") : marginal ? _L("Some layers are at risk") : _L("Likely to cause defects"), good ? TONE_GOOD : marginal ? TONE_WARN : TONE_BAD);

    // The verdict in one sentence, then where.
    if (report.direction == "OVERHEATING")
        add_line(_L("Small layers do not get enough time to cool. Expect sagging, rough walls or a blob at the top."));
    else if (report.direction == "OVERCOOLING")
        add_line(_L("Layers cool too far before the next one lands. Expect weak layer bonds, warping or cracks."));
    else
        add_line(_L("Every layer lands on material at a comfortable temperature."));
    const int hot = int(std::lround(report.hot_fraction * 100)), cold = int(std::lround(report.cold_fraction * 100));
    if (hot > 0)
        add_line(wxString::Format(_L("Too hot: %d%% of the print time, %s."), hot, z_range(report.hot_bottom_z, report.hot_top_z)));
    if (cold > 0) add_line(wxString::Format(_L("Too cold: %d%% of the print time."), cold));
    add_scale();
    add_line(_L("The preview is coloured by this rating."));

    bool offer = false;
    if (report.suggested_speed_factor < 0.999f && original_time_seconds > 0) {
        float optimizable = 0.f;
        for (const auto& rt : roles_times)
            if (rt.first == erPerimeter || rt.first == erExternalPerimeter || rt.first == erInternalInfill || rt.first == erSolidInfill)
                optimizable += rt.second;
        const int saved = int(optimizable * (1.f - report.suggested_speed_factor));
        if (saved > 0) {
            offer = true;
            add_line(wxString::Format(_L("Adjusting layer speeds could also save about %s (%s → %s)."), seconds_text(saved), seconds_text(original_time_seconds),
                                      seconds_text(original_time_seconds - saved)));
        }
    }
    offer |= !good; // adjusting speeds is the first remedy for a bad rating

    // What to do, quickest first; everything else folds away.
    if (!report.fixes.empty()) {
        add_line(_L("What you can do"), true);
        int n = 0;
        for (const auto& f : report.fixes)
            if (n++ < 3) add_line(wxString::FromUTF8("• " + f.second));
    }
    std::vector<wxString> details;
    details.push_back(wxString::Format(_L("Mean thermal index %.2f, spread %.2f (−1 cold, 0 comfortable, +1 hot)."), report.mean, report.stddev));
    for (size_t i = 3; i < report.fixes.size(); ++i) details.push_back(wxString::FromUTF8("• " + report.fixes[i].second));
    for (const std::string& c : report.caveats) details.push_back(wxString::FromUTF8("• " + c));
    for (const std::string& a : report.assumptions) details.push_back(wxString::FromUTF8(a));
    add_details(details);
    finish(offer, report.hot_top_z > 0.f ? report.hot_top_z : 0.f);
}

ThermalOptimizationResultDialog::ThermalOptimizationResultDialog(wxWindow* parent, int original_time_seconds, int optimized_time_seconds,
                                                                 const std::string& balance_grade, const std::string& consistency_grade,
                                                                 const ThermalOptimizeSummary& summary)
    : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, TITLE, wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    const int  hot   = int(std::lround(summary.hot_after * 100)), cold = int(std::lround(summary.cold_after * 100));
    const bool clean = summary.layers > 0 && hot <= 2 && cold <= 2;
    init(clean ? _L("Layer speeds adjusted") : _L("Layer speeds adjusted, some layers still at risk"), clean ? TONE_GOOD : TONE_WARN);

    const int diff = original_time_seconds - optimized_time_seconds;
    if (diff > 0)
        add_line(wxString::Format(_L("Print time %s → %s, %s faster."), seconds_text(original_time_seconds), seconds_text(optimized_time_seconds), seconds_text(diff)));
    else if (diff == 0)
        add_line(wxString::Format(_L("Print time unchanged (%s)."), seconds_text(original_time_seconds)));
    else
        add_line(wxString::Format(_L("Print time %s → %s, %s slower to let hot layers cool."), seconds_text(original_time_seconds), seconds_text(optimized_time_seconds), seconds_text(-diff)));
    if (summary.layers > 0) {
        wxString what = wxString::Format(_L("%d of %d layers slowed down, %d sped up."), summary.slowed, summary.layers, summary.sped_up);
        if (summary.paused > 0) what += " " + wxString::Format(_L("Cooling pauses on %d layers, %s in total."), summary.paused, seconds_text(int(std::lround(summary.pause_total))));
        add_line(what);
    }
    if (hot > 2) {
        add_line(wxString::Format(_L("Still too hot: %d%% of the print time, %s. Speed alone cannot cool these layers."), hot, z_range(summary.hot_bottom_z, summary.hot_top_z)));
        add_line(summary.paused > 0 ? _L("Raise the pause limit in Settings, or add a cooling tower so each layer gets more time.") :
                                      _L("Turn on cooling pauses in Settings, or add a cooling tower so each layer gets more time."));
    }
    if (cold > 2) add_line(wxString::Format(_L("Still too cold: %d%% of the print time. A warmer chamber or less part cooling helps there."), cold));
    add_scale();
    add_line(_L("The preview shows the adjusted G-code. Slice again to go back to the original speeds."));

    auto grade = [](const std::string& g) { return g == "HIGH" ? _L("high") : g == "MEDIUM" ? _L("medium") : _L("low"); };
    std::vector<wxString> details;
    details.push_back(wxString::Format(_L("Mean thermal index %.2f → %.2f, spread %.2f → %.2f (−1 cold, 0 comfortable, +1 hot)."), summary.mean_before, summary.mean_after,
                                       summary.std_before, summary.std_after));
    details.push_back(wxString::Format(_L("Improvement: balance %s, consistency %s."), grade(balance_grade), grade(consistency_grade)));
    if (summary.layers > 0 && std::abs(summary.mean_before) < 0.2f && summary.std_before < 0.3f)
        details.push_back(_L("The plate was already close to comfortable, so there was little to change."));
    else if (summary.layers > 0 && summary.slowed == summary.layers - 1 && summary.sped_up == 0)
        details.push_back(_L("Every layer read hot, so all speeds went down as far as the limits and the filament's minimum speed allow. A plate the filament profile already slowed down changes little."));
    add_details(details);
    finish(false, hot > 2 && summary.hot_top_z > 0.f ? summary.hot_top_z : 0.f);
}

void ThermalOptimizationResultDialog::init(const wxString& headline, const wxColour& tone)
{
    SetBackgroundColour(*wxWHITE);
    m_sizer = new wxBoxSizer(wxVERTICAL);
    m_sizer->AddSpacer(FromDIP(16));
    auto* row = new wxBoxSizer(wxHORIZONTAL);
    auto* dot = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(12), FromDIP(12)));
    dot->SetBackgroundColour(tone);
    row->Add(dot, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    row->Add(new Label(this, Label::Head_16, headline), 0, wxALIGN_CENTER_VERTICAL);
    m_sizer->Add(row, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(16));
}

void ThermalOptimizationResultDialog::add_line(const wxString& text, bool bold)
{
    auto* label = new Label(this, bold ? Label::Head_14 : Label::Body_13, text, LB_AUTO_WRAP);
    label->SetMinSize(wxSize(FromDIP(460), -1));
    label->Wrap(FromDIP(460));
    m_sizer->Add(label, 0, wxLEFT | wxRIGHT | wxBOTTOM | (bold ? wxTOP : 0), FromDIP(bold ? 8 : 6));
}

void ThermalOptimizationResultDialog::add_scale()
{
    // Same direction as the preview legend: blue cold, green comfortable, red hot.
    const int w = FromDIP(300), h = FromDIP(10);
    wxImage   img(w, h);
    for (int x = 0; x < w; ++x) {
        const float t = float(x) / float(w - 1) * 2.f - 1.f, a = std::abs(t);
        const wxColour c0(40, 190, 70), c1 = t < 0.f ? wxColour(30, 80, 200) : wxColour(210, 45, 35);
        const unsigned char r = (unsigned char)(c0.Red() + (c1.Red() - c0.Red()) * a), g = (unsigned char)(c0.Green() + (c1.Green() - c0.Green()) * a),
                            b = (unsigned char)(c0.Blue() + (c1.Blue() - c0.Blue()) * a);
        for (int y = 0; y < h; ++y) img.SetRGB(x, y, r, g, b);
    }
    auto* col = new wxBoxSizer(wxVERTICAL);
    col->Add(new wxStaticBitmap(this, wxID_ANY, wxBitmap(img)), 0);
    auto* labels = new wxBoxSizer(wxHORIZONTAL);
    labels->SetMinSize(wxSize(w, -1));
    labels->Add(new Label(this, Label::Body_12, _L("too cold")), 0);
    labels->AddStretchSpacer();
    labels->Add(new Label(this, Label::Body_12, _L("comfortable")), 0);
    labels->AddStretchSpacer();
    labels->Add(new Label(this, Label::Body_12, _L("too hot")), 0);
    col->Add(labels, 0, wxEXPAND | wxTOP, FromDIP(2));
    m_sizer->Add(col, 0, wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, FromDIP(8));
}

void ThermalOptimizationResultDialog::add_details(const std::vector<wxString>& lines)
{
    if (lines.empty()) return;
    auto* toggle = new Label(this, Label::Body_13, wxString::FromUTF8("▸ ") + _L("Details"));
    toggle->SetForegroundColour(TONE_GOOD);
    toggle->SetCursor(wxCursor(wxCURSOR_HAND));
    m_sizer->Add(toggle, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
    auto* panel  = new wxPanel(this);
    auto* psizer = new wxBoxSizer(wxVERTICAL);
    panel->SetBackgroundColour(*wxWHITE);
    for (const wxString& t : lines) {
        auto* l = new Label(panel, Label::Body_12, t, LB_AUTO_WRAP);
        l->SetMinSize(wxSize(FromDIP(460), -1));
        l->Wrap(FromDIP(460));
        l->SetForegroundColour(wxColour(110, 110, 110));
        psizer->Add(l, 0, wxTOP, FromDIP(4));
    }
    panel->SetSizer(psizer);
    panel->Hide();
    m_sizer->Add(panel, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(8));
    toggle->Bind(wxEVT_LEFT_DOWN, [this, toggle, panel](wxMouseEvent&) {
        panel->Show(!panel->IsShown());
        toggle->SetLabel(wxString::FromUTF8(panel->IsShown() ? "▾ " : "▸ ") + _L("Details"));
        Layout();
        Fit();
    });
}

void ThermalOptimizationResultDialog::finish(bool offer_optimize, float cooling_tower_height)
{
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    if (cooling_tower_height > 0.f) {
        const float h     = std::ceil(cooling_tower_height + 2.f);
        auto*       tower = make_button(this, wxString::Format(_L("Add cooling tower (%.0f mm)"), h), false);
        tower->SetToolTip(_L("Adds a thin hollow cylinder beside the part, as tall as the hot region. While it prints, every small layer of the part gets that much more time to cool, without pausing on the part. Slice again afterwards."));
        tower->Bind(wxEVT_BUTTON, [this, h](wxCommandEvent&) {
            EndModal(wxID_CANCEL);
            wxGetApp().plater()->CallAfter([h]() { wxGetApp().plater()->add_thermal_cooling_tower(h); });
        });
        buttons->Add(tower, 0, wxRIGHT, FromDIP(8));
    }
    buttons->AddStretchSpacer();
    if (offer_optimize) {
        auto* optimize = make_button(this, _L("Adjust layer speeds…"), true);
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
