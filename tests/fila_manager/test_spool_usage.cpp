#include <catch2/catch.hpp>

#include <boost/filesystem.hpp>

#include "slic3r/GUI/fila_manager/wgtFilaManagerStore.h"
#include "slic3r/GUI/fila_manager/wgtSpoolUsage.h"

using namespace Slic3r::GUI;

static std::string mounted(wgtFilaManagerStore &store, const std::string &slot, double net, const std::string &tag = {})
{
    FilamentSpool s;
    s.brand          = "Test";
    s.material_type  = "PLA";
    s.initial_weight = 1000;
    s.net_weight     = net;
    s.tag_uid        = tag;
    s.in_printer     = true;
    s.dev_id         = "DEV";
    s.ams_id         = 0;
    s.slot_id        = slot;
    return store.add_spool(s);
}

static SpoolPendingJob job(const std::string &name = "part")
{
    SpoolPendingJob j;
    j.dev_id  = "DEV";
    j.name    = name;
    j.uses    = {{"0", "1", 100.}, {"0", "2", 50.}};
    j.sent_at = 1000;
    return j;
}

TEST_CASE("Spool usage: a finished print takes its grams from untagged spools", "[fila_manager][spool_usage]")
{
    wgtFilaManagerStore store;
    const std::string plain = mounted(store, "1", 500), tagged = mounted(store, "2", 500, "A1B2C3D4");
    wgtSpoolUsage usage(&store);
    usage.on_send(job());
    REQUIRE(usage.on_status("DEV", "PREPARE", 0, "part", 1010).empty());
    REQUIRE(usage.on_status("DEV", "RUNNING", 50, "part", 1020).empty());
    const auto changed = usage.on_status("DEV", "FINISH", 100, "part.gcode.3mf", 1030);
    REQUIRE(changed == std::vector<std::string>{plain});
    REQUIRE(store.get_spool(plain)->net_weight == Approx(400.));
    REQUIRE(store.get_spool(plain)->remain_percent == 40);
    REQUIRE(store.get_spool(tagged)->net_weight == Approx(500.)); // the printer reports RFID spools itself
    REQUIRE(usage.pending().empty());
    // a later status of the same print does not count it twice
    REQUIRE(usage.on_status("DEV", "IDLE", 0, "", 1040).empty());
    REQUIRE(store.get_spool(plain)->net_weight == Approx(400.));
}

TEST_CASE("Spool usage: a cancelled print takes the progress share", "[fila_manager][spool_usage]")
{
    wgtFilaManagerStore store;
    const std::string plain = mounted(store, "1", 500);
    wgtSpoolUsage usage(&store);
    usage.on_send(job());
    usage.on_status("DEV", "RUNNING", 40, "part", 1010);
    usage.on_status("DEV", "FAILED", 0, "part", 1020); // progress resets on failure; the last running value counts
    REQUIRE(store.get_spool(plain)->net_weight == Approx(460.));
}

TEST_CASE("Spool usage: another print on the printer is not charged to the sent job", "[fila_manager][spool_usage]")
{
    wgtFilaManagerStore store;
    const std::string plain = mounted(store, "1", 500);
    wgtSpoolUsage usage(&store);
    usage.on_send(job("part"));
    usage.on_status("DEV", "RUNNING", 10, "something_else", 1010);
    REQUIRE(usage.on_status("DEV", "FINISH", 100, "something_else", 1020).empty());
    REQUIRE(store.get_spool(plain)->net_weight == Approx(500.));
    REQUIRE(usage.pending().size() == 1);
}

TEST_CASE("Spool usage: a sent job survives a restart", "[fila_manager][spool_usage]")
{
    const auto dir  = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path();
    const auto path = (dir / "usage.json").string();
    wgtFilaManagerStore store;
    const std::string plain = mounted(store, "1", 500);
    {
        wgtSpoolUsage usage(&store, path);
        usage.on_send(job());
        usage.on_status("DEV", "RUNNING", 30, "part", 1010);
    }
    wgtSpoolUsage usage(&store, path);
    usage.load();
    REQUIRE(usage.pending().size() == 1);
    usage.on_status("DEV", "RUNNING", 60, "part", 2000); // the app sees the print again after the restart
    usage.on_status("DEV", "FINISH", 100, "part", 2010);
    REQUIRE(store.get_spool(plain)->net_weight == Approx(400.));
    boost::filesystem::remove_all(dir);
}
