// MachineTests.cpp : tests for machines, materials, and tools' feeds per material.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "FileIO.h"
#include "Machine.h"
#include "Materials.h"
#include "ProgramSettings.h"

using namespace camcore;
namespace fs = std::filesystem;

namespace
{
    // A folder of its own under the temp folder, removed afterwards.
    struct Folder
    {
        fs::path path;
        Folder()
        {
            path = fs::temp_directory_path() / ("AlibreCAM-machine-test-" + NewStableId());
            fs::create_directories(path);
        }
        ~Folder()
        {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    };

    void Write(const fs::path& path, const std::string& text)
    {
        std::ofstream file(path, std::ios::binary);
        file << text;
    }

    Tool Mill(const std::string& id, int number)
    {
        Tool tool;
        tool.id = id;
        tool.number = number;
        tool.name = "6 mm end mill";
        tool.diameter = 6;
        tool.fluteLength = 20;
        tool.overallLength = 50;
        return tool;
    }

    ToolFeeds Row(const std::string& material, double rpm, double cut)
    {
        ToolFeeds row;
        row.materialId = material;
        row.feeds.spindleRpm = rpm;
        row.feeds.cut = cut;
        return row;
    }

    Machine Router()
    {
        Machine machine;
        machine.id = "router1";
        machine.name = "Router";
        machine.axes = 4;
        machine.rotary = RotaryAxis::MinusX;
        machine.travelX = 800;
        machine.travelY = 500;
        machine.travelZ = 120;
        machine.maxSpindleRpm = 18000;
        Tool tool = Mill("t1", 1);
        tool.feeds.push_back(Row("alu", 16000, 800));
        tool.feeds.push_back(Row("mdf", 18000, 2500));
        machine.tools.tools.push_back(tool);
        return machine;
    }
}

TEST_CASE("a machine saves and loads with its tools and their feeds")
{
    Folder folder;
    const Machine machine = Router();
    const fs::path path = MachineFilePath(folder.path, machine.id);
    CHECK(path.filename() == "router1.json");
    REQUIRE(SaveMachine(machine, path).ok);

    const MachineLoadResult loaded = LoadMachine(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    CHECK(loaded.machine.name == "Router");
    CHECK(loaded.machine.axes == 4);
    CHECK(loaded.machine.rotary == RotaryAxis::MinusX);
    CHECK(loaded.machine.travelX == 800);
    CHECK(loaded.machine.travelY == 500);
    CHECK(loaded.machine.travelZ == 120);
    CHECK(loaded.machine.maxSpindleRpm == 18000);
    REQUIRE(loaded.machine.tools.tools.size() == 1);
    const Tool& tool = loaded.machine.tools.tools[0];
    REQUIRE(tool.feeds.size() == 2);
    const ToolFeeds* pMdf = FeedsFor(tool, "mdf");
    REQUIRE(pMdf != nullptr);
    CHECK(pMdf->feeds.spindleRpm == 18000);
    CHECK(pMdf->feeds.cut == 2500);
    CHECK(FeedsFor(tool, "oak") == nullptr);
}

TEST_CASE("machines are checked")
{
    Machine machine = Router();
    CHECK(ValidateMachine(machine).empty());

    machine.name.clear();
    CHECK_FALSE(ValidateMachine(machine).empty());

    machine = Router();
    machine.axes = 5;
    CHECK_FALSE(ValidateMachine(machine).empty());

    machine = Router();
    machine.travelZ = 0;
    CHECK_FALSE(ValidateMachine(machine).empty());

    machine = Router();
    machine.maxSpindleRpm = -1;
    CHECK_FALSE(ValidateMachine(machine).empty());

    // A material at most once in a tool's feeds, and the feeds themselves valid.
    machine = Router();
    machine.tools.tools[0].feeds.push_back(Row("alu", 1000, 100));
    CHECK_FALSE(ValidateMachine(machine).empty());

    machine = Router();
    machine.tools.tools[0].feeds[0].feeds.cut = 0;
    CHECK_FALSE(ValidateMachine(machine).empty());
}

TEST_CASE("a version 1 machine, without travel or spindle speed, loads with the defaults")
{
    Folder folder;
    const fs::path path = folder.path / "m.json";
    Write(path, R"({"format":"AlibreCAM machine","formatVersion":1,"id":"m","name":"M","axes":3,"rotaryAxis":"+Y","tools":[]})");
    const MachineLoadResult loaded = LoadMachine(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    const Machine defaults;
    CHECK(loaded.machine.travelX == defaults.travelX);
    CHECK(loaded.machine.travelZ == defaults.travelZ);
    CHECK(loaded.machine.maxSpindleRpm == defaults.maxSpindleRpm);
}

TEST_CASE("damaged, newer and missing machine files")
{
    Folder folder;
    const fs::path path = folder.path / "m.json";
    CHECK(LoadMachine(path).status == LoadStatus::NotFound);

    Write(path, "not json");
    CHECK(LoadMachine(path).status == LoadStatus::Unreadable);

    Write(path, R"({"format":"AlibreCAM machine","formatVersion":99})");
    CHECK(LoadMachine(path).status == LoadStatus::NewerVersion);

    Write(path, R"({"format":"AlibreCAM machine","formatVersion":1,"id":"m","name":"M","axes":3,"rotaryAxis":"+Q","tools":[]})");
    CHECK(LoadMachine(path).status == LoadStatus::NotACamFile);
}

TEST_CASE("machines are found in their folder, readable ones by name first")
{
    Folder folder;
    Machine a = Router();
    a.id = "a";
    a.name = "Zebra mill";
    Machine b = Router();
    b.id = "b";
    b.name = "alpha router";
    REQUIRE(SaveMachine(a, MachineFilePath(folder.path, a.id)).ok);
    REQUIRE(SaveMachine(b, MachineFilePath(folder.path, b.id)).ok);
    Write(folder.path / "broken.json", "{");
    Write(folder.path / "notes.txt", "ignored");

    const std::vector<MachineEntry> found = FindMachines(folder.path);
    REQUIRE(found.size() == 3);
    CHECK(found[0].machine.name == "alpha router");
    CHECK(found[1].machine.name == "Zebra mill");
    CHECK(found[2].status != LoadStatus::Ok);

    CHECK(FindMachines(folder.path / "missing").empty());
}

TEST_CASE("an exported library keeps the feeds, and imports keep IDs")
{
    Folder folder;
    const Machine machine = Router();
    const fs::path exported = folder.path / "tools.json";
    REQUIRE(SaveToolLibrary(machine.tools, exported).ok);

    const ToolLibraryLoadResult loaded = LoadToolLibrary(exported);
    REQUIRE(loaded.status == LoadStatus::Ok);
    REQUIRE(loaded.library.tools.size() == 1);
    CHECK(loaded.library.tools[0].feeds.size() == 2);

    // Into a machine with a different tool numbered 1: the import keeps its ID but is renumbered.
    ToolLibrary into;
    into.tools.push_back(Mill("other", 1));
    CHECK(CountSharedTools(into, loaded.library) == 0);
    ToolImportResult result = ImportTools(into, loaded.library, false);
    CHECK(result.added == 1);
    CHECK(result.renumbered == 1);
    const Tool* pImported = FindToolById(into, "t1");
    REQUIRE(pImported != nullptr);
    CHECK(pImported->number == 2);
    CHECK(ValidateLibrary(into).empty());

    // Again: the same ID is skipped, or replaced, as asked.
    CHECK(CountSharedTools(into, loaded.library) == 1);
    result = ImportTools(into, loaded.library, false);
    CHECK(result.skipped == 1);
    CHECK(into.tools.size() == 2);
    result = ImportTools(into, loaded.library, true);
    CHECK(result.replaced == 1);
    CHECK(into.tools.size() == 2);
    CHECK(ValidateLibrary(into).empty());
}

TEST_CASE("a version 1 tool library, without feeds, still loads")
{
    Folder folder;
    const fs::path path = folder.path / "tools.json";
    Write(path, R"({"format":"AlibreCAM tool library","formatVersion":1,"tools":[{"id":"a","number":1,"name":"Mill",)"
                R"("type":"flatEndMill","unit":"mm","diameter":6,"fluteLength":20,"overallLength":50,"fluteCount":2}]})");
    const ToolLibraryLoadResult loaded = LoadToolLibrary(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    CHECK(loaded.library.tools[0].feeds.empty());
}

TEST_CASE("materials save, load and are checked")
{
    Folder folder;
    const fs::path path = folder.path / "materials.json";
    CHECK(LoadMaterials(path).status == LoadStatus::NotFound);

    MaterialList list;
    list.materials.push_back({ "alu", "6061 Aluminium" });
    list.materials.push_back({ "mdf", "MDF" });
    REQUIRE(SaveMaterials(list, path).ok);

    const MaterialsLoadResult loaded = LoadMaterials(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    REQUIRE(loaded.list.materials.size() == 2);
    const Material* pMdf = FindMaterial(loaded.list, "mdf");
    REQUIRE(pMdf != nullptr);
    CHECK(pMdf->name == "MDF");
    CHECK(FindMaterial(loaded.list, "oak") == nullptr);

    list.materials.push_back({ "mdf2", "mdf" });       // the same name, ignoring case
    CHECK_FALSE(ValidateMaterials(list).empty());
    CHECK_FALSE(SaveMaterials(list, path).ok);

    list.materials.back() = { "x", "" };
    CHECK_FALSE(ValidateMaterials(list).empty());

    Write(path, R"({"format":"AlibreCAM materials","formatVersion":9,"materials":[]})");
    CHECK(LoadMaterials(path).status == LoadStatus::NewerVersion);
}

TEST_CASE("the settings remember the last machine")
{
    Folder folder;
    const fs::path path = folder.path / "settings.json";
    ProgramSettings settings;
    settings.lastMachineId = "router1";
    REQUIRE(SaveProgramSettings(settings, path).ok);
    const ProgramSettingsLoadResult loaded = LoadProgramSettings(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    CHECK(loaded.settings.lastMachineId == "router1");
}
