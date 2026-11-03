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
