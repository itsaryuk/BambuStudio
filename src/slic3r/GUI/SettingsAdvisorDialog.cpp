#include "SettingsAdvisorDialog.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <set>

#include <wx/button.h>
#include <wx/choicdlg.h>
#include <wx/dataview.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/SettingsAdvisor.hpp"

#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "Jobs/Worker.hpp"
#include "MsgDialog.hpp"
#include "NotificationManager.hpp"
#include "ObjectDataViewModel.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "Tab.hpp"

namespace Slic3r { namespace GUI {

using namespace SettingsAdvisor;

namespace {

// Human readable option value: enum label, Yes/No for booleans, raw value otherwise.
wxString display_value(const std::string &key, const std::string &value)
{
    const ConfigOptionDef *def = print_config_def.get(key);
    if (def == nullptr)
        return from_u8(value);
    if (def->type == coBool)
        return value == "1" ? _L("Yes") : _L("No");
    for (size_t i = 0; i < def->enum_values.size() && i < def->enum_labels.size(); ++i)
        if (def->enum_values[i] == value)
            return _(def->enum_labels[i]);
    return from_u8(value) + (def->sidetext.empty() ? wxString() : " " + _(def->sidetext));
}

wxString option_label(const std::string &key)
{
    const ConfigOptionDef *def = print_config_def.get(key);
    return def == nullptr || def->label.empty() ? from_u8(key) : _(def->label);
}

class SettingsAdvisorDialog : public DPIDialog
{
public:
    SettingsAdvisorDialog(wxWindow *parent, std::vector<Suggestion> suggestions, const std::map<int, std::string> &object_names)
        : DPIDialog(parent, wxID_ANY, _L("Settings Advisor"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_suggestions(std::move(suggestions))
    {
        const int em = em_unit();
        auto *sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(new wxStaticText(this, wxID_ANY, _L("Suggested settings for this plate. Unchecked rows are left unchanged.")), 0, wxALL, em);

        m_list = new wxDataViewListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(110 * em, 35 * em));
        m_list->AppendToggleColumn(_L("Apply"), wxDATAVIEW_CELL_ACTIVATABLE, 6 * em);
        m_list->AppendTextColumn(_L("Applies to"), wxDATAVIEW_CELL_INERT, 14 * em);
        m_list->AppendTextColumn(_L("Setting"), wxDATAVIEW_CELL_INERT, 22 * em);
        m_list->AppendTextColumn(_L("Current"), wxDATAVIEW_CELL_INERT, 12 * em);
        m_list->AppendTextColumn(_L("Suggested"), wxDATAVIEW_CELL_INERT, 12 * em);
        m_list->AppendTextColumn(_L("Why"), wxDATAVIEW_CELL_INERT, 44 * em);
        for (const Suggestion &s : m_suggestions) {
            wxVector<wxVariant> row;
            row.push_back(wxVariant(true));
            auto name = object_names.find(s.object_idx);
            row.push_back(wxVariant(s.object_idx < 0 ? _L("Plate (process)") : from_u8(name != object_names.end() ? name->second : "?")));
            row.push_back(wxVariant(option_label(s.key)));
            row.push_back(wxVariant(display_value(s.key, s.current)));
            row.push_back(wxVariant(display_value(s.key, s.suggested)));
            row.push_back(wxVariant(from_u8(s.reason)));
            m_list->AppendItem(row);
        }
        sizer->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, em);

        auto *buttons    = new wxBoxSizer(wxHORIZONTAL);
        auto *select_all = new wxButton(this, wxID_ANY, _L("Select all"));
        auto *select_none = new wxButton(this, wxID_ANY, _L("Select none"));
        auto *apply_all  = new wxButton(this, wxID_ANY, _L("Apply all"));
        auto *apply_sel  = new wxButton(this, wxID_OK, _L("Apply selected"));
        auto *cancel     = new wxButton(this, wxID_CANCEL, _L("Cancel"));
        buttons->Add(select_all, 0, wxRIGHT, em / 2);
        buttons->Add(select_none, 0);
        buttons->AddStretchSpacer();
        buttons->Add(apply_all, 0, wxRIGHT, em / 2);
        buttons->Add(apply_sel, 0, wxRIGHT, em / 2);
        buttons->Add(cancel, 0);
        sizer->Add(buttons, 0, wxEXPAND | wxALL, em);

        select_all->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { set_all(true); });
        select_none->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { set_all(false); });
        apply_all->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { set_all(true); EndModal(wxID_OK); });

        SetSizerAndFit(sizer);
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
    }

    std::vector<Suggestion> selected() const
    {
        std::vector<Suggestion> out;
        for (unsigned int i = 0; i < m_list->GetItemCount(); ++i)
            if (m_list->GetToggleValue(i, 0))
                out.push_back(m_suggestions[i]);
        return out;
    }

protected:
    void on_dpi_changed(const wxRect &) override { Fit(); Refresh(); }

private:
    void set_all(bool on)
    {
        for (unsigned int i = 0; i < m_list->GetItemCount(); ++i)
            m_list->SetToggleValue(on, i, 0);
    }

    std::vector<Suggestion> m_suggestions;
    wxDataViewListCtrl     *m_list = nullptr;
};

// Process-wide keys go to the edited print preset (left unsaved), object keys become object overrides.
void apply_suggestions(const std::vector<Suggestion> &accepted)
{
    if (accepted.empty())
        return;
    Plater *plater = wxGetApp().plater();
    plater->take_snapshot("Settings Advisor");

    DynamicPrintConfig process;
    std::set<int>      touched_objects;
    Model             &model = plater->model();
    for (const Suggestion &s : accepted) {
        DynamicPrintConfig value;
        value.set_deserialize_strict(s.key, s.suggested);
        if (s.object_idx < 0) {
            process.set_key_value(s.key, value.option(s.key)->clone());
        } else if (s.object_idx < int(model.objects.size())) {
            model.objects[s.object_idx]->config.set_key_value(s.key, value.option(s.key)->clone());
            touched_objects.insert(s.object_idx);
        }
    }
    if (!process.empty())
        wxGetApp().get_tab(Preset::TYPE_PRINT)->load_config(process);

    ObjectList *obj_list = wxGetApp().obj_list();
    for (int idx : touched_objects) {
        obj_list->add_settings_item(obj_list->GetModel()->GetItemById(idx), &model.objects[idx]->config.get());
        obj_list->changed_object(idx);
    }
    plater->get_notification_manager()->push_notification(
        wxString::Format(_L("Settings Advisor: %d setting(s) applied. The process preset is modified but not saved."), int(accepted.size())).ToStdString());
    plater->schedule_background_process();
}

} // namespace

void run_settings_advisor()
{
    Plater    *plater = wxGetApp().plater();
    AppConfig *app    = wxGetApp().app_config;

    const wxString choices[] = { _L("Functional - strong parts"), _L("Visual - best looking surfaces"), _L("Fast draft - quick test print") };
    wxSingleChoiceDialog picker(plater, _L("What is this print for?"), _L("Settings Advisor"), 3, choices);
    int last = std::atoi(app->get("settings_advisor_intent").c_str());
    picker.SetSelection(std::clamp(last, 0, 2));
    if (picker.ShowModal() != wxID_OK)
        return;
    app->set("settings_advisor_intent", std::to_string(picker.GetSelection()));
    const Intent intent = Intent(picker.GetSelection());

    // Copy everything the worker needs: it must not read the live Model.
    struct Input
    {
        std::vector<indexed_triangle_set> meshes;
        std::vector<ObjectAnalysis>       objects; // name, index and overrides filled here, geometry on the worker
        Model                             model;   // private copy for the pre-slice, objects[i] <-> model.objects[i]
        DynamicPrintConfig                full;
        std::vector<Suggestion>           result;
    };
    auto input  = std::make_shared<Input>();
    input->full = wxGetApp().preset_bundle->full_config();

    Model    &model = plater->model();
    std::set<int> seen;
    for (const auto &[obj_idx, inst_idx] : plater->get_partplate_list().get_curr_plate()->get_obj_and_inst_set()) {
        if (obj_idx < 0 || obj_idx >= int(model.objects.size()) || !seen.insert(obj_idx).second)
            continue;
        const ModelObject *obj = model.objects[obj_idx];
        if (inst_idx < 0 || inst_idx >= int(obj->instances.size()))
            continue;
        TriangleMesh world;
        for (const ModelVolume *vol : obj->volumes)
            if (vol->is_model_part()) {
                TriangleMesh m = vol->mesh();
                m.transform(obj->instances[inst_idx]->get_matrix() * vol->get_matrix(), true);
                world.merge(m);
            }
        ObjectAnalysis a;
        a.name       = obj->name;
        a.object_idx = obj_idx;
        a.overrides  = obj->config.get();
        input->objects.push_back(std::move(a));
        input->meshes.push_back(std::move(world.its));
        ModelObject *copy = input->model.add_object(*obj);
        copy->clear_instances();
        copy->add_instance(*obj->instances[inst_idx]);
    }
    if (input->objects.empty()) {
        MessageDialog(plater, _L("There are no objects on the current plate."), _L("Settings Advisor"), wxOK).ShowModal();
        return;
    }

    queue_job(
        plater->get_ui_job_worker(),
        [input](JobNew::Ctl &ctl) {
            for (size_t i = 0; i < input->objects.size(); ++i) {
                ctl.update_status(int(100 * i / input->objects.size()),
                                  _u8L("Analyzing") + " " + input->objects[i].name);
                ObjectAnalysis geometry = analyze_mesh(input->meshes[i], [&ctl] { return ctl.was_canceled(); });
                if (ctl.was_canceled())
                    return;
                geometry.name       = std::move(input->objects[i].name);
                geometry.object_idx = input->objects[i].object_idx;
                geometry.overrides  = std::move(input->objects[i].overrides);
                input->objects[i]   = std::move(geometry);
            }
            // Layer outlines give exact top / unsupported areas; skipped (mesh analysis only) for huge plates.
            ctl.update_status(90, _u8L("Slicing layer outlines"));
            preslice(input->model, input->full, input->objects, [&ctl] { return ctl.was_canceled(); });
        },
        [input, intent](bool canceled, std::exception_ptr &eptr) {
            if (canceled || eptr)
                return;
            Plater *plater = wxGetApp().plater();
            input->result = suggest(input->objects, intent, input->full);
            if (input->result.empty()) {
                MessageDialog(plater, _L("Your current settings already match the suggestions for this plate."), _L("Settings Advisor"), wxOK).ShowModal();
                return;
            }
            std::map<int, std::string> names;
            for (const ObjectAnalysis &o : input->objects)
                names[o.object_idx] = o.name;
            SettingsAdvisorDialog dlg(plater, input->result, names);
            if (dlg.ShowModal() == wxID_OK)
                apply_suggestions(dlg.selected());
        });
}

}} // namespace Slic3r::GUI
