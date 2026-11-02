#ifndef slic3r_wgtSpoolUsage_h_
#define slic3r_wgtSpoolUsage_h_

#include <map>
#include <string>
#include <vector>
#include "nlohmann/json.hpp"

namespace Slic3r { namespace GUI {

class wgtFilaManagerStore;

// Grams a sent job takes from one printer slot.
struct SpoolSlotUse {
    std::string ams_id;
    std::string slot_id;
    double      grams = 0.;
};

struct SpoolPendingJob {
    std::string               dev_id;
    std::string               name;     // job (subtask) name as sent to the printer
    int                       plate = -1;
    std::vector<SpoolSlotUse> uses;
    long long                 sent_at = 0; // unix seconds
};

// Subtracts what each print used from the spools in the Filament Manager.
// A job is recorded when it is sent; when the printer reports the print finished
// the full grams come off, when it failed or was cancelled the progress share does.
// Spools with an RFID tag are left alone: the printer reports their remaining weight.
class wgtSpoolUsage {
public:
    explicit wgtSpoolUsage(wgtFilaManagerStore *store, std::string storage_path = {});

    void on_send(SpoolPendingJob job);
    // Feed every status update of every printer. Returns the ids of spools that changed.
    std::vector<std::string> on_status(const std::string &dev_id, const std::string &print_state,
                                       int percent, const std::string &subtask_name, long long now);

    const std::vector<SpoolPendingJob> &pending() const { return m_jobs; }

    void load();
    void save() const;

private:
    struct PrinterState { std::string state; int percent = 0; };

    std::vector<std::string> deduct(const SpoolPendingJob &job, double share);

    wgtFilaManagerStore                *m_store;
    std::string                         m_path;
    std::vector<SpoolPendingJob>        m_jobs;
    std::map<std::string, PrinterState> m_printers;
};

}} // namespace Slic3r::GUI

#endif
