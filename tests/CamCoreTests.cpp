// CamCoreTests.cpp : tests for CamCore. These run without Alibre.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>

#include "CamJob.h"
#include "FileIO.h"
#include "Origin.h"
#include "ProgramSettings.h"
#include "ToolLibrary.h"

namespace fs = std::filesystem;
using namespace camcore;

namespace
{
    // A folder that is removed at the end of the test.
    class TempFolder
    {
    public:
        TempFolder()
        {
            static std::atomic<int> counter{ 0 };
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            m_path = fs::temp_directory_path() /
                     ("camcore_test_" + std::to_string(stamp) + "_" + std::to_string(counter++));
            fs::create_directories(m_path);
        }

        ~TempFolder()
        {
            std::error_code ignored;
            fs::remove_all(m_path, ignored);
        }

        fs::path operator/(const char* name) const { return m_path / name; }

    private:
        fs::path m_path;
    };

    void WriteText(const fs::path& path, const std::string& text)
    {
        std::ofstream file(path, std::ios::binary);
        file << text;
    }

    std::string ReadText(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    Job SampleJob()
    {
        Job job;
        job.assembly.name = "Bracket";
        job.assembly.guid = "A4A39FD0-567C-4D6B-A085-77688483C8EA";
        return job;
    }

    size_t CountFiles(const TempFolder& folder)
    {
        size_t count = 0;
        for (const auto& entry : fs::directory_iterator(folder / ""))
        {
            (void)entry;
            ++count;
        }
        return count;
    }
}

TEST_CASE("a saved job loads back unchanged")
{
    TempFolder folder;
    const fs::path path = folder / "Bracket.cam";

    const SaveResult saved = SaveJob(SampleJob(), path);
    REQUIRE(saved.ok);
    CHECK(saved.message.empty());

    const LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    CHECK(loaded.message.empty());
    CHECK(loaded.fileVersion == kCamFormatVersion);
    CHECK(loaded.job.assembly.name == "Bracket");
    CHECK(loaded.job.assembly.guid == "A4A39FD0-567C-4D6B-A085-77688483C8EA");
}

TEST_CASE("a profiling operation is an operation, and loads back with its shared settings")
{
    CHECK(IsOperation(StepType::Profiling));

    TempFolder folder;
    const fs::path path = folder / "Bracket.cam";
    Job job = SampleJob();
    SequenceStep origin;
    origin.type = StepType::SetOrigin;
    origin.id = NewStableId();
    origin.name = "Origin 1";
    job.sequence.push_back(origin);
    SequenceStep group;
    group.type = StepType::Group;
    group.id = NewStableId();
    group.name = "Folder 1";
    SequenceStep profiling;
    profiling.type = StepType::Profiling;
    profiling.id = NewStableId();
    profiling.name = "Profiling 1";
    profiling.toolId = "t1";
    profiling.cutLevels.totalDepth = 3;
    profiling.cutLevels.roughDepth = 2.5;
    profiling.cutLevels.finishDepth = 0.5;
    profiling.entryExit.entry = EntryMode::Entry2D;
    profiling.profiling.stockToLeave = -0.25;
    profiling.profiling.direction = CutDirection::Climb;
    profiling.profiling.startSide = ProfileSide::Left;
    profiling.profiling.useClosedSide = true;
    profiling.profiling.closedSide = ClosedProfileSide::Inside;
    profiling.profiling.useModelForSide = true;
    profiling.profiling.cutWidth = 6;
    profiling.profiling.stepPerCut = 2;
    profiling.profiling.useTabs = true;
    profiling.profiling.tabHeight = 1.5;
    profiling.profiling.tabLength = 8;
    profiling.profiling.tabSpacing = TabSpacing::Distance;
    profiling.profiling.tabCount = 3;
    profiling.profiling.tabDistance = 40;
    PickedGeometry spline;
    spline.kind = PickKind::CurveEdge;
    spline.start = { 1, 2, 3 };
    spline.point = { 4, 5, 3 };
    spline.end = { 7, 8, 3 };
    profiling.features.push_back(spline);
    group.children.push_back(profiling);
    job.sequence.push_back(group);
    REQUIRE(SaveJob(job, path).ok);

    const LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    const SequenceStep& back = loaded.job.sequence[1].children[0];
    CHECK(back.type == StepType::Profiling);
    CHECK(back.name == "Profiling 1");
    CHECK(back.toolId == "t1");
    CHECK(back.cutLevels.totalDepth == 3);
    CHECK(back.entryExit.entry == EntryMode::Entry2D);
    const ProfilingSettings& p = back.profiling;
    CHECK(p.stockToLeave == -0.25);
    CHECK(p.direction == CutDirection::Climb);
    CHECK(p.startSide == ProfileSide::Left);
    CHECK(p.useClosedSide);
    CHECK(p.closedSide == ClosedProfileSide::Inside);
    CHECK(p.useModelForSide);
    CHECK(p.cutWidth == 6);
    CHECK(p.stepPerCut == 2);
    CHECK(p.useTabs);
    CHECK(p.tabHeight == 1.5);
    CHECK(p.tabLength == 8);
    CHECK(p.tabSpacing == TabSpacing::Distance);
    CHECK(p.tabCount == 3);
    CHECK(p.tabDistance == 40);
    REQUIRE(back.features.size() == 1);
    CHECK(back.features[0].kind == PickKind::CurveEdge);
    CHECK(back.features[0].start.y == 2);
    CHECK(back.features[0].end.x == 7);
    CHECK(back.features[0].point.x == 4);
    CHECK(ReadText(path).find("\"profiling\"") != std::string::npos);
}

TEST_CASE("a pocketing operation is an operation, and loads back with its shared settings")
{
    CHECK(IsOperation(StepType::Pocketing));

    TempFolder folder;
    const fs::path path = folder / "Bracket.cam";
    Job job = SampleJob();
    SequenceStep origin;
    origin.type = StepType::SetOrigin;
    origin.id = NewStableId();
    origin.name = "Origin 1";
    job.sequence.push_back(origin);
    SequenceStep group;
    group.type = StepType::Group;
    group.id = NewStableId();
    group.name = "Folder 1";
    SequenceStep pocketing;
    pocketing.type = StepType::Pocketing;
    pocketing.id = NewStableId();
    pocketing.name = "Pocketing 1";
    pocketing.toolId = "t2";
    pocketing.cutLevels.totalDepth = 4;
    pocketing.cutLevels.roughDepth = 4;
    pocketing.cutLevels.finishDepth = 0;
    pocketing.entryExit.entry = EntryMode::AlongPath3D;
    pocketing.pocketing.stockToLeave = 0.3;
    pocketing.pocketing.cutType = PocketCutType::Linear;
    pocketing.pocketing.direction = CutDirection::Mixed;
    pocketing.pocketing.cutAngle = 30;
    pocketing.pocketing.start = PocketStart::Outside;
    pocketing.pocketing.linearStart = PocketLinearStart::Top;
    pocketing.pocketing.stepover = { false, 40, 1.5 };
    group.children.push_back(pocketing);
    job.sequence.push_back(group);
    REQUIRE(SaveJob(job, path).ok);

    const LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    const SequenceStep& back = loaded.job.sequence[1].children[0];
    CHECK(back.type == StepType::Pocketing);
    CHECK(back.name == "Pocketing 1");
    CHECK(back.toolId == "t2");
    CHECK(back.cutLevels.totalDepth == 4);
    CHECK(back.entryExit.entry == EntryMode::AlongPath3D);
    const PocketingSettings& p = back.pocketing;
    CHECK(p.stockToLeave == 0.3);
    CHECK(p.cutType == PocketCutType::Linear);
    CHECK(p.direction == CutDirection::Mixed);
    CHECK(p.cutAngle == 30);
    CHECK(p.start == PocketStart::Outside);
    CHECK(p.linearStart == PocketLinearStart::Top);
    CHECK_FALSE(p.stepover.usePercent);
    CHECK(p.stepover.percent == 40);
    CHECK(p.stepover.distance == 1.5);
    CHECK(ReadText(path).find("\"pocketing\"") != std::string::npos);
}

TEST_CASE("hole pocketing settings default as asked, are checked, and load back")
{
    HolePocketingSettings settings;
    CHECK(settings.location == CutGeometryLocation::AtTop);
    CHECK(settings.depth == 10);
    CHECK_FALSE(settings.useModelForDepth);
    CHECK(settings.useArcDiameter);
    CHECK(settings.diameter == 10);
    CHECK_FALSE(settings.cleanUpEachLevel);
    CHECK(settings.direction == CutDirection::Climb);
    CHECK(settings.stepover.percent == 50);
    CHECK(settings.stepDown == HoleStepDown::Percent);
    CHECK(settings.stepDownPercent == 25);
    CHECK(settings.stepDownDistance == 1);
    CHECK(settings.stepDownLevels == 4);
    CHECK(settings.helixDiameter == 3);
    CHECK(settings.pitch == HelixPitch::Angle);
    CHECK(settings.pitchAngle == 3);
    CHECK(settings.pitchHeight == 1);
    CHECK(settings.approachDistance == 1);
    CHECK(ValidateHolePocketing(settings).empty());

    // The diameter's only checked when it's used, and so are the step down and pitch choices.
    settings.diameter = 0;
    CHECK(ValidateHolePocketing(settings).empty());
    settings.useArcDiameter = false;
    CHECK(ValidateHolePocketing(settings).size() == 1);
    settings.diameter = 8;
    settings.stepDownLevels = 0;
    CHECK(ValidateHolePocketing(settings).empty());
    settings.stepDown = HoleStepDown::Levels;
    CHECK(ValidateHolePocketing(settings).size() == 1);
    settings.stepDownLevels = 3;
    settings.pitchAngle = 90;
    CHECK(ValidateHolePocketing(settings).size() == 1);
    settings.pitch = HelixPitch::Height;
    CHECK(ValidateHolePocketing(settings).empty());
    settings.depth = 0;
    settings.approachDistance = -1;
    settings.direction = CutDirection::Mixed;
    CHECK(ValidateHolePocketing(settings).size() == 3);

    // Saved and loaded with its operation.
    TempFolder folder;
    const fs::path path = folder / "Bracket.cam";
    Job job = SampleJob();
    SequenceStep origin;
    origin.type = StepType::SetOrigin;
    origin.id = NewStableId();
    origin.name = "Origin 1";
    job.sequence.push_back(origin);
    SequenceStep group;
    group.type = StepType::Group;
    group.id = NewStableId();
    group.name = "Folder 1";
    SequenceStep hole;
    hole.type = StepType::HolePocketing;
    hole.id = NewStableId();
    hole.name = "Hole Pocketing 1";
    HolePocketingSettings& h = hole.holePocketing;
    h.location = CutGeometryLocation::PickTop;
    h.topZ = 4;
    h.depth = 12;
    h.useModelForDepth = true;
    h.diameter = 9;
    h.useArcDiameter = false;
    h.cleanUpEachLevel = true;
    h.direction = CutDirection::Conventional;
    h.stepover = { false, 30, 1.5 };
    h.stepDown = HoleStepDown::Levels;
    h.stepDownPercent = 20;
    h.stepDownDistance = 0.5;
    h.stepDownLevels = 6;
    h.helixDiameter = 4;
    h.pitch = HelixPitch::Height;
    h.pitchAngle = 2;
    h.pitchHeight = 0.8;
    h.approachDistance = 2;
    group.children.push_back(hole);
    job.sequence.push_back(group);
    REQUIRE(SaveJob(job, path).ok);

    const LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    const SequenceStep& back = loaded.job.sequence[1].children[0];
    CHECK(IsOperation(back.type));
    CHECK(back.type == StepType::HolePocketing);
    const HolePocketingSettings& b = back.holePocketing;
    CHECK(b.location == CutGeometryLocation::PickTop);
    CHECK(b.topZ == 4);
    CHECK(b.depth == 12);
    CHECK(b.useModelForDepth);
    CHECK(b.diameter == 9);
    CHECK_FALSE(b.useArcDiameter);
    CHECK(b.cleanUpEachLevel);
    CHECK(b.direction == CutDirection::Conventional);
    CHECK_FALSE(b.stepover.usePercent);
    CHECK(b.stepover.distance == 1.5);
    CHECK(b.stepDown == HoleStepDown::Levels);
    CHECK(b.stepDownPercent == 20);
    CHECK(b.stepDownDistance == 0.5);
    CHECK(b.stepDownLevels == 6);
    CHECK(b.helixDiameter == 4);
    CHECK(b.pitch == HelixPitch::Height);
    CHECK(b.pitchAngle == 2);
    CHECK(b.pitchHeight == 0.8);
    CHECK(b.approachDistance == 2);
}

TEST_CASE("pocketing settings default as asked, and are checked")
{
    PocketingSettings settings;
    CHECK(settings.stockToLeave == 0);
    CHECK(settings.cutType == PocketCutType::Offset);
    CHECK(settings.direction == CutDirection::Climb);
    CHECK(settings.cutAngle == 0);
    CHECK(settings.start == PocketStart::Inside);
    CHECK(settings.linearStart == PocketLinearStart::Bottom);
    CHECK(settings.stepover.usePercent);
    CHECK(settings.stepover.percent == 50);
    CHECK(settings.stepover.distance == 2);
    CHECK(ValidatePocketing(settings).empty());

    // Only Linear uses the angle, and Spiral can't be Mixed.
    CHECK(PocketUsesAngle(PocketCutType::Linear));
    CHECK_FALSE(PocketUsesAngle(PocketCutType::Offset));
    settings.direction = CutDirection::Mixed;
    CHECK(PocketDirection(settings) == CutDirection::Mixed);
    settings.cutType = PocketCutType::Spiral;
    CHECK(PocketDirection(settings) == CutDirection::Climb);
    CHECK_FALSE(PocketAllowsMixed(PocketCutType::Spiral));
    CHECK(PocketAllowsMixed(PocketCutType::Radial));

    // The stepover in use must fit the tool, or be a distance more than 0.
    settings.stepover.percent = 0;
    CHECK(ValidatePocketing(settings).size() == 1);
    settings.stepover.percent = 101;
    CHECK(ValidatePocketing(settings).size() == 1);
    settings.stepover.usePercent = false;
    CHECK(ValidatePocketing(settings).empty());
    settings.stepover.distance = 0;
    CHECK(ValidatePocketing(settings).size() == 1);

    // Stock to leave can be negative.
    settings.stepover.distance = 1;
    settings.stockToLeave = -0.2;
    CHECK(ValidatePocketing(settings).empty());
}

TEST_CASE("profiling settings default as asked, and are checked")
{
    ProfilingSettings settings;
    CHECK(settings.stockToLeave == 0);
    CHECK(settings.direction == CutDirection::Mixed);
    CHECK(settings.startSide == ProfileSide::Right);
    CHECK_FALSE(settings.useClosedSide);
    CHECK(settings.closedSide == ClosedProfileSide::Outside);
    CHECK_FALSE(settings.useTabs);
    CHECK(settings.tabSpacing == TabSpacing::Count);
    CHECK(ValidateProfiling(settings).empty());

    // A width needs a step that fits in it. Equal is fine.
    settings.cutWidth = 4;
    CHECK(ValidateProfiling(settings).size() == 1);
    settings.stepPerCut = 4;
    CHECK(ValidateProfiling(settings).empty());
    settings.stepPerCut = 5;
    CHECK(ValidateProfiling(settings).size() == 1);
    settings.stepPerCut = 1;
    settings.cutWidth = -1;
    CHECK(ValidateProfiling(settings).size() == 1);
    settings.cutWidth = 4;

    // Stock to leave can be negative.
    settings.stockToLeave = -1;
    CHECK(ValidateProfiling(settings).empty());

    // Tab values are only checked when tabs are used, and only the chosen spacing.
    settings.tabHeight = 0;
    settings.tabDistance = 0;
    CHECK(ValidateProfiling(settings).empty());
    settings.useTabs = true;
    CHECK(ValidateProfiling(settings).size() == 1);     // the height; the count is used
    settings.tabHeight = 1;
    settings.tabCount = 0;
    CHECK(ValidateProfiling(settings).size() == 1);
    settings.tabSpacing = TabSpacing::Distance;
    CHECK(ValidateProfiling(settings).size() == 1);     // now the distance
    settings.tabDistance = 30;
    CHECK(ValidateProfiling(settings).empty());
}

TEST_CASE("suppressed steps load back suppressed, and suppressed origins are skipped")
{
    Job job = SampleJob();
    for (int i = 0; i < 2; ++i)
    {
        SequenceStep origin;
        origin.type = StepType::SetOrigin;
        origin.id = NewStableId();
        origin.name = "Origin " + std::to_string(i + 1);
        job.sequence.push_back(origin);

        SequenceStep group;
        group.type = StepType::Group;
        group.id = NewStableId();
        group.name = "Folder " + std::to_string(i + 1);
        SequenceStep drill;
        drill.type = StepType::Drill;
        drill.id = NewStableId();
        drill.name = "Drill " + std::to_string(i + 1);
        group.children.push_back(drill);
        group.children.push_back(drill);
        group.children.back().id = NewStableId();
        job.sequence.push_back(group);
    }
    // Origin 1, Folder 1, Origin 2, Folder 2.

    CHECK(ActiveOriginIndex(job, 3) == 2);
    CHECK(WorkZeroIndex(job) == 0);
    job.sequence[2].suppressed = true;
    CHECK(ActiveOriginIndex(job, 3) == 0);      // past the suppressed one
    CHECK(ActiveOriginIndex(job, 2) == 2);      // but it's still its own
    job.sequence[0].suppressed = true;
    CHECK(WorkZeroIndex(job) == kNoStepIndex);
    CHECK(ActiveOriginIndex(job, 1) == kNoStepIndex);
    job.sequence[2].suppressed = false;
    CHECK(WorkZeroIndex(job) == 2);

    job.sequence[1].children[1].suppressed = true;
    job.sequence[3].suppressed = true;
    CHECK_FALSE(IsOperationSuppressed(job, 1, 0));
    CHECK(IsOperationSuppressed(job, 1, 1));
    CHECK(IsOperationSuppressed(job, 3, 0));        // its folder is

    TempFolder folder;
    const fs::path path = folder / "Bracket.cam";
    REQUIRE(SaveJob(job, path).ok);
    const LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    CHECK(loaded.job.sequence[0].suppressed);
    CHECK_FALSE(loaded.job.sequence[1].suppressed);
    CHECK_FALSE(loaded.job.sequence[1].children[0].suppressed);
    CHECK(loaded.job.sequence[1].children[1].suppressed);
    CHECK_FALSE(loaded.job.sequence[2].suppressed);
    CHECK(loaded.job.sequence[3].suppressed);
}

TEST_CASE("program settings save and load, and keep their defaults when missing")
{
    TempFolder folder;
    const fs::path path = folder / "settings.json";

    // No file yet: the defaults.
    ProgramSettingsLoadResult loaded = LoadProgramSettings(path);
    CHECK(loaded.status == LoadStatus::NotFound);
    CHECK(loaded.settings.viewSize == ViewSize::Small);

    ProgramSettings settings;
    settings.viewSize = ViewSize::Large;
    REQUIRE(SaveProgramSettings(settings, path).ok);
    loaded = LoadProgramSettings(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    CHECK(loaded.settings.viewSize == ViewSize::Large);

    // A file without the view size keeps the default; a newer one isn't to be overwritten.
    WriteText(path, R"({"format":"AlibreCAM settings","formatVersion":1})");
    loaded = LoadProgramSettings(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    CHECK(loaded.settings.viewSize == ViewSize::Small);
    WriteText(path, R"({"format":"AlibreCAM settings","formatVersion":9,"viewSize":"huge"})");
    CHECK(LoadProgramSettings(path).status == LoadStatus::NewerVersion);
    WriteText(path, R"({"format":"AlibreCAM settings","formatVersion":1,"viewSize":"huge"})");
    CHECK(LoadProgramSettings(path).status == LoadStatus::NotACamFile);

    CHECK(ViewIconSize(ViewSize::Small) == 16);
    CHECK(ViewIconSize(ViewSize::Medium) == 24);
    CHECK(ViewIconSize(ViewSize::Large) == 32);
    CHECK(ViewTextScale(ViewSize::Large) == 1.5);
}

TEST_CASE("non-ASCII names and paths survive a save and load")
{
    TempFolder folder;
    const fs::path path = folder / "" / fs::u8path("Tr\xC3\xA4ger \xE6\x94\xAF\xE6\x9E\xB6.cam");

    Job job = SampleJob();
    job.assembly.name = "Tr\xC3\xA4ger \xE6\x94\xAF\xE6\x9E\xB6";

    REQUIRE(SaveJob(job, path).ok);

    const LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    CHECK(loaded.job.assembly.name == job.assembly.name);
}

TEST_CASE("the file records its format and version")
{
    TempFolder folder;
    const fs::path path = folder / "Bracket.cam";
    REQUIRE(SaveJob(SampleJob(), path).ok);

    const std::string text = ReadText(path);
    CHECK(text.find("\"format\": \"AlibreCAM\"") != std::string::npos);
    CHECK(text.find("\"formatVersion\": " + std::to_string(kCamFormatVersion)) != std::string::npos);
    CHECK(text.find("\"sequence\": []") != std::string::npos);
}

TEST_CASE("saving replaces an existing file and leaves no temporary file")
{
    TempFolder folder;
    const fs::path path = folder / "Bracket.cam";
    REQUIRE(SaveJob(SampleJob(), path).ok);

    Job changed = SampleJob();
    changed.assembly.name = "Renamed";
    REQUIRE(SaveJob(changed, path).ok);

    CHECK(LoadJob(path).job.assembly.name == "Renamed");
    CHECK(CountFiles(folder) == 1);
}

TEST_CASE("a failed save reports an error and leaves nothing behind")
{
    TempFolder folder;

    SUBCASE("the folder doesn't exist")
    {
        const SaveResult saved = SaveJob(SampleJob(), folder / "missing/Bracket.cam");
        CHECK_FALSE(saved.ok);
        CHECK_FALSE(saved.message.empty());
        CHECK(CountFiles(folder) == 0);
    }

    SUBCASE("the target can't be replaced")
    {
        const fs::path target = folder / "Bracket.cam";
        fs::create_directories(target);

        const SaveResult saved = SaveJob(SampleJob(), target);
        CHECK_FALSE(saved.ok);
        CHECK_FALSE(saved.message.empty());
        CHECK(fs::is_directory(target));
        CHECK(CountFiles(folder) == 1);
    }
}

TEST_CASE("files that can't be used are reported")
{
    TempFolder folder;
    const fs::path path = folder / "Bracket.cam";

    SUBCASE("missing")
    {
        CHECK(LoadJob(path).status == LoadStatus::NotFound);
    }

    SUBCASE("not JSON")
    {
        WriteText(path, "this is not json {");
        CHECK(LoadJob(path).status == LoadStatus::Unreadable);
    }

    SUBCASE("empty")
    {
        WriteText(path, "");
        CHECK(LoadJob(path).status == LoadStatus::Unreadable);
    }

    SUBCASE("JSON from something else")
    {
        WriteText(path, R"({"name":"package.json"})");
        CHECK(LoadJob(path).status == LoadStatus::NotACamFile);
    }

    SUBCASE("not an object")
    {
        WriteText(path, "[1,2,3]");
        CHECK(LoadJob(path).status == LoadStatus::NotACamFile);
    }

    SUBCASE("version zero")
    {
        WriteText(path, R"({"format":"AlibreCAM","formatVersion":0,"assembly":{"name":"a","guid":"b"}})");
        CHECK(LoadJob(path).status == LoadStatus::NotACamFile);
    }

    SUBCASE("version isn't a number")
    {
        WriteText(path, R"({"format":"AlibreCAM","formatVersion":"1","assembly":{"name":"a","guid":"b"}})");
        CHECK(LoadJob(path).status == LoadStatus::NotACamFile);
    }

    SUBCASE("no assembly section")
    {
        WriteText(path, R"({"format":"AlibreCAM","formatVersion":1})");
        CHECK(LoadJob(path).status == LoadStatus::NotACamFile);
    }

    SUBCASE("assembly GUID is the wrong type")
    {
        WriteText(path, R"({"format":"AlibreCAM","formatVersion":1,"assembly":{"name":"a","guid":5}})");
        CHECK(LoadJob(path).status == LoadStatus::NotACamFile);
    }
}

TEST_CASE("a file from a newer build is identified and left alone")
{
    TempFolder folder;
    const fs::path path = folder / "Bracket.cam";
    const std::string text = R"({"format":"AlibreCAM","formatVersion":999,"future":true})";
    WriteText(path, text);

    const LoadResult loaded = LoadJob(path);
    CHECK(loaded.status == LoadStatus::NewerVersion);
    CHECK(loaded.fileVersion == 999);
    CHECK_FALSE(loaded.message.empty());
    CHECK(ReadText(path) == text);
}

TEST_CASE("assembly identities are compared by GUID")
{
    AssemblyIdentity a{ "Bracket", "A4A39FD0-567C-4D6B-A085-77688483C8EA" };
    AssemblyIdentity lower{ "Renamed", "a4a39fd0-567c-4d6b-a085-77688483c8ea" };
    AssemblyIdentity other{ "Bracket", "7E79980B-0000-0000-0000-000000000000" };
    AssemblyIdentity none{ "Bracket", "" };

    CHECK(CompareIdentity(a, a) == IdentityMatch::Same);
    CHECK(CompareIdentity(a, lower) == IdentityMatch::Same);
    CHECK(CompareIdentity(a, other) == IdentityMatch::Different);
    CHECK(CompareIdentity(a, none) == IdentityMatch::Unknown);
    CHECK(CompareIdentity(none, a) == IdentityMatch::Unknown);
}

namespace
{
    Tool SampleMill()
    {
        Tool tool;
        tool.id = NewToolId();
        tool.number = 1;
        tool.name = "6 mm flat end mill";
        tool.type = ToolType::FlatEndMill;
        tool.unit = ToolUnit::Millimetres;
        tool.diameter = 6.0;
        tool.fluteLength = 22.0;
        tool.overallLength = 60.0;
        tool.fluteCount = 2;
        return tool;
    }

    Tool SampleDrill()
    {
        Tool tool;
        tool.id = NewToolId();
        tool.number = 2;
        tool.name = "1/8 in drill";
        tool.type = ToolType::Drill;
        tool.unit = ToolUnit::Inches;
        tool.diameter = 0.125;
        tool.fluteLength = 1.5;
        tool.overallLength = 2.5;
        tool.pointAngle = 135.0;
        return tool;
    }

    bool HasProblem(const std::vector<std::string>& problems, const std::string& text)
    {
        for (const std::string& problem : problems)
        {
            if (problem.find(text) != std::string::npos)
                return true;
        }
        return false;
    }
}

TEST_CASE("a saved tool library loads back unchanged")
{
    TempFolder folder;
    const fs::path path = folder / "tools.json";

    ToolLibrary library;
    library.tools.push_back(SampleMill());
    library.tools.push_back(SampleDrill());

    const SaveResult saved = SaveToolLibrary(library, path);
    REQUIRE(saved.ok);

    const ToolLibraryLoadResult loaded = LoadToolLibrary(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    REQUIRE(loaded.library.tools.size() == 2);

    const Tool& mill = loaded.library.tools[0];
    CHECK(mill.id == library.tools[0].id);
    CHECK(mill.number == 1);
    CHECK(mill.name == "6 mm flat end mill");
    CHECK(mill.type == ToolType::FlatEndMill);
    CHECK(mill.unit == ToolUnit::Millimetres);
    CHECK(mill.diameter == 6.0);
    CHECK(mill.fluteLength == 22.0);
    CHECK(mill.overallLength == 60.0);
    CHECK(mill.fluteCount == 2);

    const Tool& drill = loaded.library.tools[1];
    CHECK(drill.type == ToolType::Drill);
    CHECK(drill.unit == ToolUnit::Inches);
    CHECK(drill.diameter == 0.125);
    CHECK(drill.pointAngle == 135.0);
}

TEST_CASE("cutting settings in a library are ignored and not written back")
{
    TempFolder folder;
    const fs::path path = folder / "tools.json";

    {
        std::ofstream file(path, std::ios::binary);
        file << R"({
            "format": "AlibreCAM tool library",
            "formatVersion": 1,
            "tools": [{
                "id": "abc", "number": 1, "name": "Mill", "type": "flatEndMill", "unit": "mm",
                "diameter": 6.0, "fluteLength": 22.0, "overallLength": 60.0, "fluteCount": 2,
                "spindleSpeed": 18000.0, "plungeFeed": 300.0, "feed": 1200.0, "stepdown": 3.0, "stepover": 2.4
            }]
        })";
    }

    const ToolLibraryLoadResult loaded = LoadToolLibrary(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    REQUIRE(loaded.library.tools.size() == 1);

    REQUIRE(SaveToolLibrary(loaded.library, path).ok);

    std::string text;
    REQUIRE(ReadWholeFile(path, text));
    CHECK(text.find("\"feed\"") == std::string::npos);
    CHECK(text.find("spindleSpeed") == std::string::npos);
    CHECK(text.find("stepover") == std::string::npos);
}

TEST_CASE("an empty tool library is valid")
{
    TempFolder folder;
    const fs::path path = folder / "tools.json";
    REQUIRE(SaveToolLibrary(ToolLibrary(), path).ok);

    const ToolLibraryLoadResult loaded = LoadToolLibrary(path);
    CHECK(loaded.status == LoadStatus::Ok);
    CHECK(loaded.library.tools.empty());
}

TEST_CASE("tool validation")
{
    CHECK(ValidateTool(SampleMill()).empty());
    CHECK(ValidateTool(SampleDrill()).empty());

    Tool mill = SampleMill();

    SUBCASE("number") { mill.number = 0; CHECK(HasProblem(ValidateTool(mill), "number")); }
    SUBCASE("name") { mill.name.clear(); CHECK(HasProblem(ValidateTool(mill), "name")); }
    SUBCASE("diameter") { mill.diameter = 0.0; CHECK(HasProblem(ValidateTool(mill), "diameter")); }
    SUBCASE("flute length") { mill.fluteLength = -1.0; CHECK(HasProblem(ValidateTool(mill), "flute length")); }
    SUBCASE("overall length") { mill.overallLength = 10.0; CHECK(HasProblem(ValidateTool(mill), "overall length")); }
    SUBCASE("flute count") { mill.fluteCount = 0; CHECK(HasProblem(ValidateTool(mill), "flute count")); }

    SUBCASE("drills ignore mill fields, but check their own")
    {
        Tool drill = SampleDrill();
        drill.fluteCount = 0;
        CHECK(ValidateTool(drill).empty());

        drill.pointAngle = 200.0;
        CHECK(HasProblem(ValidateTool(drill), "point angle"));
    }
}

TEST_CASE("duplicate tool numbers aren't allowed")
{
    ToolLibrary library;
    library.tools.push_back(SampleMill());
    Tool second = SampleDrill();
    second.number = 1;
    library.tools.push_back(second);

    CHECK(HasProblem(ValidateLibrary(library), "same number"));

    TempFolder folder;
    const SaveResult saved = SaveToolLibrary(library, folder / "tools.json");
    CHECK_FALSE(saved.ok);
    CHECK(CountFiles(folder) == 0);
}

TEST_CASE("inch tools convert to millimetres exactly")
{
    const Tool drill = ToolInMillimetres(SampleDrill());
    CHECK(drill.unit == ToolUnit::Millimetres);
    CHECK(drill.diameter == doctest::Approx(3.175));
    CHECK(drill.fluteLength == doctest::Approx(38.1));
    CHECK(drill.overallLength == doctest::Approx(63.5));
    CHECK(drill.pointAngle == 135.0);

    const Tool mill = ToolInMillimetres(SampleMill());
    CHECK(mill.diameter == 6.0);
}

TEST_CASE("tool libraries that can't be used are reported and left alone")
{
    TempFolder folder;
    const fs::path path = folder / "tools.json";

    SUBCASE("missing")
    {
        CHECK(LoadToolLibrary(path).status == LoadStatus::NotFound);
    }

    SUBCASE("not JSON")
    {
        WriteText(path, "{ nope");
        CHECK(LoadToolLibrary(path).status == LoadStatus::Unreadable);
    }

    SUBCASE("a .cam file instead")
    {
        REQUIRE(SaveJob(SampleJob(), path).ok);
        CHECK(LoadToolLibrary(path).status == LoadStatus::NotACamFile);
    }

    SUBCASE("newer version")
    {
        const std::string text = R"({"format":"AlibreCAM tool library","formatVersion":99,"tools":[]})";
        WriteText(path, text);
        CHECK(LoadToolLibrary(path).status == LoadStatus::NewerVersion);
        CHECK(ReadText(path) == text);
    }

    SUBCASE("no tools section")
    {
        WriteText(path, R"({"format":"AlibreCAM tool library","formatVersion":1})");
        CHECK(LoadToolLibrary(path).status == LoadStatus::NotACamFile);
    }

    SUBCASE("a tool with a missing field")
    {
        WriteText(path, R"({"format":"AlibreCAM tool library","formatVersion":1,"tools":[{"id":"a","number":1}]})");
        const ToolLibraryLoadResult loaded = LoadToolLibrary(path);
        CHECK(loaded.status == LoadStatus::NotACamFile);
        CHECK_FALSE(loaded.message.empty());
    }

    SUBCASE("a tool with an invalid value")
    {
        ToolLibrary library;
        library.tools.push_back(SampleMill());
        REQUIRE(SaveToolLibrary(library, path).ok);

        std::string text = ReadText(path);
        const std::string field = "\"diameter\": 6.0";
        REQUIRE(text.find(field) != std::string::npos);
        text.replace(text.find(field), field.size(), "\"diameter\": -6.0");
        WriteText(path, text);

        const ToolLibraryLoadResult loaded = LoadToolLibrary(path);
        CHECK(loaded.status == LoadStatus::NotACamFile);
        CHECK(loaded.message.find("diameter") != std::string::npos);
    }
}

TEST_CASE("finding tools and choosing numbers")
{
    ToolLibrary library;
    CHECK(NextFreeToolNumber(library) == 1);

    Tool mill = SampleMill();
    mill.number = 1;
    Tool drill = SampleDrill();
    drill.number = 3;
    library.tools.push_back(mill);
    library.tools.push_back(drill);

    CHECK(NextFreeToolNumber(library) == 2);
    REQUIRE(FindToolById(library, drill.id) != nullptr);
    CHECK(FindToolById(library, drill.id)->number == 3);
    CHECK(FindToolById(library, "missing") == nullptr);
    CHECK(NewToolId().size() == 32);
    CHECK(NewToolId() != NewToolId());
}

TEST_CASE("a job remembers its selected tool, machine and material")
{
    TempFolder folder;
    const fs::path path = folder / "Bracket.cam";

    Job job = SampleJob();
    job.selectedToolId = "abc123";
    job.machineId = "router";
    job.materialId = "alu";
    REQUIRE(SaveJob(job, path).ok);

    const LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    CHECK(loaded.job.selectedToolId == "abc123");
    CHECK(loaded.job.machineId == "router");
    CHECK(loaded.job.materialId == "alu");
}

TEST_CASE("an older job's tool library is read but not saved again")
{
    TempFolder folder;
    const fs::path path = folder / "Bracket.cam";
    WriteText(path, R"({"format":"AlibreCAM","formatVersion":4,"assembly":{"name":"a","guid":"b"},)"
                    R"("tools":{"library":"shop_tools.json","selected":"t1"},"setup":{},"sequence":[]})");

    LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    CHECK(loaded.job.toolLibraryPath == "shop_tools.json");
    CHECK(loaded.job.selectedToolId == "t1");
    CHECK(loaded.job.machineId.empty());

    REQUIRE(SaveJob(loaded.job, path).ok);
    loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    CHECK(loaded.job.toolLibraryPath.empty());
    CHECK(loaded.job.selectedToolId == "t1");
}

TEST_CASE("jobs saved before tools existed still load")
{
    TempFolder folder;
    const fs::path path = folder / "Bracket.cam";
    WriteText(path, R"({"format":"AlibreCAM","formatVersion":1,"assembly":{"name":"a","guid":"b"},"setup":{},"sequence":[]})");

    const LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    CHECK(loaded.job.toolLibraryPath.empty());
    CHECK(loaded.job.selectedToolId.empty());
}

TEST_CASE("a library beside the job is stored by name, so they can move together")
{
    const fs::path cam = "folder/Bracket.cam";

    CHECK(StoredLibraryPath(cam, "folder/tools.json") == "tools.json");
    CHECK(StoredLibraryPath(cam, "other/tools.json") == fs::path("other/tools.json").u8string());
    CHECK(StoredLibraryPath(fs::path(), "other/tools.json") == fs::path("other/tools.json").u8string());
    CHECK(StoredLibraryPath(cam, fs::path()).empty());

    CHECK(ResolveLibraryPath(cam, "tools.json") == fs::path("folder/tools.json"));
    CHECK(ResolveLibraryPath("moved/Bracket.cam", "tools.json") == fs::path("moved/tools.json"));
    CHECK(ResolveLibraryPath(cam, "other/tools.json") == fs::path("other/tools.json"));
    CHECK(ResolveLibraryPath(cam, "").empty());
}

TEST_CASE("the .cam file sits next to the assembly with the same name")
{
    CHECK(CamPathForAssembly("folder/Bracket.AD_ASM") == fs::path("folder/Bracket.cam"));
    CHECK(CamPathForAssembly("folder/My.Bracket.v2.AD_ASM") == fs::path("folder/My.Bracket.v2.cam"));
    CHECK(CamPathForAssembly("").empty());
}
