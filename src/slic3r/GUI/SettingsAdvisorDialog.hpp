#ifndef slic3r_SettingsAdvisorDialog_hpp_
#define slic3r_SettingsAdvisorDialog_hpp_

namespace Slic3r { namespace GUI {

// Toolbar entry: ask for the print intent, analyse the current plate in the background,
// then show the suggestions for review and apply the accepted ones.
void run_settings_advisor();

}} // namespace Slic3r::GUI

#endif // slic3r_SettingsAdvisorDialog_hpp_
