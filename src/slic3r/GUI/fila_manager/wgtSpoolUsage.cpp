#include "wgtSpoolUsage.h"
#include "wgtFilaManagerStore.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {

namespace {
constexpr long long kJobMaxAge = 7 * 24 * 3600; // a sent job that never finished is dropped after a week

bool is_printing(const std::string &s) { return s == "RUNNING" || s == "PAUSE" || s == "PREPARE" || s == "SLICING"; }

// Job names as sent and as reported can differ in case and extension.
std::string norm(std::string n)
{
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char *ext : {".gcode.3mf", ".3mf", ".gcode"})
        if (n.size() > strlen(ext) && n.compare(n.size() - strlen(ext), strlen(ext), ext) == 0) n.resize(n.size() - strlen(ext));
    return n;
}
bool same_job(const std::string &sent, const std::string &reported)
{
    if (sent.empty() || reported.empty()) return true;
    const std::string a = norm(sent), b = norm(reported);
    return a.find(b) != std::string::npos || b.find(a) != std::string::npos;
}
} // namespace

wgtSpoolUsage::wgtSpoolUsage(wgtFilaManagerStore *store, std::string storage_path)
    : m_store(store), m_path(std::move(storage_path))
{}

void wgtSpoolUsage::on_send(SpoolPendingJob job)
{
    // one pending job per printer: a new send replaces one that never started
    m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(), [&](const SpoolPendingJob &j) { return j.dev_id == job.dev_id; }), m_jobs.end());
    m_jobs.push_back(std::move(job));
    save();
}

std::vector<std::string> wgtSpoolUsage::on_status(const std::string &dev_id, const std::string &print_state, int percent,
                                                  const std::string &subtask_name, long long now)
{
    std::vector<std::string> changed;
    PrinterState &ps   = m_printers[dev_id];
    const bool    was  = is_printing(ps.state);
    if (is_printing(print_state) && percent >= 0)
        ps.percent = percent;
    const bool ended = was && !print_state.empty() && !is_printing(print_state);
    if (!print_state.empty())
        ps.state = print_state;

    const size_t before = m_jobs.size();
    m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(), [&](const SpoolPendingJob &j) { return now - j.sent_at > kJobMaxAge; }), m_jobs.end());

    if (ended) {
        // the job sent to this printer; a known different name means it was some other print
        auto it = std::find_if(m_jobs.begin(), m_jobs.end(), [&](const SpoolPendingJob &j) { return j.dev_id == dev_id; });
        if (it != m_jobs.end() && same_job(it->name, subtask_name)) {
            const double share = print_state == "FINISH" ? 1. : std::clamp(ps.percent / 100., 0., 1.);
            BOOST_LOG_TRIVIAL(info) << "spool usage: " << dev_id << " ended " << print_state << " at " << ps.percent << "%, job " << it->name;
            changed = deduct(*it, share);
            m_jobs.erase(it);
        }
        ps.percent = 0;
    }
    if (ended || m_jobs.size() != before)
        save();
    return changed;
}

std::vector<std::string> wgtSpoolUsage::deduct(const SpoolPendingJob &job, double share)
{
    std::vector<std::string> changed;
    if (!m_store || share <= 0.)
        return changed;
    for (const SpoolSlotUse &u : job.uses) {
        const FilamentSpool *sp = m_store->find_by_slot(job.dev_id, u.ams_id, u.slot_id);
        if (!sp || FilamentSpool::is_valid_tag_uid(sp->tag_uid))
            continue; // unknown slot, or an RFID spool the printer keeps track of
        FilamentSpool s = *sp;
        s.net_weight    = std::max(0., s.net_weight - u.grams * share);
        const double total = s.effective_total_net_weight();
        if (total > 0.)
            s.remain_percent = int(std::lround(100. * s.net_weight / total));
        if (s.status != "archived")
            s.status = s.net_weight <= 0. ? "empty" : s.remain_percent <= 10 ? "low" : "active";
        m_store->update_spool(s);
        changed.push_back(s.spool_id);
    }
    return changed;
}

void wgtSpoolUsage::load()
{
    if (m_path.empty() || !boost::filesystem::exists(m_path))
        return;
    try {
        std::ifstream in(m_path);
        const nlohmann::json j = nlohmann::json::parse(in);
        m_jobs.clear();
        for (const auto &jj : j.value("pending", nlohmann::json::array())) {
            SpoolPendingJob job;
            job.dev_id  = jj.value("dev_id", "");
            job.name    = jj.value("name", "");
            job.plate   = jj.value("plate", -1);
            job.sent_at = jj.value("sent_at", 0LL);
            for (const auto &u : jj.value("uses", nlohmann::json::array()))
                job.uses.push_back({u.value("ams_id", ""), u.value("slot_id", ""), u.value("grams", 0.)});
            m_jobs.push_back(std::move(job));
        }
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "spool usage: cannot read " << m_path << ": " << e.what();
    }
}

void wgtSpoolUsage::save() const
{
    if (m_path.empty())
        return;
    nlohmann::json pending = nlohmann::json::array();
    for (const SpoolPendingJob &job : m_jobs) {
        nlohmann::json uses = nlohmann::json::array();
        for (const SpoolSlotUse &u : job.uses)
            uses.push_back({{"ams_id", u.ams_id}, {"slot_id", u.slot_id}, {"grams", u.grams}});
        pending.push_back({{"dev_id", job.dev_id}, {"name", job.name}, {"plate", job.plate}, {"sent_at", job.sent_at}, {"uses", uses}});
    }
    try {
        boost::filesystem::create_directories(boost::filesystem::path(m_path).parent_path());
        const std::string tmp = m_path + ".tmp";
        { std::ofstream out(tmp); out << nlohmann::json{{"pending", pending}}.dump(1); }
        boost::filesystem::rename(tmp, m_path);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "spool usage: cannot write " << m_path << ": " << e.what();
    }
}

}} // namespace Slic3r::GUI
