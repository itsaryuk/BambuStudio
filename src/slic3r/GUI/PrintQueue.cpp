#include "PrintQueue.hpp"

#include <algorithm>
#include <atomic>
#include <ctime>
#include <fstream>
#include <mutex>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/timer.h>

#include <wx/datectrl.h>
#include <wx/dateevt.h>
#include <wx/timectrl.h>
#include <wx/radiobut.h>
#include <wx/dialog.h>
#include <wx/listctrl.h>
#include <wx/radiobut.h>
#include <wx/checkbox.h>
#include <wx/sizer.h>
#include <wx/button.h>
#include <wx/stattext.h>
#include <wx/msgdlg.h>
#include <wx/datetime.h>

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "format.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "PartPlate.hpp"
#include "NotificationManager.hpp"
#include "Jobs/PrintJob.hpp"
#include "Jobs/ProgressIndicator.hpp"
#include "DeviceCore/DevManager.h"
#include "DeviceCore/DevStorage.h"
#include "DeviceManager.hpp"
#include "fila_manager/wgtSpoolUsage.h"

namespace fs = boost::filesystem;

namespace Slic3r { namespace GUI {

// ---------------------------------------------------------------- PrintQueue

PrintQueue::PrintQueue(std::string dir) : m_dir(std::move(dir)) {}

QueuedPrint *PrintQueue::find(const std::string &id)
{
    auto it = std::find_if(m_items.begin(), m_items.end(), [&](const QueuedPrint &p) { return p.id == id; });
    return it == m_items.end() ? nullptr : &*it;
}

QueuedPrint &PrintQueue::add(QueuedPrint p)
{
    static int counter = 0;
    p.added_at = p.added_at ? p.added_at : (long long) std::time(nullptr);
    p.id       = std::to_string(p.added_at) + "_" + std::to_string(++counter);
    if (!m_dir.empty()) {
        fs::create_directories(m_dir);
        auto copy = [&](std::string &f, const char *suffix) {
            if (f.empty() || !fs::exists(f)) { f.clear(); return; }
            const std::string dst = (fs::path(m_dir) / (p.id + suffix)).string();
            fs::copy_file(f, dst, fs::copy_option::overwrite_if_exists);
            f = dst;
        };
        copy(p.file, ".3mf");
        copy(p.config_file, "_config.3mf");
    }
    m_items.push_back(std::move(p));
    save();
    return m_items.back();
}

void PrintQueue::remove(const std::string &id)
{
    auto it = std::find_if(m_items.begin(), m_items.end(), [&](const QueuedPrint &p) { return p.id == id; });
    if (it == m_items.end()) return;
    if (!m_dir.empty()) {
        boost::system::error_code ec;
        for (const std::string &f : {it->file, it->config_file})
            if (!f.empty() && fs::path(f).parent_path() == fs::path(m_dir)) fs::remove(f, ec);
    }
    m_items.erase(it);
    save();
}

void PrintQueue::move(const std::string &id, int delta)
{
    auto it = std::find_if(m_items.begin(), m_items.end(), [&](const QueuedPrint &p) { return p.id == id; });
    if (it == m_items.end()) return;
    // swap with the neighbour of the same printer, so order between printers does not matter
    const std::string dev = it->dev_id;
    if (delta < 0) {
        auto rit = std::find_if(std::make_reverse_iterator(it), m_items.rend(), [&](const QueuedPrint &p) { return p.dev_id == dev; });
        if (rit != m_items.rend()) std::iter_swap(it, rit);
    } else {
        auto nit = std::find_if(std::next(it), m_items.end(), [&](const QueuedPrint &p) { return p.dev_id == dev; });
        if (nit != m_items.end()) std::iter_swap(it, nit);
    }
    save();
}

QueuedPrint *PrintQueue::front(const std::string &dev_id)
{
    auto it = std::find_if(m_items.begin(), m_items.end(), [&](const QueuedPrint &p) { return p.dev_id == dev_id; });
    return it == m_items.end() ? nullptr : &*it;
}

std::set<std::string> PrintQueue::printers() const
{
    std::set<std::string> s;
    for (const QueuedPrint &p : m_items) s.insert(p.dev_id);
    return s;
}

PrintQueue::Ready PrintQueue::ready(const QueuedPrint &p, bool printer_idle, long long now)
{
    if (!printer_idle || !p.error.empty()) return Ready::No;
    if (!p.cleared) return Ready::NeedsConfirm;
    return p.run_at == 0 || now >= p.run_at ? Ready::Go : Ready::No;
}

void PrintQueue::load()
{
    if (m_dir.empty()) return;
    const fs::path path = fs::path(m_dir) / "queue.json";
    if (!fs::exists(path)) return;
    try {
        std::ifstream  in(path.string());
        nlohmann::json j = nlohmann::json::parse(in);
        m_items.clear();
        for (const auto &e : j.value("jobs", nlohmann::json::array())) {
            QueuedPrint p;
            p.id          = e.value("id", "");
            p.dev_id      = e.value("dev_id", "");
            p.dev_name    = e.value("dev_name", "");
            p.name        = e.value("name", "");
            p.plate_idx   = e.value("plate_idx", 0);
            p.bed_type    = e.value("bed_type", 0);
            p.file        = e.value("file", "");
            p.config_file = e.value("config_file", "");
            p.imported    = e.value("imported", false);
            p.job         = e.value("job", nlohmann::json::object());
            p.spool       = e.value("spool", nlohmann::json::array());
            p.run_at      = e.value("run_at", 0LL);
            p.error       = e.value("error", "");
            p.added_at    = e.value("added_at", 0LL);
            // a confirmation does not survive a restart: the plate may have changed meanwhile
            if (!p.id.empty() && fs::exists(p.file)) m_items.push_back(std::move(p));
        }
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "print queue: cannot read " << path << ": " << e.what();
    }
}

void PrintQueue::save() const
{
    if (m_dir.empty()) return;
    nlohmann::json jobs = nlohmann::json::array();
    for (const QueuedPrint &p : m_items)
        jobs.push_back({{"id", p.id}, {"dev_id", p.dev_id}, {"dev_name", p.dev_name}, {"name", p.name}, {"plate_idx", p.plate_idx},
                        {"bed_type", p.bed_type}, {"file", p.file}, {"config_file", p.config_file}, {"imported", p.imported},
                        {"job", p.job}, {"spool", p.spool}, {"run_at", p.run_at}, {"error", p.error}, {"added_at", p.added_at}});
    try {
        fs::create_directories(m_dir);
        const fs::path path = fs::path(m_dir) / "queue.json";
        const std::string tmp = path.string() + ".tmp";
        { std::ofstream out(tmp); out << nlohmann::json{{"jobs", jobs}}.dump(1); }
        fs::rename(tmp, path);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "print queue: cannot write " << m_dir << ": " << e.what();
    }
}

// ---------------------------------------------------------- PrintQueueRunner

// Records what a queued send reports; nothing is drawn.
struct QueueProgress : ProgressIndicator {
    std::mutex  mtx;
    std::string status, error;
    void clear_percent() override {}
    void show_error_info(wxString msg, int, wxString, wxString) override { std::lock_guard<std::mutex> l(mtx); error = msg.ToUTF8().data(); }
    void set_range(int) override {}
    void set_cancel_callback(CancelFn) override {}
    void set_progress(int) override {}
    void set_status_text(const char *s) override { std::lock_guard<std::mutex> l(mtx); if (s && *s) status = s; }
    int  get_range() const override { return 100; }
};

static const int EVT_QUEUED_PRINT_SENT = wxNewEventType(); // keeps the plater from switching to the Device tab

static nlohmann::json job_options(const PrintJob &j)
{
    return {{"project_name", j.m_project_name}, {"print_type", j.m_print_type},
            {"nozzle_mapping", j.task_nozzle_mapping}, {"ams_mapping", j.task_ams_mapping}, {"ams_mapping2", j.task_ams_mapping2},
            {"ams_mapping_info", j.task_ams_mapping_info}, {"nozzles_info", j.task_nozzles_info},
            {"bed_leveling", j.task_bed_leveling}, {"flow_cali", j.task_flow_cali}, {"vibration_cali", j.task_vibration_cali},
            {"record_timelapse", j.task_record_timelapse}, {"layer_inspect", j.task_layer_inspect}, {"use_ams", j.task_use_ams},
            {"ext_change_assist", j.task_ext_change_assist}, {"timelapse_use_internal", j.task_timelapse_use_internal},
            {"auto_bed_leveling", j.auto_bed_leveling}, {"auto_flow_cali", j.auto_flow_cali}, {"auto_offset_cali", j.auto_offset_cali},
            {"extruder_cali_manual_mode", j.extruder_cali_manual_mode}};
}

static void apply_options(PrintJob &j, const nlohmann::json &o)
{
    j.m_project_name               = o.value("project_name", "");
    j.m_print_type                 = o.value("print_type", "from_normal");
    j.task_nozzle_mapping          = o.value("nozzle_mapping", "");
    j.task_ams_mapping             = o.value("ams_mapping", "");
    j.task_ams_mapping2            = o.value("ams_mapping2", "");
    j.task_ams_mapping_info        = o.value("ams_mapping_info", "");
    j.task_nozzles_info            = o.value("nozzles_info", "");
    j.task_bed_leveling            = o.value("bed_leveling", true);
    j.task_flow_cali               = o.value("flow_cali", false);
    j.task_vibration_cali          = o.value("vibration_cali", false);
    j.task_record_timelapse        = o.value("record_timelapse", false);
    j.task_layer_inspect           = o.value("layer_inspect", true);
    j.task_use_ams                 = o.value("use_ams", true);
    j.task_ext_change_assist       = o.value("ext_change_assist", false);
    j.task_timelapse_use_internal  = o.value("timelapse_use_internal", false);
    j.auto_bed_leveling            = o.value("auto_bed_leveling", 0);
    j.auto_flow_cali               = o.value("auto_flow_cali", 0);
    j.auto_offset_cali             = o.value("auto_offset_cali", 0);
    j.extruder_cali_manual_mode    = o.value("extruder_cali_manual_mode", -1);
}

PrintQueueRunner::PrintQueueRunner(std::string dir) : m_queue(std::move(dir)), m_timer(std::make_unique<wxTimer>())
{
    m_queue.load();
    m_timer->Bind(wxEVT_TIMER, [this](wxTimerEvent &) { tick(); });
    m_timer->Start(5000);
}

PrintQueueRunner::~PrintQueueRunner()
{
    m_timer->Stop();
    if (m_in_flight) m_in_flight->join();
}

void PrintQueueRunner::add(const PrintJob &job, Plater *plater, int plate_idx, const std::string &dev_name,
                           nlohmann::json spool_uses, long long run_at, bool cleared)
{
    QueuedPrint p;
    p.dev_id    = job.get_dev_id();
    p.dev_name  = dev_name;
    p.name      = job.m_project_name;
    p.plate_idx = plate_idx;
    if (PartPlate *plate = plater->get_partplate_list().get_plate(plate_idx))
        p.bed_type = int(plate->get_bed_type(true));
    p.imported = !job.m_imported_3mf_path.empty();
    if (p.imported) {
        p.file = job.m_imported_3mf_path;
    } else {
        PrintPrepareData data;
        plater->get_print_job_data(&data);
        p.file        = data._3mf_path.string();
        p.config_file = job.connection_type == "lan" ? std::string() : data._3mf_config_path.string();
    }
    p.job     = job_options(job);
    p.spool   = std::move(spool_uses);
    p.run_at  = run_at;
    p.cleared = cleared;
    try {
        m_queue.add(std::move(p));
    } catch (const std::exception &e) {
        notify(std::string(_u8L("Could not queue the print: ")) + e.what(), {}, {}, true);
        return;
    }
    notify(_u8L("Added to the print queue."), _u8L("Open print queue"),
           [](wxEvtHandler *) { show_dialog(wxGetApp().mainframe); return true; });
    tick();
}

void PrintQueueRunner::confirm_cleared(const std::string &id)
{
    if (QueuedPrint *p = m_queue.find(id)) {
        p->cleared = true;
        p->error.clear();
        m_asked.erase(id);
        m_queue.save();
        tick();
    }
}

void PrintQueueRunner::remove(const std::string &id)
{
    if (sending(id)) return;
    m_asked.erase(id);
    m_queue.remove(id);
}

void PrintQueueRunner::notify(const std::string &text, const std::string &link, std::function<bool(wxEvtHandler *)> cb, bool error)
{
    if (!wxGetApp().plater()) return;
    wxGetApp().plater()->get_notification_manager()->push_notification(
        NotificationType::CustomNotification,
        error ? NotificationManager::NotificationLevel::ErrorNotificationLevel : NotificationManager::NotificationLevel::ImportantNotificationLevel,
        text, link, cb);
}

static bool printer_idle(MachineObject *obj)
{
    return obj && obj->is_connected() && obj->is_info_ready() && !obj->print_status.empty() && !obj->is_in_printing() &&
           !obj->is_in_upgrading() && !obj->is_system_printing();
}

void PrintQueueRunner::tick()
{
    if (wxGetApp().is_closing() || !wxGetApp().mainframe) return;
    // a send in progress: wait for it, then drop the job or keep it with the error
    if (m_in_flight) {
        if (m_in_flight->is_running() || !m_in_flight->is_finalized()) return;
        std::string error;
        {
            std::lock_guard<std::mutex> l(m_progress->mtx);
            error = m_progress->error;
            if (error.empty() && !m_in_flight->is_finished())
                error = m_progress->status.empty() ? _u8L("The print was not sent.") : m_progress->status;
        }
        const std::string id = m_in_flight_id;
        m_in_flight.reset();
        m_in_flight_id.clear();
        if (QueuedPrint *p = m_queue.find(id)) {
            if (error.empty()) {
                BOOST_LOG_TRIVIAL(info) << "print queue: sent " << p->name << " to " << p->dev_id;
                if (auto *usage = wxGetApp().spool_usage(); usage && p->spool.is_array() && !p->spool.empty()) {
                    SpoolPendingJob sj;
                    sj.dev_id  = p->dev_id;
                    sj.name    = p->name;
                    sj.plate   = p->plate_idx;
                    sj.sent_at = (long long) std::time(nullptr);
                    for (const auto &u : p->spool)
                        sj.uses.push_back({u.value("ams_id", ""), u.value("slot_id", ""), u.value("grams", 0.)});
                    usage->on_send(std::move(sj));
                }
                notify(GUI::format(_u8L("Queued print \"%1%\" sent to %2%."), p->name, p->dev_name));
                m_queue.remove(id);
            } else {
                BOOST_LOG_TRIVIAL(warning) << "print queue: sending " << p->name << " failed: " << error;
                p->error   = error;
                p->cleared = false;
                m_queue.save();
                notify(GUI::format(_u8L("Queued print \"%1%\" was not sent: %2%"), p->name, error), _u8L("Open print queue"),
                       [](wxEvtHandler *) { show_dialog(wxGetApp().mainframe); return true; }, true);
            }
        }
    }

    DeviceManager *dev = wxGetApp().getDeviceManager();
    if (!dev) return;
    const long long now = (long long) std::time(nullptr);
    for (const std::string &dev_id : m_queue.printers()) {
        QueuedPrint   *p   = m_queue.front(dev_id);
        MachineObject *obj = dev->get_my_machine(dev_id);
        if (!p) continue;
        if (obj && obj->is_in_printing()) {
            // something is printing: the plate will hold it afterwards, so earlier confirmations are void
            bool changed = false;
            for (QueuedPrint &q : m_queue.items())
                if (q.dev_id == dev_id && q.cleared) { q.cleared = false; changed = true; }
            if (changed) m_queue.save();
        }
        switch (PrintQueue::ready(*p, printer_idle(obj), now)) {
        case PrintQueue::Ready::NeedsConfirm:
            if (m_asked.insert(p->id).second) {
                const std::string id = p->id;
                notify(GUI::format(_u8L("%1% is free. Clear the plate to start the queued print \"%2%\"."), p->dev_name, p->name),
                       _u8L("The plate is clear, start"), [this, id](wxEvtHandler *) { confirm_cleared(id); return true; });
            }
            break;
        case PrintQueue::Ready::Go:
            if (!m_in_flight) start(*p);
            break;
        default: break;
        }
    }
}

void PrintQueueRunner::start(QueuedPrint &p)
{
    MachineObject *obj    = wxGetApp().getDeviceManager()->get_my_machine(p.dev_id);
    Plater        *plater = wxGetApp().plater();
    if (!obj || !plater) return;

    m_progress = std::make_shared<QueueProgress>();
    auto job   = std::make_shared<PrintJob>(m_progress, plater, p.dev_id);
    apply_options(*job, p.job);
    // the connection may have changed since the job was queued
    job->m_dev_ip        = obj->get_dev_ip();
    job->m_ftp_folder    = obj->get_ftp_folder();
    job->m_access_code   = obj->get_access_code();
    job->m_local_use_ssl_for_ftp  = obj->local_use_ssl_for_ftp;
    job->m_local_use_ssl_for_mqtt = obj->local_use_ssl_for_mqtt;
    job->connection_type  = obj->connection_type();
    job->cloud_print_only = obj->is_support_cloud_print_only;
    job->has_sdcard       = obj->GetStorage()->get_sdcard_state() == DevStorage::SdcardState::HAS_SDCARD_NORMAL;
    job->could_emmc_print = obj->is_support_print_with_emmc;
    // the file is our own copy; the plater's current plates have nothing to do with it
    if (p.imported) {
        job->set_imported_3mf_path(p.file);
    } else {
        job->job_data.is_from_plater    = false;
        job->job_data._3mf_path         = p.file;
        job->job_data._3mf_config_path  = p.config_file;
    }
    job->job_data.plate_idx         = p.plate_idx;
    job->plate_data.is_valid        = true;
    job->plate_data.plate_count     = p.plate_idx + 1;
    job->plate_data.cur_plate_index = p.plate_idx + 1;
    job->plate_data.bed_type        = BedType(p.bed_type);
    job->set_print_job_finished_event(EVT_QUEUED_PRINT_SENT);
    auto progress = m_progress;
    job->on_check_ip_address_fail([progress]() {
        std::lock_guard<std::mutex> l(progress->mtx);
        progress->error = _u8L("The printer's access code or IP address is not valid.");
    });

    BOOST_LOG_TRIVIAL(info) << "print queue: sending " << p.name << " plate " << p.plate_idx + 1 << " to " << p.dev_id;
    m_in_flight    = job;
    m_in_flight_id = p.id;
    notify(GUI::format(_u8L("Sending queued print \"%1%\" to %2%."), p.name, p.dev_name));
    job->start();
}

// ------------------------------------------------------------------ dialogs

static wxString start_text(const QueuedPrint &p)
{
    wxString when = p.run_at ? wxDateTime((time_t) p.run_at).Format("%a %d %b %H:%M") : _L("when the plate is clear");
    return when;
}

void PrintQueueRunner::show_dialog(wxWindow *parent)
{
    PrintQueueRunner *runner = wxGetApp().print_queue();
    if (!runner) return;
    wxDialog dlg(parent, wxID_ANY, _L("Print Queue"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    auto *list = new wxListView(&dlg, wxID_ANY, wxDefaultPosition, wxSize(dlg.FromDIP(720), dlg.FromDIP(260)), wxLC_REPORT | wxLC_SINGLE_SEL);
    list->AppendColumn(_L("Printer"), wxLIST_FORMAT_LEFT, dlg.FromDIP(140));
    list->AppendColumn(_L("Print"), wxLIST_FORMAT_LEFT, dlg.FromDIP(220));
    list->AppendColumn(_L("Start"), wxLIST_FORMAT_LEFT, dlg.FromDIP(130));
    list->AppendColumn(_L("Status"), wxLIST_FORMAT_LEFT, dlg.FromDIP(220));
    auto *btn_clear  = new wxButton(&dlg, wxID_ANY, _L("Plate is clear"));
    auto *btn_up     = new wxButton(&dlg, wxID_ANY, _L("Up"));
    auto *btn_down   = new wxButton(&dlg, wxID_ANY, _L("Down"));
    auto *btn_remove = new wxButton(&dlg, wxID_ANY, _L("Remove"));
    auto *btn_close  = new wxButton(&dlg, wxID_CANCEL, _L("Close"));

    std::vector<std::string> ids;
    auto selected = [&]() -> std::string { long i = list->GetFirstSelected(); return i >= 0 && size_t(i) < ids.size() ? ids[i] : std::string(); };
    auto refresh = [&]() {
        const std::string keep = selected();
        list->DeleteAllItems();
        ids.clear();
        std::set<std::string> seen;
        for (const QueuedPrint &p : runner->queue().items()) {
            const bool first = seen.insert(p.dev_id).second;
            wxString status = runner->sending(p.id) ? _L("Sending")
                            : !p.error.empty()      ? wxString::FromUTF8(p.error)
                            : !first                ? _L("Waiting for the jobs before it")
                            : p.cleared             ? _L("Plate clear, waiting for the printer or start time")
                                                    : _L("Waiting for the plate to be cleared");
            long row = list->InsertItem(list->GetItemCount(), wxString::FromUTF8(p.dev_name.empty() ? p.dev_id : p.dev_name));
            list->SetItem(row, 1, wxString::Format("%s  (%s %d)", wxString::FromUTF8(p.name), _L("plate"), p.plate_idx + 1));
            list->SetItem(row, 2, start_text(p));
            list->SetItem(row, 3, status);
            ids.push_back(p.id);
            if (p.id == keep) list->Select(row);
        }
    };
    refresh();

    btn_clear->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) {
        const std::string id = selected();
        QueuedPrint *p = runner->queue().find(id);
        if (!p) return;
        if (runner->queue().front(p->dev_id) != p) {
            wxMessageBox(_L("Only the next print of a printer can be started."), _L("Print Queue"), wxOK | wxICON_INFORMATION, &dlg);
            return;
        }
        runner->confirm_cleared(id);
        refresh();
    });
    btn_up->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { runner->queue().move(selected(), -1); refresh(); });
    btn_down->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { runner->queue().move(selected(), +1); refresh(); });
    btn_remove->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { runner->remove(selected()); refresh(); });
    wxTimer timer(&dlg);
    dlg.Bind(wxEVT_TIMER, [&](wxTimerEvent &) { refresh(); });
    timer.Start(3000);

    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    for (wxButton *b : {btn_clear, btn_up, btn_down, btn_remove}) buttons->Add(b, 0, wxRIGHT, dlg.FromDIP(6));
    buttons->AddStretchSpacer();
    buttons->Add(btn_close);
    auto *top = new wxBoxSizer(wxVERTICAL);
    auto *intro = new wxStaticText(&dlg, wxID_ANY, _L("Each printer prints its jobs in order. A job starts when the printer is free and you confirm the plate is clear; a job with a start time waits for that time too. Bambu Studio has to be running."));
    intro->Wrap(dlg.FromDIP(700));
    top->Add(intro, 0, wxALL | wxEXPAND, dlg.FromDIP(10));
    top->Add(list, 1, wxLEFT | wxRIGHT | wxEXPAND, dlg.FromDIP(10));
    top->Add(buttons, 0, wxALL | wxEXPAND, dlg.FromDIP(10));
    dlg.SetSizerAndFit(top);
    dlg.ShowModal();
}

bool PrintQueueRunner::ask_start(wxWindow *parent, bool printer_idle, long long &run_at, bool &cleared)
{
    wxDialog dlg(parent, wxID_ANY, _L("Add to Print Queue"));
    auto *when_clear = new wxRadioButton(&dlg, wxID_ANY, _L("As soon as the printer is free and I confirm the plate is clear"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    auto *when_time  = new wxRadioButton(&dlg, wxID_ANY, _L("Not before:"));
    wxDateTime start = wxDateTime::Now() + wxTimeSpan::Hours(1);
    start.SetSecond(0);
    auto *date = new wxDatePickerCtrl(&dlg, wxID_ANY, start);
    auto *time = new wxTimePickerCtrl(&dlg, wxID_ANY, start);
    auto *clear = new wxCheckBox(&dlg, wxID_ANY, _L("The plate is clear now (lets a timed job start unattended)"));
    clear->Enable(printer_idle);
    auto sync = [&]() { date->Enable(when_time->GetValue()); time->Enable(when_time->GetValue()); };
    when_clear->Bind(wxEVT_RADIOBUTTON, [&](wxCommandEvent &) { sync(); });
    when_time->Bind(wxEVT_RADIOBUTTON, [&](wxCommandEvent &) { sync(); });
    sync();

    auto *row = new wxBoxSizer(wxHORIZONTAL);
    row->Add(when_time, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, dlg.FromDIP(8));
    row->Add(date, 0, wxRIGHT, dlg.FromDIP(6));
    row->Add(time, 0);
    auto *top = new wxBoxSizer(wxVERTICAL);
    top->Add(when_clear, 0, wxALL, dlg.FromDIP(10));
    top->Add(row, 0, wxLEFT | wxRIGHT | wxBOTTOM, dlg.FromDIP(10));
    top->Add(clear, 0, wxLEFT | wxRIGHT | wxBOTTOM, dlg.FromDIP(10));
    top->Add(dlg.CreateStdDialogButtonSizer(wxOK | wxCANCEL), 0, wxALL | wxEXPAND, dlg.FromDIP(10));
    dlg.SetSizerAndFit(top);
    if (dlg.ShowModal() != wxID_OK) return false;

    run_at = 0;
    if (when_time->GetValue()) {
        wxDateTime d = date->GetValue(), t = time->GetValue();
        d.SetHour(t.GetHour());
        d.SetMinute(t.GetMinute());
        d.SetSecond(0);
        run_at = (long long) d.GetTicks();
    }
    cleared = clear->IsEnabled() && clear->GetValue();
    return true;
}

}} // namespace Slic3r::GUI
