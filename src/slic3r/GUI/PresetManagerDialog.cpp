#include "PresetManagerDialog.hpp"

#include <algorithm>
#include <memory>

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choicdlg.h>
#include <wx/choice.h>
#include <wx/dataview.h>
#include <wx/listbox.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PresetBulkEdit.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Tab.hpp"

namespace Slic3r { namespace GUI {

namespace {

const Preset::Type TYPES[] = {Preset::TYPE_PRINT, Preset::TYPE_FILAMENT, Preset::TYPE_PRINTER};

PresetCollection &collection(Preset::Type type)
{
    PresetBundle *pb = wxGetApp().preset_bundle;
    return type == Preset::TYPE_FILAMENT ? pb->filaments : type == Preset::TYPE_PRINTER ? pb->printers : pb->prints;
}

// Writes a user preset the way a normal save does: only its differences from the parent, and marked for the cloud.
bool save_user_preset(PresetCollection &c, Preset &p)
{
    Preset *parent = p.inherits().empty() ? nullptr : c.find_preset(p.inherits(), false, true);
    if (!p.save(parent ? &parent->config : nullptr)) return false;
    // Never uploaded yet: keep it empty so the next sync creates it.
    if (p.sync_info != "create") p.sync_info = p.setting_id.empty() ? std::string() : "update";
    p.save_info();
    return true;
}

wxString option_label(const DynamicPrintConfig &cfg, const std::string &key)
{
    const ConfigOptionDef *def = cfg.def()->get(key);
    if (def == nullptr || def->label.empty()) return from_u8(key);
    wxString s = _(def->full_label.empty() ? def->label : def->full_label);
    // Units tell apart same-named settings: "Inner wall (mm/s)" is a speed, "Inner wall (mm/s²)" an acceleration.
    if (!def->sidetext.empty()) s += " (" + _(def->sidetext) + ")";
    return s;
}

std::string show_value(const ConfigOption *opt)
{
    if (opt == nullptr) return {};
    if (auto *vec = dynamic_cast<const ConfigOptionVectorBase *>(opt)) {
        std::vector<std::string> v = vec->vserialize();
        if (!v.empty() && std::all_of(v.begin(), v.end(), [&](const std::string &s) { return s == v.front(); })) return v.front();
    }
    return opt->serialize();
}

// Keys that bind a preset to its printer or name it; taking them from another preset would break it.
bool binding_key(const std::string &k)
{
    return boost::starts_with(k, "compatible_") || boost::ends_with(k, "_extruder_variant") || boost::ends_with(k, "_extruder_id") ||
           k == "printer_model" || k == "printer_variant" || k == "printer_technology" || boost::starts_with(k, "default_") ||
           boost::ends_with(k, "_settings_id");
}

// Presets saved in the user's preset folder; not system, project or loaded-from-file ones.
bool is_user(const Preset *p) { return p != nullptr && p->is_user() && !p->is_external; }

} // namespace

std::string rename_user_presets(PresetCollection &c, const std::map<std::string, std::string> &new_names, const std::function<void(Preset &)> &modify)
{
    PresetBundle *pb = wxGetApp().preset_bundle;
    std::vector<const Preset *>        ours;
    std::map<std::string, std::string> inherits, base_ids; // old name -> parent / parent's cloud id
    for (const auto &[from, to] : new_names) {
        const Preset *p = c.find_preset(from, false);
        if (!is_user(p)) return into_u8(wxString::Format(_L("\"%s\" is not one of your presets."), from_u8(from)));
        if (c.find_preset(to, false)) return into_u8(wxString::Format(_L("A preset named \"%s\" already exists."), from_u8(to)));
        ours.push_back(p);
        inherits[from] = p->inherits();
        base_ids[from] = p->base_id;
    }
    const std::string user_id = wxGetApp().is_user_login() ? wxGetApp().getAgent()->get_user_id() : std::string();

    std::vector<std::string> failures;
    c.clone_presets(ours, failures, [&](Preset &p, Preset::Type &) {
        const std::string  from   = p.name;
        const std::string &parent = inherits[from]; // clone_presets clears it; a rename keeps the parent
        p.name       = new_names.at(from);
        p.inherits() = new_names.count(parent) ? new_names.at(parent) : parent;
        if (modify) modify(p);
        p.sync_info = "create";
        p.user_id   = user_id;
    });
    if (!failures.empty()) return into_u8(_L("Renaming failed."));
    for (const auto &[from, to] : new_names) {
        Preset *p = c.find_preset(to, false, true);
        if (p != nullptr) // clone_presets clears base_id (the parent's cloud id); a renamed parent gets a new one when it uploads.
            p->base_id = new_names.count(inherits[from]) ? std::string() : base_ids[from];
        // Stays "create"; writes only the differences from the parent. On failure the old presets are kept.
        if (p == nullptr || !save_user_preset(c, *p))
            return into_u8(wxString::Format(_L("Could not save \"%s\". Nothing was deleted."), from_u8(to)));
    }

    // User presets that inherit from a renamed one follow it.
    std::vector<std::string> children;
    for (const Preset &p : c.get_presets())
        if (is_user(&p) && new_names.count(p.inherits()) && !new_names.count(p.name)) children.push_back(p.name);
    for (const std::string &n : children)
        if (Preset *p = c.find_preset(n, false, true)) {
            p->inherits() = new_names.at(p->inherits());
            p->base_id.clear();
            if (!save_user_preset(c, *p)) return into_u8(wxString::Format(_L("Could not save \"%s\". Nothing was deleted."), from_u8(n)));
        }

    const std::string selected = c.get_selected_preset_name();
    if (new_names.count(selected)) c.select_preset_by_name(new_names.at(selected), true); // before the old one is deleted
    for (const auto &[from, to] : new_names) {
        Preset *p = c.find_preset(from, false, true);
        if (p == nullptr) continue;
        if (!p->setting_id.empty()) {
            c.set_sync_info_and_save(p->name, p->setting_id, "delete", 0);
            wxGetApp().delete_preset_from_cloud(p->setting_id);
        }
        c.delete_preset(from);
    }
    if (&c == &pb->filaments)
        for (std::string &slot : pb->filament_presets)
            if (new_names.count(slot)) slot = new_names.at(slot);
    pb->update_compatible(PresetSelectCompatibleType::Never);
    pb->export_selections(*wxGetApp().app_config);
    BOOST_LOG_TRIVIAL(info) << "rename_user_presets: renamed " << new_names.size() << " presets";
    return {};
}

PresetManagerDialog::PresetManagerDialog(wxWindow *parent, Preset::Type type)
    : wxDialog(parent, wxID_ANY, _L("Preset Manager"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER | wxMAXIMIZE_BOX)
{
    const int gap = FromDIP(8);

    m_type = new wxChoice(this, wxID_ANY);
    m_type->Append(_L("Process"));
    m_type->Append(_L("Filament"));
    m_type->Append(_L("Printer"));
    m_type->SetSelection(type == Preset::TYPE_FILAMENT ? 1 : type == Preset::TYPE_PRINTER ? 2 : 0);
    m_filter = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(200), -1));
    m_filter->SetHint(_L("Filter by name"));
    m_printer = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(260), -1));
    m_printer->SetToolTip(_L("Show only presets that work with this printer"));
    m_system    = new wxCheckBox(this, wxID_ANY, _L("System presets"));
    m_only_diff = new wxCheckBox(this, wxID_ANY, _L("Only differences"));
    m_only_diff->SetToolTip(_L("Show every setting that differs between the ticked presets (or all shown presets when fewer than two are ticked)"));
    auto *add_col   = new wxButton(this, wxID_ANY, _L("Add column..."));
    auto *clear_col = new wxButton(this, wxID_ANY, _L("Clear columns"));

    auto *top = new wxBoxSizer(wxHORIZONTAL);
    for (wxWindow *w : std::initializer_list<wxWindow *>{m_type, m_filter, m_printer, m_system, m_only_diff, add_col, clear_col})
        top->Add(w, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, gap);

    m_list = new wxDataViewListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(1100), FromDIP(520)),
                                    wxDV_ROW_LINES | wxDV_VERT_RULES | wxDV_SINGLE);

    auto *bottom = new wxBoxSizer(wxHORIZONTAL);
    auto  button = [&](const wxString &label, const wxString &tip, std::function<void()> fn) {
        auto *b = new wxButton(this, wxID_ANY, label);
        b->SetToolTip(tip);
        b->Bind(wxEVT_BUTTON, [fn](wxCommandEvent &) { fn(); });
        bottom->Add(b, 0, wxRIGHT, gap);
        return b;
    };
    button(_L("Tick all"), _L("Tick every shown preset"), [this] { m_ticked.insert(m_rows.begin(), m_rows.end()); rebuild(); });
    button(_L("Tick none"), wxEmptyString, [this] { m_ticked.clear(); rebuild(); });
    bottom->AddSpacer(gap * 2);
    button(_L("Set value..."), _L("Change one setting in all ticked presets"), [this] { set_value(); });
    button(_L("Take values..."), _L("Copy settings from the highlighted row into the ticked presets"), [this] { take_values(); });
    button(_L("Rename..."), _L("Find and replace in the names of the ticked presets"), [this] { rename(); });
    button(_L("Delete..."), _L("Delete the ticked presets"), [this] { remove(); });
    button(_L("Copy to printers..."), _L("Copy the ticked presets to other printers"), [this] { copy_to_printers(); });
    button(_L("Find duplicates"), _L("Tick presets that are identical to another preset or to the preset they are based on"), [this] { find_duplicates(); });
    bottom->AddStretchSpacer();
    bottom->Add(new wxButton(this, wxID_CANCEL, _L("Close")));

    auto *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(top, 0, wxALL | wxEXPAND, gap);
    sizer->Add(new wxStaticText(this, wxID_ANY, _L("Tick presets to compare or change them. Click a row to use it as the source for \"Take values\". System presets are read-only.")),
               0, wxLEFT | wxRIGHT, gap);
    sizer->Add(m_list, 1, wxALL | wxEXPAND, gap);
    sizer->Add(bottom, 0, wxALL | wxEXPAND, gap);
    SetSizerAndFit(sizer);
    SetEscapeId(wxID_CANCEL);

    auto fill_printers = [this] {
        m_printer->Clear();
        m_printer->Append(_L("All printers"));
        for (const Preset &p : collection(Preset::TYPE_PRINTER).get_presets())
            if (p.is_visible && !p.is_default) m_printer->Append(from_u8(p.name));
        m_printer->SetSelection(0);
        m_printer->Enable(this->type() != Preset::TYPE_PRINTER);
    };
    fill_printers();
    m_type->Bind(wxEVT_CHOICE, [this, fill_printers](wxCommandEvent &) { m_ticked.clear(); m_columns.clear(); fill_printers(); rebuild(); });
    m_filter->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { rebuild(); });
    m_printer->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { rebuild(); });
    m_system->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { rebuild(); });
    m_only_diff->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { rebuild(); });
    add_col->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        for (const std::string &k : pick_keys(_L("Add columns"), true))
            if (std::find(m_columns.begin(), m_columns.end(), k) == m_columns.end()) m_columns.push_back(k);
        m_only_diff->SetValue(false);
        rebuild();
    });
    clear_col->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { m_columns.clear(); m_only_diff->SetValue(false); rebuild(); });
    m_list->Bind(wxEVT_DATAVIEW_ITEM_VALUE_CHANGED, [this](wxDataViewEvent &e) {
        const int row = m_list->ItemToRow(e.GetItem());
        if (row < 0 || size_t(row) >= m_rows.size() || e.GetColumn() != 0) return;
        if (m_list->GetToggleValue(row, 0)) m_ticked.insert(m_rows[row]);
        else m_ticked.erase(m_rows[row]);
        if (m_only_diff->GetValue()) CallAfter([this] { rebuild(); });
    });

    rebuild();
    wxGetApp().UpdateDlgDarkUI(this);
    CentreOnParent();
}

Preset::Type PresetManagerDialog::type() const { return TYPES[std::max(0, m_type->GetSelection())]; }

PresetCollection &PresetManagerDialog::presets() const { return collection(type()); }

void PresetManagerDialog::rebuild()
{
    PresetCollection &c = presets();

    // Rows: filter by name, printer and system.
    const Preset *printer = m_printer->GetSelection() > 0 ? collection(Preset::TYPE_PRINTER).find_preset(into_u8(m_printer->GetStringSelection()), false) : nullptr;
    const std::string filter = boost::algorithm::to_lower_copy(into_u8(m_filter->GetValue()));
    m_rows.clear();
    for (const Preset &p : c.get_presets()) {
        if (p.is_default || (p.is_system && (!p.is_visible || !m_system->GetValue()))) continue;
        if (!filter.empty() && boost::algorithm::to_lower_copy(p.name).find(filter) == std::string::npos) continue;
        if (printer != nullptr && type() != Preset::TYPE_PRINTER &&
            !is_compatible_with_printer(c.get_preset_with_vendor_profile(p), collection(Preset::TYPE_PRINTER).get_preset_with_vendor_profile(*printer)))
            continue;
        m_rows.push_back(p.name);
    }

    // Columns: the user's, or every setting that differs.
    std::vector<std::string> keys = m_columns;
    if (m_only_diff->GetValue()) {
        std::vector<const Preset *> cmp;
        for (const std::string &n : m_rows)
            if (m_ticked.count(n)) cmp.push_back(c.find_preset(n, false));
        if (cmp.size() < 2) {
            cmp.clear();
            for (const std::string &n : m_rows) cmp.push_back(c.find_preset(n, false));
        }
        std::set<std::string> diff;
        for (size_t i = 1; i < cmp.size(); ++i)
            for (const std::string &k : PresetBulkEdit::settings_diff(cmp.front()->config, cmp[i]->config)) diff.insert(k);
        keys.assign(diff.begin(), diff.end());
    }

    const DynamicPrintConfig &defaults = c.default_preset().config;
    m_list->DeleteAllItems();
    m_list->ClearColumns();
    m_list->AppendToggleColumn(wxEmptyString, wxDATAVIEW_CELL_ACTIVATABLE, FromDIP(32));
    m_list->AppendTextColumn(_L("Preset"), wxDATAVIEW_CELL_INERT, FromDIP(280), wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
    for (const std::string &k : keys)
        m_list->AppendTextColumn(option_label(defaults, k), wxDATAVIEW_CELL_INERT, FromDIP(120), wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE | wxDATAVIEW_COL_SORTABLE);
    for (const std::string &n : m_rows) {
        const Preset       *p = c.find_preset(n, false);
        wxVector<wxVariant> row;
        row.push_back(wxVariant(m_ticked.count(n) > 0));
        row.push_back(wxVariant(from_u8(p->is_system ? n + "  [" + into_u8(_L("system")) + "]" : n)));
        for (const std::string &k : keys) row.push_back(wxVariant(from_u8(show_value(p->config.option(k)))));
        m_list->AppendItem(row);
    }
    SetTitle(wxString::Format(_L("Preset Manager - %d presets, %d ticked"), int(m_rows.size()),
                              int(std::count_if(m_rows.begin(), m_rows.end(), [this](const std::string &n) { return m_ticked.count(n) > 0; }))));
}

// Ticked presets that are shown; system ones go to `system` (or are dropped) when `user_only`.
std::vector<std::string> PresetManagerDialog::ticked(bool user_only, std::vector<std::string> *system) const
{
    std::vector<std::string> out;
    for (const std::string &n : m_rows)
        if (m_ticked.count(n)) {
            if (user_only && !is_user(presets().find_preset(n, false))) {
                if (system) system->push_back(n);
            } else
                out.push_back(n);
        }
    return out;
}

std::vector<std::string> PresetManagerDialog::pick_keys(const wxString &title, bool multiple)
{
    const DynamicPrintConfig &cfg = presets().default_preset().config;
    std::vector<std::pair<wxString, std::string>> all; // label, key
    for (const std::string &k : cfg.keys()) {
        const ConfigOptionDef *def = cfg.def()->get(k);
        if (def == nullptr || def->label.empty() || k == "inherits" || boost::ends_with(k, "_settings_id")) continue;
        wxString label = option_label(cfg, k);
        if (!def->category.empty()) label += "   (" + _(def->category) + ")";
        all.emplace_back(label, k);
    }
    std::sort(all.begin(), all.end());

    wxDialog dlg(this, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto *search = new wxTextCtrl(&dlg, wxID_ANY);
    search->SetHint(_L("Search settings"));
    auto *list = new wxListBox(&dlg, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(460), FromDIP(400)), 0, nullptr, multiple ? wxLB_EXTENDED : wxLB_SINGLE);
    std::vector<std::string> shown;
    auto fill = [&] {
        const wxString f = search->GetValue().Lower();
        list->Clear();
        shown.clear();
        for (const auto &[label, key] : all)
            if (f.empty() || label.Lower().Contains(f) || from_u8(key).Contains(f)) {
                list->Append(label);
                shown.push_back(key);
            }
    };
    fill();
    search->Bind(wxEVT_TEXT, [&](wxCommandEvent &) { fill(); });
    list->Bind(wxEVT_LISTBOX_DCLICK, [&](wxCommandEvent &) { dlg.EndModal(wxID_OK); });
    auto *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(search, 0, wxALL | wxEXPAND, FromDIP(8));
    sizer->Add(list, 1, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(8));
    sizer->Add(dlg.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxALL | wxEXPAND, FromDIP(8));
    dlg.SetSizerAndFit(sizer);
    dlg.CentreOnParent();
    wxGetApp().UpdateDlgDarkUI(&dlg);
    search->SetFocus();
    if (dlg.ShowModal() != wxID_OK) return {};
    wxArrayInt sel;
    list->GetSelections(sel);
    std::vector<std::string> keys;
    for (int i : sel) keys.push_back(shown[i]);
    return keys;
}

bool PresetManagerDialog::confirm(const wxString &title, const wxString &intro, const wxString &details)
{
    wxDialog dlg(this, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto *text = new wxTextCtrl(&dlg, wxID_ANY, details, wxDefaultPosition, wxSize(FromDIP(640), FromDIP(320)), wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
    auto *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(new wxStaticText(&dlg, wxID_ANY, intro), 0, wxALL, FromDIP(8));
    sizer->Add(text, 1, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(8));
    sizer->Add(dlg.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxALL | wxEXPAND, FromDIP(8));
    dlg.SetSizerAndFit(sizer);
    dlg.CentreOnParent();
    wxGetApp().UpdateDlgDarkUI(&dlg);
    return dlg.ShowModal() == wxID_OK;
}

// After presets changed on disk: reload the edited copy if it changed, and refresh tabs and the sidebar.
void PresetManagerDialog::refresh_app(const std::string &selected_before)
{
    PresetCollection &c = presets();
    // Reselecting copies the saved preset over the edited one, so not when that holds unsaved edits.
    if (!c.current_is_dirty() && c.find_preset(selected_before, false) != nullptr) c.select_preset_by_name(selected_before, true);
    wxGetApp().preset_bundle->update_compatible(PresetSelectCompatibleType::Never);
    wxGetApp().load_current_presets(false, false);
    rebuild();
    Raise(); // reloading the presets can bring the main window to the front
}

void PresetManagerDialog::set_value()
{
    PresetCollection        &c = presets();
    std::vector<std::string> system;
    const std::vector<std::string> names = ticked(true, &system);
    if (names.empty()) {
        MessageDialog(this, _L("Tick the presets to change first. System presets can not be changed."), _L("Set value"), wxOK | wxICON_INFORMATION).ShowModal();
        return;
    }
    const std::vector<std::string> keys = pick_keys(_L("Setting to change"), false);
    if (keys.empty()) return;
    const std::string &key = keys.front();
    const wxString     label = option_label(c.default_preset().config, key);

    wxTextEntryDialog ask(this,
                          wxString::Format(_L("New value of \"%s\" for %d presets."), label, int(names.size())) + "\n\n" +
                              _L("A value sets it. +5 or -5 changes it by that amount, +10% or -10% scales it.\n"
                                 "Write =-5 to set a negative value."),
                          _L("Set value"));
    if (const Preset *first = c.find_preset(names.front(), false)) ask.SetValue(from_u8(show_value(first->config.option(key))));
    if (ask.ShowModal() != wxID_OK) return;
    const std::string expr = into_u8(ask.GetValue());

    const bool        dirty  = c.current_is_dirty();
    const std::string edited = c.get_edited_preset().name;
    std::vector<std::pair<std::string, std::unique_ptr<ConfigOption>>> changes;
    wxString details, skipped;
    for (const std::string &n : names) {
        const ConfigOption *opt = c.find_preset(n, false)->config.option(key);
        if (opt == nullptr) continue;
        if (dirty && n == edited) { skipped += "  " + from_u8(n) + "\n"; continue; }
        std::unique_ptr<ConfigOption> v(opt->clone());
        if (const std::string err = PresetBulkEdit::apply(*v, expr); !err.empty()) {
            MessageDialog(this, from_u8(err), _L("Set value"), wxOK | wxICON_WARNING).ShowModal();
            return;
        }
        if (*v == *opt) continue;
        details += from_u8(n + ":   " + show_value(opt) + "  →  " + show_value(v.get())) + "\n";
        changes.emplace_back(n, std::move(v));
    }
    if (!skipped.empty()) details += "\n" + _L("Skipped, it has unsaved changes:") + "\n" + skipped;
    if (!system.empty()) details += "\n" + wxString::Format(_L("Skipped %d system presets."), int(system.size())) + "\n";
    if (changes.empty()) {
        MessageDialog(this, _L("Nothing changes.") + "\n\n" + details, _L("Set value"), wxOK | wxICON_INFORMATION).ShowModal();
        return;
    }
    if (!confirm(_L("Set value"), wxString::Format(_L("Change \"%s\" in %d presets:"), label, int(changes.size())), details)) return;

    const std::string selected = c.get_selected_preset_name();
    wxString          failed;
    for (auto &[n, v] : changes)
        if (Preset *p = c.find_preset(n, false, true)) {
            c.lock();
            p->config.option(key)->set(v.get());
            c.unlock();
            if (!save_user_preset(c, *p)) failed += "  " + from_u8(n) + "\n";
        }
    BOOST_LOG_TRIVIAL(info) << "PresetManager: set " << key << " = " << expr << " in " << changes.size() << " presets";
    refresh_app(selected);
    if (!failed.empty()) MessageDialog(this, _L("Could not save:") + "\n" + failed, _L("Set value"), wxOK | wxICON_WARNING).ShowModal();
}

void PresetManagerDialog::take_values()
{
    PresetCollection &c   = presets();
    const int         row = m_list->GetSelectedRow();
    if (row == wxNOT_FOUND || size_t(row) >= m_rows.size()) {
        MessageDialog(this, _L("Click the row to take the values from, and tick the presets to copy them into."), _L("Take values"), wxOK | wxICON_INFORMATION).ShowModal();
        return;
    }
    const std::string        src_name = m_rows[row];
    std::vector<std::string> system, targets;
    for (const std::string &n : ticked(true, &system))
        if (n != src_name) targets.push_back(n);
    if (targets.empty()) {
        MessageDialog(this, _L("Tick the presets to copy the values into. System presets can not be changed."), _L("Take values"), wxOK | wxICON_INFORMATION).ShowModal();
        return;
    }
    const bool        dirty  = c.current_is_dirty();
    const std::string edited = c.get_edited_preset().name;
    if (dirty) targets.erase(std::remove(targets.begin(), targets.end(), edited), targets.end());
    if (targets.empty()) return;

    const Preset         &src = *c.find_preset(src_name, false);
    std::set<std::string> diff;
    for (const std::string &n : targets)
        for (const std::string &k : PresetBulkEdit::settings_diff(src.config, c.find_preset(n, false)->config))
            if (!binding_key(k)) diff.insert(k);
    if (diff.empty()) {
        MessageDialog(this, _L("The ticked presets already have the same values."), _L("Take values"), wxOK | wxICON_INFORMATION).ShowModal();
        return;
    }
    const std::vector<std::string> keys(diff.begin(), diff.end());
    wxArrayString labels;
    wxArrayInt    all;
    for (const std::string &k : keys) {
        all.Add(int(labels.size()));
        labels.Add(option_label(src.config, k) + ":   " + from_u8(show_value(src.config.option(k))));
    }
    wxMultiChoiceDialog pick(this,
                             wxString::Format(_L("Settings of \"%s\" that differ in the %d ticked presets. Untick the ones to keep."), from_u8(src_name), int(targets.size())),
                             _L("Take values"), labels);
    pick.SetSelections(all);
    if (pick.ShowModal() != wxID_OK || pick.GetSelections().empty()) return;

    const std::string        selected = c.get_selected_preset_name();
    const DynamicPrintConfig src_cfg  = src.config; // saving can move presets
    std::set<std::string>    lost;
    wxString                 failed;
    for (const std::string &n : targets) {
        Preset *p = c.find_preset(n, false, true);
        if (p == nullptr) continue;
        c.lock();
        for (int i : pick.GetSelections()) {
            const std::string  &k    = keys[i];
            const ConfigOption *from = src_cfg.option(k);
            ConfigOption       *to   = p->config.option(k, true);
            if (from == nullptr || to == nullptr) continue;
            auto *fv = dynamic_cast<const ConfigOptionVectorBase *>(from);
            auto *tv = dynamic_cast<ConfigOptionVectorBase *>(to);
            if (fv && tv && fv->size() != tv->size()) { lost.insert(k); continue; } // ponytail: no per-nozzle matching; Copy to printers has it
            to->set(from);
        }
        c.unlock();
        if (!save_user_preset(c, *p)) failed += "  " + from_u8(n) + "\n";
    }
    BOOST_LOG_TRIVIAL(info) << "PresetManager: took " << pick.GetSelections().size() << " values from " << src_name << " into " << targets.size() << " presets";
    refresh_app(selected);
    if (!failed.empty()) MessageDialog(this, _L("Could not save:") + "\n" + failed, _L("Take values"), wxOK | wxICON_WARNING).ShowModal();
    if (!lost.empty()) {
        std::vector<std::string> l(lost.begin(), lost.end());
        MessageDialog(this, _L("Not copied where the presets have a different number of extruders or nozzles:") + "\n" + from_u8(boost::join(l, ", ")),
                      _L("Take values"), wxOK | wxICON_INFORMATION).ShowModal();
    }
}

void PresetManagerDialog::rename()
{
    PresetCollection &c = presets();
    auto info = [this](const wxString &m) { MessageDialog(this, m, _L("Rename"), wxOK | wxICON_INFORMATION).ShowModal(); };
    if (type() == Preset::TYPE_PRINTER) return info(_L("Rename printers from the printer settings, because filament and process presets refer to them by name."));
    const std::vector<std::string> names = ticked(true);
    if (names.empty()) return info(_L("Tick the presets to rename first. System presets can not be renamed."));
    if (c.current_is_dirty()) return info(_L("The current preset has unsaved changes. Save or discard them first."));

    wxDialog dlg(this, wxID_ANY, _L("Rename"));
    auto *find_in = new wxTextCtrl(&dlg, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(320), -1));
    auto *repl_in = new wxTextCtrl(&dlg, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(320), -1));
    auto *grid    = new wxFlexGridSizer(2, FromDIP(8), FromDIP(8));
    grid->Add(new wxStaticText(&dlg, wxID_ANY, _L("Find")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(find_in, 1, wxEXPAND);
    grid->Add(new wxStaticText(&dlg, wxID_ANY, _L("Replace with")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(repl_in, 1, wxEXPAND);
    auto *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(new wxStaticText(&dlg, wxID_ANY, wxString::Format(_L("Replaces text in the names of %d presets."), int(names.size()))), 0, wxALL, FromDIP(12));
    sizer->Add(grid, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(12));
    sizer->Add(dlg.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxALL | wxEXPAND, FromDIP(12));
    dlg.SetSizerAndFit(sizer);
    dlg.CentreOnParent();
    wxGetApp().UpdateDlgDarkUI(&dlg);
    if (dlg.ShowModal() != wxID_OK) return;
    const std::string find = into_u8(find_in->GetValue()), repl = into_u8(repl_in->GetValue());
    if (find.empty()) return;

    std::map<std::string, std::string> new_names;
    std::set<std::string>              taken;
    wxString                           details;
    for (const std::string &n : names) {
        std::string to = boost::algorithm::trim_copy(boost::replace_all_copy(n, find, repl));
        if (to == n) continue;
        if (to.empty() || to.find_first_of("<>[]:/\\|?*\";\n\r\t\v") != std::string::npos)
            return info(wxString::Format(_L("\"%s\" is not a valid name."), from_u8(to)) + "\n" + _L("illegal characters:") + " <>[]:/\\|?*\";");
        if (c.find_preset(to, false) || !taken.insert(to).second)
            return info(wxString::Format(_L("A preset named \"%s\" already exists."), from_u8(to)));
        new_names[n] = to;
        details += from_u8(n + "  →  " + to) + "\n";
    }
    if (new_names.empty()) return info(_L("No names contain that text."));
    if (!confirm(_L("Rename"), wxString::Format(_L("Rename %d presets:"), int(new_names.size())), details)) return;

    if (const std::string err = rename_user_presets(c, new_names); !err.empty()) return info(from_u8(err));
    for (const auto &[from, to] : new_names)
        if (m_ticked.erase(from)) m_ticked.insert(to);
    refresh_app(c.get_selected_preset_name());
}

void PresetManagerDialog::remove()
{
    PresetCollection &c  = presets();
    PresetBundle     *pb = wxGetApp().preset_bundle;
    auto info = [this](const wxString &m) { MessageDialog(this, m, _L("Delete"), wxOK | wxICON_INFORMATION).ShowModal(); };
    if (type() == Preset::TYPE_PRINTER) return info(_L("Delete printers from the printer settings, because that also removes the presets that belong to them."));
    const std::vector<std::string> names = ticked(true);
    if (names.empty()) return info(_L("Tick the presets to delete first. System presets can not be deleted."));

    std::set<std::string>       in_use{c.get_selected_preset_name()};
    if (type() == Preset::TYPE_FILAMENT) in_use.insert(pb->filament_presets.begin(), pb->filament_presets.end());
    // A preset stays if it is in use, or if a preset that stays is based on it; repeat until nothing changes.
    std::set<std::string> doomed_set;
    for (const std::string &n : names)
        if (!in_use.count(n)) doomed_set.insert(n);
    std::map<std::string, wxString> why;
    for (bool changed = true; changed;) {
        changed = false;
        for (auto it = doomed_set.begin(); it != doomed_set.end();)
            if (std::any_of(c.get_presets().begin(), c.get_presets().end(),
                            [&](const Preset &p) { return p.inherits() == *it && !p.is_system && !doomed_set.count(p.name); })) {
                why[*it] = _L("other presets are based on it");
                it       = doomed_set.erase(it);
                changed  = true;
            } else
                ++it;
    }
    std::vector<std::string> doomed;
    wxString                 details, kept;
    for (const std::string &n : names) {
        if (doomed_set.count(n)) { doomed.push_back(n); details += "  " + from_u8(n) + "\n"; }
        else kept += "  " + from_u8(n) + "   (" + (in_use.count(n) ? _L("in use") : why[n]) + ")\n";
    }
    if (!kept.empty()) details += "\n" + _L("Kept:") + "\n" + kept;
    if (doomed.empty()) return info(_L("Nothing can be deleted.") + "\n\n" + details);
    if (!confirm(_L("Delete"), wxString::Format(_L("Delete %d presets? They are also deleted from your cloud account. This can not be undone."), int(doomed.size())), details))
        return;

    const std::string selected = c.get_selected_preset_name();
    // Children before their parents.
    while (!doomed.empty()) {
        auto it = std::find_if(doomed.begin(), doomed.end(), [&](const std::string &n) {
            return std::none_of(doomed.begin(), doomed.end(), [&](const std::string &o) { return c.find_preset(o, false)->inherits() == n; });
        });
        if (it == doomed.end()) it = doomed.begin();
        Preset *p = c.find_preset(*it, false, true);
        if (p != nullptr && !p->setting_id.empty()) {
            c.set_sync_info_and_save(p->name, p->setting_id, "delete", 0);
            wxGetApp().delete_preset_from_cloud(p->setting_id);
        }
        c.delete_preset(*it);
        m_ticked.erase(*it);
        doomed.erase(it);
    }
    BOOST_LOG_TRIVIAL(info) << "PresetManager: deleted presets";
    refresh_app(selected);
}

void PresetManagerDialog::copy_to_printers()
{
    if (type() == Preset::TYPE_PRINTER) {
        MessageDialog(this, _L("Copy to printers is for filament and process presets."), _L("Copy to other printers"), wxOK | wxICON_INFORMATION).ShowModal();
        return;
    }
    const std::vector<std::string> names = ticked(true);
    if (names.empty()) {
        MessageDialog(this, _L("Tick the presets to copy first."), _L("Copy to other printers"), wxOK | wxICON_INFORMATION).ShowModal();
        return;
    }
    Tab::copy_presets_to_printers(this, type(), names);
    rebuild();
}

void PresetManagerDialog::find_duplicates()
{
    PresetCollection        &c = presets();
    std::vector<const Preset *> users;
    for (const std::string &n : m_rows)
        if (const Preset *p = c.find_preset(n, false); is_user(p)) users.push_back(p);

    std::set<std::string> flagged;
    wxString              details;
    for (const Preset *p : users)
        if (const Preset *parent = p->inherits().empty() ? nullptr : c.find_preset(p->inherits(), false);
            parent != nullptr && PresetBulkEdit::settings_diff(p->config, parent->config).empty()) {
            flagged.insert(p->name);
            details += wxString::Format(_L("\"%s\" has no changes from \"%s\""), from_u8(p->name), from_u8(parent->name)) + "\n";
        }
    for (size_t i = 0; i < users.size(); ++i)
        for (size_t j = i + 1; j < users.size(); ++j)
            if (!flagged.count(users[j]->name) && !flagged.count(users[i]->name) && PresetBulkEdit::settings_diff(users[i]->config, users[j]->config).empty()) {
                flagged.insert(users[j]->name);
                details += wxString::Format(_L("\"%s\" is identical to \"%s\""), from_u8(users[j]->name), from_u8(users[i]->name)) + "\n";
            }
    if (flagged.empty()) {
        MessageDialog(this, _L("No duplicates among the shown presets."), _L("Find duplicates"), wxOK | wxICON_INFORMATION).ShowModal();
        return;
    }
    m_ticked = flagged;
    rebuild();
    MessageDialog(this, wxString::Format(_L("Ticked %d presets you may not need. Check them, then use Delete."), int(flagged.size())) + "\n\n" + details,
                  _L("Find duplicates"), wxOK | wxICON_INFORMATION).ShowModal();
}

}} // namespace Slic3r::GUI
