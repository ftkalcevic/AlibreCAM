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
        machine.holderDiameter = 25;
        machine.holderDepth = 40;
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
    CHECK(loaded.machine.holderDiameter == 25);
    CHECK(loaded.machine.holderDepth == 40);
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
    CHECK(loaded.machine.holderDiameter == 20.0);
    CHECK(loaded.machine.holderDepth == 30.0);
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

TEST_CASE("every tool type saves and loads with its own fields")
{
    Folder folder;
    ToolLibrary library;
    int number = 1;
    for (ToolType type : ToolTypes())
    {
        Tool tool = Mill("id" + std::to_string(number), number);
        tool.type = type;
        tool.name = ToolTypeTitle(type);
        tool.diameter = 10;
        tool.cornerRadius = 2;
        tool.includedAngle = 60;
        tool.tipDiameter = 0.5;
        tool.taperAngle = 3;
        tool.pitch = 1.5;
        tool.threadAngle = 55;
        tool.pointAngle = 135;
        library.tools.push_back(tool);
        ++number;
    }
    CHECK(library.tools.size() == 8);
    REQUIRE(ValidateLibrary(library).empty());

    const fs::path path = folder.path / "tools.json";
    REQUIRE(SaveToolLibrary(library, path).ok);
    const ToolLibraryLoadResult loaded = LoadToolLibrary(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    REQUIRE(loaded.library.tools.size() == 8);
    for (size_t i = 0; i < library.tools.size(); ++i)
    {
        const Tool& back = loaded.library.tools[i];
        const ToolFields fields = FieldsOf(back.type);
        CHECK(back.type == library.tools[i].type);
        if (fields.cornerRadius)
            CHECK(back.cornerRadius == 2);
        if (fields.includedAngle)
            CHECK(back.includedAngle == 60);
        if (fields.tipDiameter)
            CHECK(back.tipDiameter == 0.5);
        if (fields.taperAngle)
            CHECK(back.taperAngle == 3);
        if (fields.pitch)
        {
            CHECK(back.pitch == 1.5);
            CHECK(back.threadAngle == 55);
        }
        if (fields.pointAngle)
            CHECK(back.pointAngle == 135);
    }
    CHECK(ToolTypeTitle(ToolType::VMill) == "V-mill");
}

TEST_CASE("each tool type's own fields are checked")
{
    Tool tool = Mill("a", 1);
    tool.diameter = 6;

    tool.type = ToolType::CornerRadiusMill;
    tool.cornerRadius = 3;
    CHECK(ValidateTool(tool).empty());
    tool.cornerRadius = 3.5;            // more than half the diameter
    CHECK_FALSE(ValidateTool(tool).empty());

    tool.type = ToolType::VMill;
    tool.includedAngle = 180;
    CHECK_FALSE(ValidateTool(tool).empty());
    tool.includedAngle = 90;
    tool.tipDiameter = 6;               // as wide as the tool
    CHECK_FALSE(ValidateTool(tool).empty());
    tool.tipDiameter = 0;
    CHECK(ValidateTool(tool).empty());

    tool.type = ToolType::TaperMill;
    tool.taperAngle = 90;
    CHECK_FALSE(ValidateTool(tool).empty());

    tool.type = ToolType::ThreadMill;
    tool.pitch = 0;
    CHECK_FALSE(ValidateTool(tool).empty());

    // A ball mill has nothing extra to get wrong.
    tool.type = ToolType::BallMill;
    CHECK(ValidateTool(tool).empty());
}

TEST_CASE("inch tools' extra lengths become mm")
{
    Tool tool = Mill("a", 1);
    tool.unit = ToolUnit::Inches;
    tool.cornerRadius = 0.1;
    tool.tipDiameter = 0.2;
    tool.pitch = 0.05;
    const Tool mm = ToolInMillimetres(tool);
    CHECK(mm.cornerRadius == doctest::Approx(2.54));
    CHECK(mm.tipDiameter == doctest::Approx(5.08));
    CHECK(mm.pitch == doctest::Approx(1.27));
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
