// FeatureTests.cpp : tests for operation features, their order, and how they're saved.

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <string>

#include "CamJob.h"
#include "Features.h"

namespace fs = std::filesystem;
using namespace camcore;

namespace
{
    PickedGeometry Edge(const std::string& key, const Vec3& centre)
    {
        PickedGeometry pick;
        pick.kind = PickKind::CircleEdge;
        pick.occurrenceKey = { 17, "0A" };
        pick.targetKey = { 17, key };
        pick.occurrenceName = "Plate<1>";
        pick.point = centre;
        pick.direction = { 0, 0, 1 };
        pick.radius = 2.5;
        return pick;
    }

    std::vector<Vec3> Points(std::initializer_list<Vec3> list)
    {
        return std::vector<Vec3>(list);
    }

    fs::path TempCam()
    {
        static std::atomic<int> counter{ 0 };
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        return fs::temp_directory_path() /
               ("camcore_features_" + std::to_string(stamp) + "_" + std::to_string(counter++) + ".cam");
    }
}

TEST_CASE("Facing accepts flat faces and edges")
{
    CHECK(FeatureAllowed(FeatureFilter::FacingArea, PickKind::PlanarFace));
    CHECK(FeatureAllowed(FeatureFilter::FacingArea, PickKind::LineEdge));
    CHECK(FeatureAllowed(FeatureFilter::FacingArea, PickKind::CircleEdge));
    CHECK_FALSE(FeatureAllowed(FeatureFilter::FacingArea, PickKind::CylindricalFace));
    CHECK_FALSE(FeatureAllowed(FeatureFilter::FacingArea, PickKind::DesignPoint));
}

TEST_CASE("a facing operation is an operation, and loads back inside its folder")
{
    CHECK(IsOperation(StepType::Facing));
    CHECK_FALSE(IsOperation(StepType::Group));

    const fs::path path = TempCam();

    Job job;
    SequenceStep origin;
    origin.id = NewStableId();
    origin.name = "O1";
    job.sequence.push_back(origin);

    SequenceStep group;
    group.type = StepType::Group;
    group.id = NewStableId();
    group.name = "F1";

    SequenceStep facing;
    facing.type = StepType::Facing;
    facing.id = NewStableId();
    facing.name = "Facing 1";
    PickedGeometry top;
    top.kind = PickKind::PlanarFace;
    top.targetKey = { 17, "09" };
    top.direction = { 0, 0, 1 };
    facing.features.push_back(top);
    group.children.push_back(facing);
    job.sequence.push_back(group);

    REQUIRE(SaveJob(job, path).ok);
    const LoadResult loaded = LoadJob(path);
    std::error_code ignored;
    fs::remove(path, ignored);

    REQUIRE(loaded.status == LoadStatus::Ok);
    const SequenceStep& back = loaded.job.sequence[1].children[0];
    CHECK(back.type == StepType::Facing);
    CHECK(back.name == "Facing 1");
    REQUIRE(back.features.size() == 1);
    CHECK(back.features[0].targetKey.hex == "09");
}

TEST_CASE("clearance settings load back unchanged, and default when missing")
{
    const fs::path path = TempCam();

    Job job;
    SequenceStep origin;
    origin.id = NewStableId();
    origin.name = "O1";
    job.sequence.push_back(origin);

    SequenceStep group;
    group.type = StepType::Group;
    group.id = NewStableId();
    group.name = "F1";

    SequenceStep drill;
    drill.type = StepType::Drill;
    drill.id = NewStableId();
    drill.name = "D1";
    drill.toolId = "tool-7";
    drill.feeds.spindleRpm = 12000;
    drill.feeds.plunge = 150;
    drill.feeds.departure = 2500;
    drill.feeds.transferAtRapid = false;
    drill.feeds.transfer = 4000;
    drill.clearance.mode = ClearanceMode::StockMaxZ;
    drill.clearance.partDistance = 3;
    drill.clearance.stockDistance = 7.5;
    drill.clearance.absoluteZ = -2;
    group.children.push_back(drill);

    SequenceStep facing;
    facing.type = StepType::Facing;
    facing.id = NewStableId();
    facing.name = "Facing 1";
    facing.clearance.mode = ClearanceMode::AbsoluteZ;
    group.children.push_back(facing);
    job.sequence.push_back(group);

    REQUIRE(SaveJob(job, path).ok);
    const LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    const ClearanceSettings& back = loaded.job.sequence[1].children[0].clearance;
    CHECK(back.mode == ClearanceMode::StockMaxZ);
    CHECK(back.partDistance == 3);
    CHECK(back.stockDistance == 7.5);
    CHECK(back.absoluteZ == -2);
    CHECK(loaded.job.sequence[1].children[0].toolId == "tool-7");
    const FeedSettings& feeds = loaded.job.sequence[1].children[0].feeds;
    CHECK(feeds.spindleRpm == 12000);
    CHECK(feeds.plunge == 150);
    CHECK(feeds.cut == FeedSettings().cut);
    CHECK(feeds.departure == 2500);
    CHECK_FALSE(feeds.transferAtRapid);
    CHECK(feeds.transfer == 4000);
    CHECK(loaded.job.sequence[1].children[1].feeds.transferAtRapid);
    CHECK(loaded.job.sequence[1].children[1].toolId.empty());
    CHECK(loaded.job.sequence[1].children[1].clearance.mode == ClearanceMode::AbsoluteZ);

    // A drill saved before the Clearance tab existed.
    {
        std::ofstream out(path, std::ios::binary);
        out << R"({"format":"AlibreCAM","formatVersion":2,"assembly":{"name":"a","guid":"b"},"sequence":[)"
            << R"({"type":"group","id":"g","name":"F","children":[{"type":"drill","id":"d","name":"D1"}]}]})";
    }
    const LoadResult old = LoadJob(path);
    REQUIRE(old.status == LoadStatus::Ok);
    CHECK(old.job.sequence[0].children[0].clearance.mode == ClearanceMode::Automatic);
    CHECK(old.job.sequence[0].children[0].toolId.empty());

    // An unknown mode is damage.
    {
        std::ofstream out(path, std::ios::binary);
        out << R"({"format":"AlibreCAM","formatVersion":2,"assembly":{"name":"a","guid":"b"},"sequence":[)"
            << R"({"type":"group","id":"g","name":"F","children":[{"type":"drill","id":"d","name":"D1",)"
            << R"("clearance":{"mode":"high","partDistance":1,"stockDistance":1,"absoluteZ":1}}]}]})";
    }
    CHECK(LoadJob(path).status == LoadStatus::NotACamFile);

    std::error_code ignored;
    fs::remove(path, ignored);
}

TEST_CASE("clearance distances can't be negative, but an absolute Z can")
{
    ClearanceSettings settings;
    settings.partDistance = -1;
    settings.stockDistance = -1;
    settings.absoluteZ = -50;
    CHECK(ValidateClearance(settings).empty());     // automatic uses none of them

    settings.mode = ClearanceMode::PartMaxZ;
    CHECK(ValidateClearance(settings).size() == 1);
    settings.mode = ClearanceMode::StockMaxZ;
    CHECK(ValidateClearance(settings).size() == 1);
    settings.mode = ClearanceMode::AbsoluteZ;
    CHECK(ValidateClearance(settings).empty());
}

TEST_CASE("speeds and feeds must be more than 0, and the transfer feed only when it's used")
{
    FeedSettings feeds;
    CHECK(ValidateFeeds(feeds).empty());

    feeds.spindleRpm = 0;
    feeds.cut = -5;
    feeds.transfer = 0;     // rapid is used, so this isn't checked
    CHECK(ValidateFeeds(feeds).size() == 2);

    feeds.transferAtRapid = false;
    CHECK(ValidateFeeds(feeds).size() == 3);
}

TEST_CASE("cut levels load back unchanged, and default when missing")
{
    const fs::path path = TempCam();

    Job job;
    SequenceStep origin;
    origin.id = NewStableId();
    origin.name = "O1";
    job.sequence.push_back(origin);

    SequenceStep group;
    group.type = StepType::Group;
    group.id = NewStableId();
    group.name = "F1";

    SequenceStep facing;
    facing.type = StepType::Facing;
    facing.id = NewStableId();
    facing.name = "Facing 1";
    CutLevelSettings& levels = facing.cutLevels;
    levels.location = CutGeometryLocation::PickTop;
    levels.topZ = -3.5;
    levels.totalDepth = 2;
    levels.roughDepth = 1.5;
    levels.finishDepth = 0.5;
    levels.roughStep = 0.75;
    levels.finishStep = 0.25;
    levels.clearIslandTops = true;
    levels.useModelForDepth = true;
    levels.order = CutLevelOrder::LevelFirst;
    group.children.push_back(facing);
    job.sequence.push_back(group);

    REQUIRE(SaveJob(job, path).ok);
    const LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    const CutLevelSettings& back = loaded.job.sequence[1].children[0].cutLevels;
    CHECK(back.location == CutGeometryLocation::PickTop);
    CHECK(back.topZ == -3.5);
    CHECK(back.totalDepth == 2);
    CHECK(back.roughDepth == 1.5);
    CHECK(back.finishDepth == 0.5);
    CHECK(back.roughStep == 0.75);
    CHECK(back.finishStep == 0.25);
    CHECK(back.clearIslandTops);
    CHECK(back.useModelForDepth);
    CHECK(back.order == CutLevelOrder::LevelFirst);

    // A facing saved before the Cut Levels tab existed.
    {
        std::ofstream out(path, std::ios::binary);
        out << R"({"format":"AlibreCAM","formatVersion":2,"assembly":{"name":"a","guid":"b"},"sequence":[)"
            << R"({"type":"group","id":"g","name":"F","children":[{"type":"facing","id":"f","name":"Facing 1"}]}]})";
    }
    const LoadResult old = LoadJob(path);
    REQUIRE(old.status == LoadStatus::Ok);
    CHECK(old.job.sequence[0].children[0].cutLevels.totalDepth == CutLevelSettings().totalDepth);

    std::error_code ignored;
    fs::remove(path, ignored);
}

TEST_CASE("entry and exit settings load back unchanged, and default when missing")
{
    const fs::path path = TempCam();

    Job job;
    SequenceStep origin;
    origin.id = NewStableId();
    origin.name = "O1";
    job.sequence.push_back(origin);

    SequenceStep group;
    group.type = StepType::Group;
    group.id = NewStableId();
    group.name = "F1";

    SequenceStep facing;
    facing.type = StepType::Facing;
    facing.id = NewStableId();
    facing.name = "Facing 1";
    EntryExitSettings& settings = facing.entryExit;
    settings.entry = EntryMode::AlongPath3D;
    settings.engage.shape = LeadShape::Radial;
    settings.engage.radius = 4;
    settings.alongPathAngle = 3;
    settings.alongPathHeight = 1.5;
    settings.exit = ExitMode::Exit2D;
    settings.retract.length = 7;
    settings.retract.angle = 90;
    settings.eachCutLevel = true;
    group.children.push_back(facing);
    job.sequence.push_back(group);

    REQUIRE(SaveJob(job, path).ok);
    const LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    const EntryExitSettings& back = loaded.job.sequence[1].children[0].entryExit;
    CHECK(back.entry == EntryMode::AlongPath3D);
    CHECK(back.engage.shape == LeadShape::Radial);
    CHECK(back.engage.radius == 4);
    CHECK(back.alongPathAngle == 3);
    CHECK(back.alongPathHeight == 1.5);
    CHECK(back.exit == ExitMode::Exit2D);
    CHECK(back.retract.shape == LeadShape::Linear);
    CHECK(back.retract.length == 7);
    CHECK(back.retract.angle == 90);
    CHECK(back.eachCutLevel);

    // A facing saved before the Entry/Exit tab existed.
    {
        std::ofstream out(path, std::ios::binary);
        out << R"({"format":"AlibreCAM","formatVersion":2,"assembly":{"name":"a","guid":"b"},"sequence":[)"
            << R"({"type":"group","id":"g","name":"F","children":[{"type":"facing","id":"f","name":"Facing 1"}]}]})";
    }
    const LoadResult old = LoadJob(path);
    REQUIRE(old.status == LoadStatus::Ok);
    CHECK(old.job.sequence[0].children[0].entryExit.entry == EntryMode::None);

    std::error_code ignored;
    fs::remove(path, ignored);
}

TEST_CASE("facing settings load back unchanged, and default when missing")
{
    const fs::path path = TempCam();

    Job job;
    SequenceStep origin;
    origin.id = NewStableId();
    origin.name = "O1";
    job.sequence.push_back(origin);

    SequenceStep group;
    group.type = StepType::Group;
    group.id = NewStableId();
    group.name = "F1";

    SequenceStep facing;
    facing.type = StepType::Facing;
    facing.id = NewStableId();
    facing.name = "Facing 1";
    facing.facing.direction = CutDirection::Mixed;
    facing.facing.stepDistance = { false, 30, 4.5 };
    facing.facing.overtravel = { true, 75, 6 };
    group.children.push_back(facing);
    job.sequence.push_back(group);

    REQUIRE(SaveJob(job, path).ok);
    const LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    const FacingSettings& back = loaded.job.sequence[1].children[0].facing;
    CHECK(back.direction == CutDirection::Mixed);
    CHECK_FALSE(back.stepDistance.usePercent);
    CHECK(back.stepDistance.percent == 30);
    CHECK(back.stepDistance.distance == 4.5);
    CHECK(back.overtravel.usePercent);
    CHECK(back.overtravel.percent == 75);
    CHECK(back.overtravel.distance == 6);

    // A facing saved before the Facing tab existed gets 25% and 50%.
    {
        std::ofstream out(path, std::ios::binary);
        out << R"({"format":"AlibreCAM","formatVersion":2,"assembly":{"name":"a","guid":"b"},"sequence":[)"
            << R"({"type":"group","id":"g","name":"F","children":[{"type":"facing","id":"f","name":"Facing 1"}]}]})";
    }
    const LoadResult old = LoadJob(path);
    REQUIRE(old.status == LoadStatus::Ok);
    const FacingSettings& defaults = old.job.sequence[0].children[0].facing;
    CHECK(defaults.direction == CutDirection::Climb);
    CHECK(defaults.stepDistance.usePercent);
    CHECK(defaults.stepDistance.percent == 25);
    CHECK(defaults.overtravel.percent == 50);

    std::error_code ignored;
    fs::remove(path, ignored);
}

TEST_CASE("step distance and overtravel work from the tool, or as distances")
{
    FacingSettings facing;
    CHECK(ResolveSize(facing.stepDistance, 10) == doctest::Approx(2.5));
    CHECK(ResolveSize(facing.overtravel, 10) == doctest::Approx(5));
    facing.stepDistance.usePercent = false;
    CHECK(ResolveSize(facing.stepDistance, 10) == 2);
    CHECK(ValidateFacing(facing).empty());

    facing.stepDistance = { true, 0, 2 };
    CHECK(ValidateFacing(facing).size() == 1);
    facing.stepDistance = { true, 120, 2 };
    CHECK(ValidateFacing(facing).size() == 1);
    facing.stepDistance = { false, 0, 0 };
    CHECK(ValidateFacing(facing).size() == 1);

    facing = FacingSettings();
    facing.overtravel = { true, 0, -1 };     // 0% is fine, the distance isn't used
    CHECK(ValidateFacing(facing).empty());
    facing.overtravel.usePercent = false;
    CHECK(ValidateFacing(facing).size() == 1);
}

namespace
{
    PickedGeometry FlatFace(const Vec3& point, const Vec3& normal)
    {
        PickedGeometry pick;
        pick.kind = PickKind::PlanarFace;
        pick.point = point;
        pick.direction = normal;
        return pick;
    }

    PickedGeometry LineAt(const Vec3& start, const Vec3& end)
    {
        PickedGeometry pick;
        pick.kind = PickKind::LineEdge;
        pick.start = start;
        pick.end = end;
        pick.point = start;
        pick.direction = end - start;
        return pick;
    }

    PickedGeometry CircleAt(const Vec3& centre, const Vec3& axis)
    {
        PickedGeometry pick;
        pick.kind = PickKind::CircleEdge;
        pick.point = centre;
        pick.direction = axis;
        return pick;
    }
}

TEST_CASE("facing takes one flat face parallel to the origin plane")
{
    Frame frame;
    frame.origin = { 0, 0, 10 };

    FacingFeatureCheck check = CheckFacingFeatures(frame, { FlatFace({ 5, 5, 12 }, { 0, 0, 1 }) });
    REQUIRE(check.ok);
    CHECK(check.level == doctest::Approx(2));

    // Facing down is still parallel.
    CHECK(CheckFacingFeatures(frame, { FlatFace({ 5, 5, 12 }, { 0, 0, -1 }) }).ok);

    CHECK_FALSE(CheckFacingFeatures(frame, { FlatFace({ 0, 0, 0 }, { 0, 1, 1 }) }).ok);
    CHECK_FALSE(CheckFacingFeatures(frame, { FlatFace({ 0, 0, 0 }, { 0, 0, 1 }), FlatFace({ 0, 0, 0 }, { 0, 0, 1 }) }).ok);
    CHECK_FALSE(CheckFacingFeatures(frame, { FlatFace({ 0, 0, 0 }, { 0, 0, 1 }), LineAt({ 0, 0, 0 }, { 5, 0, 0 }) }).ok);
    CHECK_FALSE(CheckFacingFeatures(frame, {}).ok);
}

TEST_CASE("facing takes edges that lie on one plane parallel to the origin plane")
{
    Frame frame;

    // An open set of edges at Z 3: two lines and an arc.
    FacingFeatureCheck check = CheckFacingFeatures(
        frame, { LineAt({ 0, 0, 3 }, { 50, 0, 3 }), LineAt({ 50, 0, 3 }, { 50, 20, 3 }), CircleAt({ 25, 20, 3 }, { 0, 0, 1 }) });
    REQUIRE(check.ok);
    CHECK(check.level == doctest::Approx(3));

    // A line that climbs, a circle on a tilted plane, and edges at two heights.
    CHECK_FALSE(CheckFacingFeatures(frame, { LineAt({ 0, 0, 3 }, { 50, 0, 4 }) }).ok);
    CHECK_FALSE(CheckFacingFeatures(frame, { CircleAt({ 0, 0, 3 }, { 1, 0, 0 }) }).ok);
    const FacingFeatureCheck heights =
        CheckFacingFeatures(frame, { LineAt({ 0, 0, 3 }, { 50, 0, 3 }), LineAt({ 0, 10, 5 }, { 50, 10, 5 }) });
    CHECK_FALSE(heights.ok);
    CHECK(heights.problem.find("Edge 2") != std::string::npos);

    // Holes and points aren't facing features.
    PickedGeometry hole;
    hole.kind = PickKind::CylindricalFace;
    CHECK_FALSE(CheckFacingFeatures(frame, { hole }).ok);
}

TEST_CASE("facing checks use the origin's own Z")
{
    // A side origin: its Z is assembly +X, so a face facing +X is parallel to its plane.
    Frame side;
    side.origin = { 100, 0, 0 };
    side.xAxis = { 0, 1, 0 };
    side.yAxis = { 0, 0, 1 };
    side.zAxis = { 1, 0, 0 };

    const FacingFeatureCheck check = CheckFacingFeatures(side, { FlatFace({ 104, 7, 7 }, { 1, 0, 0 }) });
    REQUIRE(check.ok);
    CHECK(check.level == doctest::Approx(4));
    CHECK_FALSE(CheckFacingFeatures(side, { FlatFace({ 104, 7, 7 }, { 0, 0, 1 }) }).ok);
    CHECK(CheckFacingFeatures(side, { LineAt({ 104, 0, 0 }, { 104, 0, 30 }) }).ok);
}

TEST_CASE("only the entry and exit values in use are checked")
{
    EntryExitSettings settings;
    settings.engage.length = 0;
    settings.retract.radius = 0;
    settings.alongPathAngle = 0;
    CHECK(ValidateEntryExit(settings).empty());     // no entry, no exit

    settings.entry = EntryMode::Entry2D;
    CHECK(ValidateEntryExit(settings).size() == 1);     // linear length 0
    settings.engage.shape = LeadShape::Radial;
    CHECK(ValidateEntryExit(settings).empty());

    settings.entry = EntryMode::AlongPath3D;
    CHECK(ValidateEntryExit(settings).size() == 1);     // angle 0
    settings.alongPathAngle = 90;
    CHECK(ValidateEntryExit(settings).size() == 1);
    settings.alongPathAngle = 5;
    CHECK(ValidateEntryExit(settings).empty());

    settings.exit = ExitMode::Exit2D;
    settings.retract.shape = LeadShape::Radial;
    CHECK(ValidateEntryExit(settings).size() == 1);     // radius 0
    settings.retract.shape = LeadShape::Linear;
    settings.retract.angle = 200;
    CHECK(ValidateEntryExit(settings).size() == 1);
}

TEST_CASE("rough and finish must add up to the total, and each step fit its depth")
{
    CutLevelSettings levels;
    CHECK(ValidateCutLevels(levels).empty());

    levels.finishDepth = 0.3;       // 0.8 + 0.3 isn't 1
    CHECK(ValidateCutLevels(levels).size() == 1);

    levels = CutLevelSettings();
    levels.roughStep = 0.9;         // more than the 0.8 rough depth
    CHECK(ValidateCutLevels(levels).size() == 1);

    // No finish depth means no finish step is needed.
    levels = CutLevelSettings();
    levels.roughDepth = 1;
    levels.finishDepth = 0;
    levels.finishStep = 0;
    CHECK(ValidateCutLevels(levels).empty());

    levels.totalDepth = 0;
    levels.roughDepth = 0;
    CHECK_FALSE(ValidateCutLevels(levels).empty());
}

TEST_CASE("a signed height, and none for things along the drill direction")
{
    Frame frame;
    frame.origin = { 0, 0, 20 };

    LocatedGeometry point;
    point.point = { 5, 5, 8 };
    double height = 99;
    REQUIRE(HeightAlongZ(frame, point, height));
    CHECK(height == doctest::Approx(-12));
    CHECK(DistanceAlongZ(frame, point) == doctest::Approx(12));

    LocatedGeometry wall;
    wall.kind = LocatedGeometry::Kind::Plane;
    wall.direction = { 1, 0, 0 };
    height = 99;
    CHECK_FALSE(HeightAlongZ(frame, wall, height));
    CHECK(height == 99);
}

TEST_CASE("Drill accepts things that reduce to a point")
{
    CHECK(FeatureAllowed(FeatureFilter::Points, PickKind::CircleEdge));
    CHECK(FeatureAllowed(FeatureFilter::Points, PickKind::CylindricalFace));
    CHECK(FeatureAllowed(FeatureFilter::Points, PickKind::DesignPoint));
    CHECK_FALSE(FeatureAllowed(FeatureFilter::Points, PickKind::PlanarFace));
    CHECK_FALSE(FeatureAllowed(FeatureFilter::Points, PickKind::LineEdge));
}

TEST_CASE("a hole is drilled at the end nearer the origin's +Z")
{
    PickedGeometry hole;
    hole.kind = PickKind::CylindricalFace;
    hole.start = { 5, 5, 0 };
    hole.end = { 5, 5, 12 };

    Frame frame;
    CHECK(FeaturePoint(frame, hole).z == 12);

    frame.zAxis = { 0, 0, -1 };
    frame.yAxis = { 0, -1, 0 };
    CHECK(FeaturePoint(frame, hole).z == 0);

    PickedGeometry edge = Edge("01", { 1, 2, 3 });
    CHECK(FeaturePoint(frame, edge).y == 2);
}

TEST_CASE("points convert into the frame's own axes")
{
    Frame frame;
    frame.origin = { 10, 0, 0 };
    frame.xAxis = { 0, 1, 0 };
    frame.yAxis = { 0, 0, 1 };
    frame.zAxis = { 1, 0, 0 };

    const Vec3 local = WorldToFrame(frame, { 13, 4, 5 });
    CHECK(local.x == doctest::Approx(4));
    CHECK(local.y == doctest::Approx(5));
    CHECK(local.z == doctest::Approx(3));
}

namespace
{
    std::string Keys(const std::vector<PickedGeometry>& list)
    {
        std::string keys;
        for (const PickedGeometry& feature : list)
            keys += (keys.empty() ? "" : " ") + (feature.kind == PickKind::DesignPoint ? feature.targetName : feature.targetKey.hex);
        return keys;
    }
}

TEST_CASE("a selection change keeps the list's order, drops what's deselected and adds what's new")
{
    const PickedGeometry a = Edge("01", {});
    const PickedGeometry b = Edge("02", {});
    const PickedGeometry c = Edge("03", {});
    const PickedGeometry d = Edge("04", {});

    // The list is c a b, all selected. The user deselects a and adds d.
    CHECK(Keys(ApplySelectionChange({ c, a, b }, { c, a, b }, { b, d, c })) == "03 02 04");

    // The same edge in another occurrence is another feature.
    PickedGeometry other = a;
    other.occurrenceKey.hex = "0B";
    CHECK_FALSE(SameFeature(a, other));
    CHECK(ApplySelectionChange({ a }, { a }, { a, other }).size() == 2);
}

TEST_CASE("only a real change to the selection changes the list")
{
    const PickedGeometry a = Edge("01", {});
    const PickedGeometry b = Edge("02", {});

    // Clearing a selection the list hasn't been told about empties nothing.
    CHECK(Keys(ApplySelectionChange({ a, b }, {}, {})) == "01 02");

    // Then selecting the list's own features adds nothing twice.
    CHECK(Keys(ApplySelectionChange({ a, b }, {}, { b, a })) == "01 02");

    // A design point can't be selected in the view, so other changes keep it.
    PickedGeometry point;
    point.kind = PickKind::DesignPoint;
    point.targetName = "Point<1>";
    CHECK(Keys(ApplySelectionChange({ point, a }, { a }, { a, b })) == "Point<1> 01 02");
    CHECK(Keys(ApplySelectionChange({ point, a }, { a }, {})) == "Point<1>");
}

TEST_CASE("design points are the same feature when their names match")
{
    PickedGeometry point;
    point.kind = PickKind::DesignPoint;
    point.targetName = "Point<1>";
    PickedGeometry samePoint = point;
    samePoint.point = { 1, 1, 1 };
    CHECK(SameFeature(point, samePoint));
    samePoint.targetName = "Point<2>";
    CHECK_FALSE(SameFeature(point, samePoint));
}

TEST_CASE("minimum distance starts at the first feature and takes the nearest each time")
{
    const std::vector<size_t> order =
        SortFeatures(Points({ { 0, 0, 0 }, { 10, 0, 0 }, { 1, 0, 0 }, { 5, 0, 0 } }), FeatureOrder::MinimumDistance,
                     StartCorner::TopLeft);
    CHECK(order == std::vector<size_t>{ 0, 2, 3, 1 });
    CHECK(SortFeatures({}, FeatureOrder::MinimumDistance, StartCorner::TopLeft).empty());
}

TEST_CASE("rows along X zigzag from the chosen corner")
{
    // A 3 by 2 grid, given out of order. Index: position.
    // 0: (0,10)  1: (20,0)  2: (10,10)  3: (0,0)  4: (20,10)  5: (10,0.3)
    const std::vector<Vec3> grid =
        Points({ { 0, 10, 0 }, { 20, 0, 0 }, { 10, 10, 0 }, { 0, 0, 0 }, { 20, 10, 0 }, { 10, 0.3, 0 } });

    CHECK(SortFeatures(grid, FeatureOrder::RowsXFirst, StartCorner::BottomLeft) ==
          std::vector<size_t>{ 3, 5, 1, 4, 2, 0 });
    CHECK(SortFeatures(grid, FeatureOrder::RowsXFirst, StartCorner::TopLeft) ==
          std::vector<size_t>{ 0, 2, 4, 1, 5, 3 });
    CHECK(SortFeatures(grid, FeatureOrder::RowsXFirst, StartCorner::TopRight) ==
          std::vector<size_t>{ 4, 2, 0, 3, 5, 1 });
    CHECK(SortFeatures(grid, FeatureOrder::RowsXFirst, StartCorner::BottomRight) ==
          std::vector<size_t>{ 1, 5, 3, 0, 2, 4 });
}

TEST_CASE("columns along Y zigzag from the chosen corner")
{
    // 0: (0,10)  1: (20,0)  2: (10,10)  3: (0,0)  4: (20,10)  5: (10,0.3)
    const std::vector<Vec3> grid =
        Points({ { 0, 10, 0 }, { 20, 0, 0 }, { 10, 10, 0 }, { 0, 0, 0 }, { 20, 10, 0 }, { 10, 0.3, 0 } });

    CHECK(SortFeatures(grid, FeatureOrder::RowsYFirst, StartCorner::BottomLeft) ==
          std::vector<size_t>{ 3, 0, 2, 5, 1, 4 });
    CHECK(SortFeatures(grid, FeatureOrder::RowsYFirst, StartCorner::TopRight) ==
          std::vector<size_t>{ 4, 1, 5, 2, 0, 3 });
}

TEST_CASE("an operation's features load back in order")
{
    const fs::path path = TempCam();

    Job job;
    SequenceStep origin;
    origin.id = NewStableId();
    origin.name = "O1";
    job.sequence.push_back(origin);

    SequenceStep group;
    group.type = StepType::Group;
    group.id = NewStableId();
    group.name = "F1";

    SequenceStep drill;
    drill.type = StepType::Drill;
    drill.id = NewStableId();
    drill.name = "D1";
    drill.drill.depth = 5;
    drill.features.push_back(Edge("02", { 1, 2, 3 }));

    PickedGeometry hole;
    hole.kind = PickKind::CylindricalFace;
    hole.targetKey = { 17, "05" };
    hole.start = { 0, 0, 0 };
    hole.end = { 0, 0, 8 };
    hole.radius = 3;
    drill.features.push_back(hole);

    PickedGeometry point;
    point.kind = PickKind::DesignPoint;
    point.targetName = "Point<1>";
    point.point = { 4, 4, 4 };
    drill.features.push_back(point);

    group.children.push_back(drill);
    job.sequence.push_back(group);

    REQUIRE(SaveJob(job, path).ok);
    const LoadResult loaded = LoadJob(path);
    std::error_code ignored;
    fs::remove(path, ignored);

    REQUIRE(loaded.status == LoadStatus::Ok);
    const std::vector<PickedGeometry>& features = loaded.job.sequence[1].children[0].features;
    REQUIRE(features.size() == 3);
    CHECK(features[0].kind == PickKind::CircleEdge);
    CHECK(features[0].radius == 2.5);
    CHECK(features[0].targetKey.hex == "02");
    CHECK(features[1].kind == PickKind::CylindricalFace);
    CHECK(features[1].end.z == 8);
    CHECK(features[1].radius == 3);
    CHECK(features[2].kind == PickKind::DesignPoint);
    CHECK(features[2].targetName == "Point<1>");
    CHECK(features[2].point.x == 4);
}
