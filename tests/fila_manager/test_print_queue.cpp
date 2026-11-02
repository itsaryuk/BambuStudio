#include <catch2/catch.hpp>

#include <fstream>
#include <boost/filesystem.hpp>

#include "slic3r/GUI/PrintQueue.hpp"

using namespace Slic3r::GUI;
namespace fs = boost::filesystem;

static QueuedPrint job(const std::string &dev, const std::string &name, const std::string &file = {})
{
    QueuedPrint p;
    p.dev_id = dev;
    p.name   = name;
    p.file   = file;
    return p;
}

TEST_CASE("Print queue: a job starts only when the printer is idle and the plate confirmed clear", "[print_queue]")
{
    using R = PrintQueue::Ready;
    QueuedPrint p = job("A", "part");
    REQUIRE(PrintQueue::ready(p, false, 100) == R::No);
    REQUIRE(PrintQueue::ready(p, true, 100) == R::NeedsConfirm);
    p.cleared = true;
    REQUIRE(PrintQueue::ready(p, true, 100) == R::Go);
    p.run_at = 200; // scheduled: confirmed early, waits for its time
    REQUIRE(PrintQueue::ready(p, true, 199) == R::No);
    REQUIRE(PrintQueue::ready(p, true, 200) == R::Go);
    REQUIRE(PrintQueue::ready(p, false, 300) == R::No);
    p.error = "upload failed"; // a failed send waits for the user
    REQUIRE(PrintQueue::ready(p, true, 300) == R::No);
}

TEST_CASE("Print queue: each printer runs its jobs in order", "[print_queue]")
{
    PrintQueue q;
    const std::string a1 = q.add(job("A", "a1")).id;
    const std::string b1 = q.add(job("B", "b1")).id;
    const std::string a2 = q.add(job("A", "a2")).id;
    REQUIRE(q.front("A")->id == a1);
    REQUIRE(q.front("B")->id == b1);
    REQUIRE(q.printers() == std::set<std::string>{"A", "B"});
    q.move(a2, -1); // jumps over b1 to the earlier job of the same printer
    REQUIRE(q.front("A")->id == a2);
    REQUIRE(q.front("B")->id == b1);
    q.remove(a2);
    REQUIRE(q.front("A")->id == a1);
    q.remove(a1);
    REQUIRE(q.front("A") == nullptr);
}

TEST_CASE("Print queue: jobs keep their own file copy and survive a restart", "[print_queue]")
{
    const fs::path tmp = fs::temp_directory_path() / fs::unique_path();
    fs::create_directories(tmp);
    const std::string src = (tmp / "plate.3mf").string();
    { std::ofstream(src) << "sliced"; }
    const std::string dir = (tmp / "queue").string();
    std::string id;
    {
        PrintQueue q(dir);
        QueuedPrint p = job("A", "part", src);
        p.run_at  = 1234;
        p.cleared = true;
        p.job     = {{"ams_mapping", "[0,1]"}};
        id = q.add(p).id;
    }
    fs::remove(src); // the plater's temp file may go away
    PrintQueue q(dir);
    q.load();
    REQUIRE(q.items().size() == 1);
    const QueuedPrint &p = q.items()[0];
    REQUIRE(p.id == id);
    REQUIRE(fs::exists(p.file));
    REQUIRE(p.run_at == 1234);
    REQUIRE(!p.cleared); // the plate may have changed while the app was closed
    REQUIRE(p.job.value("ams_mapping", "") == "[0,1]");
    const std::string copy = p.file;
    q.remove(id);
    REQUIRE(!fs::exists(copy));
    fs::remove_all(tmp);
}
