#ifndef slic3r_GUI_PrintQueue_hpp_
#define slic3r_GUI_PrintQueue_hpp_

#include <map>
#include <memory>
#include <set>
#include <string>
#include <functional>
#include <vector>
#include "nlohmann/json.hpp"

class wxTimer;
class wxWindow;
class wxEvtHandler;

namespace Slic3r { namespace GUI {

class PrintJob;
class Plater;
struct QueueProgress;

// A sliced plate waiting for its printer. Holds its own copy of the print file and
// every send option, so the project can change or close before it is sent.
struct QueuedPrint {
    std::string    id;
    std::string    dev_id;
    std::string    dev_name;
    std::string    name;          // project name as sent
    int            plate_idx = 0;
    int            bed_type  = 0; // BedType of the plate
    std::string    file;          // copy of the sliced 3mf
    std::string    config_file;   // copy of the config 3mf (cloud printers), may be empty
    bool           imported = false; // the file is a user's exported .gcode.3mf
    nlohmann::json job;           // PrintJob options (mapping, calibration, timelapse ...)
    nlohmann::json spool;         // SpoolPendingJob uses, handed to the spool tracker once sent
    long long      run_at   = 0;  // unix seconds; 0 = as soon as the plate is confirmed clear
    bool           cleared  = false; // the user confirmed the plate is clear since the printer last printed
    std::string    error;         // last send failure; the job waits for a new confirmation
    long long      added_at = 0;
};

// Jobs per printer run first in, first out. A job starts only when its printer is idle,
// the user has confirmed the plate is clear, and its start time (if any) has come.
class PrintQueue {
public:
    enum class Ready { No, NeedsConfirm, Go };

    explicit PrintQueue(std::string dir = {}); // empty dir: memory only (tests)

    void load();
    void save() const;

    std::vector<QueuedPrint>       &items() { return m_items; }
    const std::vector<QueuedPrint> &items() const { return m_items; }
    QueuedPrint                    *find(const std::string &id);

    // Copies the files into the queue folder (when it has one) and appends the job.
    QueuedPrint &add(QueuedPrint p);
    void         remove(const std::string &id); // also deletes its files
    void         move(const std::string &id, int delta);

    QueuedPrint       *front(const std::string &dev_id);
    std::set<std::string> printers() const;

    static Ready ready(const QueuedPrint &p, bool printer_idle, long long now);

    const std::string &dir() const { return m_dir; }

private:
    std::string              m_dir;
    std::vector<QueuedPrint> m_items;
};

// Watches the printers and sends queued jobs, owned by GUI_App.
class PrintQueueRunner {
public:
    explicit PrintQueueRunner(std::string dir);
    ~PrintQueueRunner();

    PrintQueue &queue() { return m_queue; }
    // Takes the options from a job built by the send dialog (not started) and queues it.
    void add(const PrintJob &job, Plater *plater, int plate_idx, const std::string &dev_name,
             nlohmann::json spool_uses, long long run_at, bool cleared);
    void confirm_cleared(const std::string &id);
    void remove(const std::string &id);
    void tick();
    bool sending(const std::string &id) const { return m_in_flight && m_in_flight_id == id; }

    static void show_dialog(wxWindow *parent);
    // Asks when a job being queued should start. False when the user cancels.
    static bool ask_start(wxWindow *parent, bool printer_idle, long long &run_at, bool &cleared);

private:
    void start(QueuedPrint &p);
    void notify(const std::string &text, const std::string &link = {}, std::function<bool(wxEvtHandler *)> cb = {}, bool error = false);

    PrintQueue                 m_queue;
    std::unique_ptr<wxTimer>   m_timer;
    std::shared_ptr<PrintJob>  m_in_flight;
    std::string                m_in_flight_id;
    std::shared_ptr<QueueProgress> m_progress;
    std::set<std::string>      m_asked; // jobs whose "plate clear?" notification is showing
};

}} // namespace Slic3r::GUI

#endif
