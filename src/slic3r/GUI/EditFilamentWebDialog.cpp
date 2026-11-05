#include "EditFilamentWebDialog.hpp"

#include <wx/webview.h>
#include <boost/format.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>

#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "DeviceManager.hpp"
#include "DeviceCore/DevManager.h"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "Widgets/WebView.hpp"
#include "CreatePresetsDialog.hpp"
#include "Tab.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "PresetManagerDialog.hpp"

using namespace nlohmann;

namespace Slic3r { namespace GUI {

// ── helpers ────────────────────────────────────────────────────────────────

// Same logic as EditFilamentPresetDialog::get_filament_compatible_printer
static void get_compatible_printers(const Preset &preset, std::vector<std::string> &out)
{
    auto *opt = dynamic_cast<ConfigOptionStrings *>(
        const_cast<Preset &>(preset).config.option("compatible_printers", false));
    if (opt) out = opt->values;
}

// Derive printer series label from printer preset name
static std::string printer_series(const std::string &name)
{
    if (name.find(" H2") != std::string::npos || name.find(" H3") != std::string::npos) return "H";
    if (name.find(" P1")  != std::string::npos || name.find(" P2") != std::string::npos) return "P";
    if (name.find(" X1")  != std::string::npos || name.find(" X2") != std::string::npos) return "X";
    if (name.find(" A1")  != std::string::npos || name.find(" A2") != std::string::npos) return "A";
    return "";
}

// ── Constructor / Destructor ───────────────────────────────────────────────

EditFilamentWebDialog::EditFilamentWebDialog(wxWindow *parent, const std::string &filament_id)
    : DPIDialog(parent ? parent : nullptr,
                wxID_ANY,
                _L("Edit Filament"),
                wxDefaultPosition, wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX | wxCENTRE)
    , m_filament_id(filament_id)
{
    SetBackgroundColour(*wxWHITE);

    std::string icon_path = (boost::format("%1%/images/BambuStudioTitle.ico") % resources_dir()).str();
    SetIcon(wxIcon(encode_path(icon_path.c_str()), wxBITMAP_TYPE_ICO));

    wxSize dlg_size = FromDIP(wxSize(800, 680));
    SetSize(dlg_size);
    SetMinSize(dlg_size);

    wxString url = wxString::Format(
        "file://%s/web/filament_create/edit_filament.html",
        from_u8(resources_dir()));
    url.Replace("\\", "/");
    wxString strlang = wxGetApp().current_language_code_safe();
    if (!strlang.IsEmpty()) url = wxString::Format("%s?lang=%s", url, strlang);

    m_browser = WebView::CreateWebView(this, url);
    if (!m_browser) {
        wxLogError("EditFilamentWebDialog: failed to create WebView");
        return;
    }

    wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_browser, 1, wxEXPAND);
    SetSizer(sizer);

    Bind(wxEVT_WEBVIEW_LOADED,
         &EditFilamentWebDialog::OnDocumentLoaded, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED,
         &EditFilamentWebDialog::OnScriptMessage, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_ERROR,
         &EditFilamentWebDialog::OnError, this, m_browser->GetId());

    Centre(wxBOTH);
    wxGetApp().UpdateDlgDarkUI(this);
}

EditFilamentWebDialog::~EditFilamentWebDialog()
{
    if (m_browser) { delete m_browser; m_browser = nullptr; }
}

void EditFilamentWebDialog::on_dpi_changed(const wxRect &) { Layout(); }

void EditFilamentWebDialog::run_script(const wxString &js)
{
    if (m_browser) WebView::RunScript(m_browser, js);
}

// ── Data push ─────────────────────────────────────────────────────────────

void EditFilamentWebDialog::send_filament_edit_data()
{
    PresetBundle *pb = wxGetApp().preset_bundle;

    // Collect all user presets with this filament_id
    std::map<std::string, std::vector<const Preset *>> printer_presets;

    for (const Preset &p : pb->filaments.get_presets()) {
        if (p.is_system || p.filament_id != m_filament_id) continue;
        std::vector<std::string> printers;
        get_compatible_printers(p, printers);
        for (const auto &pr : printers)
            printer_presets[pr].push_back(&p);
        // cache filament meta from first user preset
        if (m_filament_name.empty()) {
            size_t at = p.name.find(" @");
            m_filament_name = at != std::string::npos ? p.name.substr(0, at) : p.name;
            auto *vopt = dynamic_cast<ConfigOptionStrings *>(
                const_cast<Preset &>(p).config.option("filament_vendor", false));
            if (vopt && !vopt->values.empty()) m_filament_vendor = vopt->values[0];
            auto *topt = dynamic_cast<ConfigOptionStrings *>(
                const_cast<Preset &>(p).config.option("filament_type", false));
            if (topt && !topt->values.empty()) m_filament_type = topt->values[0];
        }
    }

    json presets_arr = json::array();
    for (const auto &kv : printer_presets) {
        const std::string &printer = kv.first;
        for (const Preset *fp : kv.second) {
            // base_preset_name: public name (strip " @machine" suffix)
            std::string base_name;
            // Strip " 0.4 nozzle" suffix from display name: "Polymaker ABS @BBL A1 0.4 nozzle" → "Polymaker ABS @BBL A1"
            std::string display_name = fp->name;
            size_t sp2 = display_name.rfind(' ');  // points to "nozzle"
            if (sp2 != std::string::npos) {
                size_t sp1 = display_name.rfind(' ', sp2 - 1);  // points to "0.4"
                if (sp1 != std::string::npos)
                    display_name = display_name.substr(0, sp1);
            }

            json item;
            item["printer"]          = printer;
            item["preset_name"]      = fp->name;
            item["display_name"]     = display_name;
            item["series"]           = printer_series(printer);
            presets_arr.push_back(item);
        }
    }

    json msg;
    msg["command"]       = "filament_edit_data";
    msg["filament_name"] = m_filament_name;
    msg["presets"]       = presets_arr;

    wxString js = wxString::Format("HandleStudio(%s)",
        wxString::FromUTF8(msg.dump(-1, ' ', false, json::error_handler_t::ignore)));
    run_script(js);

    BOOST_LOG_TRIVIAL(info) << "EditFilamentWebDialog::send_filament_edit_data: "
                            << m_filament_name << " presets=" << presets_arr.size();
}

// ── Rename ─────────────────────────────────────────────────────────────────

// Renames every printer's copy ("<name> @<printer>") of this custom filament and sets its
// vendor. The filament_id stays, so AMS slots and the filament list keep matching it.
void EditFilamentWebDialog::rename_filament()
{
    PresetBundle     *pb = wxGetApp().preset_bundle;
    PresetCollection &fc = pb->filaments;
    if (m_filament_name.empty()) send_filament_edit_data();
    const std::string old_name = m_filament_name;

    std::vector<const Preset *> ours;
    for (const Preset &p : fc.get_presets())
        if (!p.is_system && !p.is_default && p.filament_id == m_filament_id &&
            p.name.compare(0, old_name.size(), old_name) == 0)
            ours.push_back(&p);
    if (ours.empty()) return;

    wxDialog dlg(this, wxID_ANY, _L("Rename filament"));
    auto *vendor_in = new wxTextCtrl(&dlg, wxID_ANY, from_u8(m_filament_vendor), wxDefaultPosition, wxSize(FromDIP(320), -1));
    auto *name_in   = new wxTextCtrl(&dlg, wxID_ANY, from_u8(old_name), wxDefaultPosition, wxSize(FromDIP(320), -1));
    auto *grid = new wxFlexGridSizer(2, FromDIP(8), FromDIP(8));
    grid->Add(new wxStaticText(&dlg, wxID_ANY, _L("Vendor")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(vendor_in, 1, wxEXPAND);
    grid->Add(new wxStaticText(&dlg, wxID_ANY, _L("Name")), 0, wxALIGN_CENTER_VERTICAL);
    grid->Add(name_in, 1, wxEXPAND);
    auto *top = new wxBoxSizer(wxVERTICAL);
    top->Add(new wxStaticText(&dlg, wxID_ANY, _L("Applies to this filament on every printer.")), 0, wxALL, FromDIP(12));
    top->Add(grid, 0, wxLEFT | wxRIGHT | wxEXPAND, FromDIP(12));
    top->Add(dlg.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxALL | wxEXPAND, FromDIP(12));
    dlg.SetSizerAndFit(top);
    dlg.CentreOnParent();
    wxGetApp().UpdateDlgDarkUI(&dlg);
    if (dlg.ShowModal() != wxID_OK) return;

    auto clean = [](wxString v) {
        std::string s = into_u8(v);
        s.erase(std::remove_if(s.begin(), s.end(), [](char c) { return c == '@' || c == ';' || c == '\n' || c == '\r' || c == '\t' || c == '\v'; }), s.end());
        boost::algorithm::trim(s);
        return s;
    };
    const std::string vendor = clean(vendor_in->GetValue());
    const std::string name   = clean(name_in->GetValue());
    auto info = [this](const wxString &m) { MessageDialog(this, m, _L("Rename filament"), wxOK | wxICON_WARNING).ShowModal(); };
    if (vendor.empty() || name.empty()) return info(_L("Vendor and name can not be empty."));
    if ((vendor + name).find_first_of("<>[]:/\\|?*\"") != std::string::npos) // same set as SavePresetDialog
        return info(_L("Name is invalid;") + "\n" + _L("illegal characters:") + " <>[]:/\\|?*\"");
    if (vendor == "Bambu" || vendor == "Generic")
        return info(_L("\"Bambu\" or \"Generic\" can not be used as a Vendor for custom filaments."));
    if (name == old_name && vendor == m_filament_vendor) return;

    for (const Preset *p : ours)
        if (fc.get_edited_preset().name == p->name && fc.current_is_dirty())
            return info(_L("This filament has unsaved changes. Save or discard them first."));

    std::map<std::string, std::string> new_names; // old preset name -> new name
    for (const Preset *p : ours) {
        const std::string to = name + p->name.substr(old_name.size());
        if (to != p->name && fc.find_preset(to, false))
            return info(wxString::Format(_L("A preset named \"%s\" already exists."), from_u8(to)));
        new_names[p->name] = to;
    }

    const std::string selected = fc.get_selected_preset_name();
    std::string       new_selected;

    if (name == old_name) {
        // Vendor only: edit in place.
        for (const Preset *cp : ours) {
            Preset *p = fc.find_preset(cp->name, false);
            p->config.option<ConfigOptionStrings>("filament_vendor", true)->values = {vendor};
            p->save(nullptr);
            if (!p->setting_id.empty()) fc.set_sync_info_and_save(p->name, p->setting_id, "update", 0);
        }
        if (std::any_of(ours.begin(), ours.end(), [&](const Preset *p) { return p->name == selected; }))
            new_selected = selected; // reload the edited copy with the new vendor
    } else {
        const std::string err = rename_user_presets(fc, new_names, [&](Preset &p) {
            p.config.option<ConfigOptionStrings>("filament_vendor", true)->values = {vendor};
        });
        if (!err.empty()) return info(from_u8(err));
        if (new_names.count(selected)) new_selected = new_names[selected];
    }
    BOOST_LOG_TRIVIAL(info) << "EditFilamentWebDialog: renamed " << old_name << " -> " << name << " (vendor " << vendor << ")";

    m_filament_name.clear();
    m_filament_vendor.clear();
    send_filament_edit_data();
    if (!new_selected.empty()) {
        fc.select_preset_by_name(new_selected, true);
        if (Tab *tab = wxGetApp().get_tab(Preset::TYPE_FILAMENT)) tab->select_preset(new_selected);
    }
}

// ── Event handlers ─────────────────────────────────────────────────────────

void EditFilamentWebDialog::OnDocumentLoaded(wxWebViewEvent &evt)
{
    if (evt.GetURL() == m_browser->GetCurrentURL())
        send_filament_edit_data();
}

void EditFilamentWebDialog::OnError(wxWebViewEvent &evt)
{
    BOOST_LOG_TRIVIAL(error) << "EditFilamentWebDialog WebView error: "
                             << evt.GetString().ToUTF8().data();
}

void EditFilamentWebDialog::OnScriptMessage(wxWebViewEvent &evt)
{
    try {
        json j = json::parse(evt.GetString().ToUTF8().data());
        std::string cmd = j.value("command", "");

        BOOST_LOG_TRIVIAL(info) << "EditFilamentWebDialog command: " << cmd;

        if (cmd == "close_page" || cmd == "edit_filament_ok") {
            EndModal(wxID_OK);

        } else if (cmd == "edit_filament_edit_preset") {
            // Close this modal first and let the caller (Plater::priv::on_modify_filament)
            // open the params tab once ShowModal() has actually returned — popping another
            // top-level window while this dialog is still modal leaves both stacked in a
            // modal-within-modal state that can misbehave on some platforms.
            m_edit_preset_name = j.value("preset_name", "");
            EndModal(wxID_EDIT);

        } else if (cmd == "edit_filament_remove_preset") {
            std::string preset_name = j.value("preset_name", "");
            if (!preset_name.empty()) {
                PresetBundle *pb = wxGetApp().preset_bundle;
                Preset *p = pb->filaments.find_preset(preset_name, false);
                if (p) {
                    // Same restriction as EditFilamentPresetDialog::delete_preset(): a base
                    // preset that other presets still inherit from can not be deleted.
                    bool is_base_preset = pb->filaments.get_preset_base(*p) == p;
                    if (is_base_preset) {
                        wxString inheriting;
                        int count = 0;
                        for (const Preset &other : pb->filaments.get_presets()) {
                            if (other.inherits() == p->name) {
                                ++count;
                                inheriting += "\n - " + from_u8(other.name);
                            }
                        }
                        if (count > 0) {
                            wxString msg = _L("Presets inherited by other presets can not be deleted");
                            MessageDialog(this, msg + inheriting, _L("Delete Preset"), wxOK | wxICON_ERROR).ShowModal();
                            return;
                        }
                    }

                    wxString confirm_msg = is_base_preset
                        ? _L("Are you sure to delete the selected preset? \nIf the preset corresponds to a filament currently in use on your printer, please reset the filament information for that slot.")
                        : _L("Are you sure to delete the selected preset?");
                    if (wxID_YES == MessageDialog(this, confirm_msg, _L("Delete preset"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION).ShowModal()) {
                        // Same delete logic as EditFilamentPresetDialog
                        if (preset_name == pb->filaments.get_selected_preset_name()) {
                            wxGetApp().get_tab(Preset::Type::TYPE_FILAMENT)->delete_preset();
                        } else {
                            if (!p->setting_id.empty()) {
                                pb->filaments.set_sync_info_and_save(p->name, p->setting_id, "delete", 0);
                                wxGetApp().delete_preset_from_cloud(p->setting_id);
                            }
                            pb->filaments.delete_preset(p->name);
                        }
                        BOOST_LOG_TRIVIAL(info) << "EditFilamentWebDialog: deleted preset " << preset_name;
                    }
                }
                // Refresh data
                send_filament_edit_data();
            }

        } else if (cmd == "edit_filament_add_preset") {
            // Use the original add-preset dialog (same as EditFilamentPresetDialog)
            CreatePresetForPrinterDialog add_dlg(this, m_filament_type, m_filament_id,
                                                 m_filament_vendor, m_filament_name);
            if (add_dlg.ShowModal() == wxID_OK)
                send_filament_edit_data(); // refresh list

        } else if (cmd == "edit_filament_rename") {
            rename_filament();

        } else if (cmd == "request_filament_edit_data") {
            send_filament_edit_data();
        }
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "EditFilamentWebDialog::OnScriptMessage exception: " << e.what();
    }
}

}} // namespace Slic3r::GUI
