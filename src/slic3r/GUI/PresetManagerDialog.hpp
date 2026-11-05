#ifndef slic3r_GUI_PresetManagerDialog_hpp_
#define slic3r_GUI_PresetManagerDialog_hpp_

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <wx/dialog.h>

#include "libslic3r/Preset.hpp"

class wxCheckBox;
class wxChoice;
class wxDataViewListCtrl;
class wxTextCtrl;

namespace Slic3r { namespace GUI {

// Renames user presets (old name -> new name). Keeps each one's parent and the parent's cloud id,
// re-points user presets that inherit a renamed one, follows the selection, and deletes the old
// presets locally and in the cloud. `modify` runs on every new copy. Returns an error, empty on success.
std::string rename_user_presets(PresetCollection &presets, const std::map<std::string, std::string> &new_names,
                                const std::function<void(Preset &)> &modify = {});

// Many presets of one type side by side, with bulk edit, take values, rename, delete and copy.
class PresetManagerDialog : public wxDialog
{
public:
    PresetManagerDialog(wxWindow *parent, Preset::Type type = Preset::TYPE_PRINT);

private:
    Preset::Type      type() const;
    PresetCollection &presets() const;
    void              rebuild();
    std::vector<std::string> ticked(bool user_only, std::vector<std::string> *system = nullptr) const;
    std::vector<std::string> pick_keys(const wxString &title, bool multiple);
    bool                     confirm(const wxString &title, const wxString &intro, const wxString &details);
    void                     refresh_app(const std::string &selected_before);

    void set_value();
    void take_values();
    void rename();
    void remove();
    void copy_to_printers();
    void find_duplicates();

    wxChoice           *m_type{nullptr};
    wxTextCtrl         *m_filter{nullptr};
    wxChoice           *m_printer{nullptr};
    wxCheckBox         *m_system{nullptr};
    wxCheckBox         *m_only_diff{nullptr};
    wxDataViewListCtrl *m_list{nullptr};

    std::vector<std::string> m_rows;    // preset name per row
    std::vector<std::string> m_columns; // option keys the user added
    std::set<std::string>    m_ticked;
};

}} // namespace Slic3r::GUI

#endif
