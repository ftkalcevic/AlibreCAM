// WaterlineFinishToolpathTests.cpp : tests for Waterline Finishing's laps and levels.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <set>

#include "CamJob.h"
#include "Stock.h"
#include "WaterlineRoughToolpath.h"

using namespace camcore;

namespace
{
    const double kPi = 3.14159265358979323846;

    // A half sphere of radius r on z = 0 at (cx, cy), facets outward.
    std::vector<Vec3> HalfSphere(double cx, double cy, double r, int around = 96, int up = 48)
    {
        std::vector<Vec3> mesh;
        const auto at = [&](int a, int u) {
            const double theta = 2.0 * kPi * a / around;
            const double phi = 0.5 * kPi * u / up;
            return Vec3{ cx + r * std::cos(phi) * std::cos(theta), cy + r * std::cos(phi) * std::sin(theta),
                         r * std::sin(phi) };
        };
        for (int u = 0; u < up; ++u)
        {
            for (int a = 0; a < around; ++a)
            {
                const Vec3 p00 = at(a, u), p10 = at(a + 1, u), p01 = at(a, u + 1), p11 = at(a + 1, u + 1);
                mesh.insert(mesh.end(), { p00, p10, p11 });
                if (u + 1 < up)
                    mesh.insert(mesh.end(), { p00, p11, p01 });
            }
        }
        return mesh;
    }

    // The 12 triangles of a box, facing out.
    std::vector<Vec3> Box(const Vec3& a, const Vec3& b)
    {
        const Vec3 c[8] = {
            { a.x, a.y, a.z }, { b.x, a.y, a.z }, { b.x, b.y, a.z }, { a.x, b.y, a.z },
            { a.x, a.y, b.z }, { b.x, a.y, b.z }, { b.x, b.y, b.z }, { a.x, b.y, b.z },
        };
        const int faces[6][4] = { { 0, 3, 2, 1 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 },
                                  { 1, 2, 6, 5 }, { 2, 3, 7, 6 }, { 3, 0, 4, 7 } };
        std::vector<Vec3> mesh;
        for (const auto& f : faces)
        {
            for (int i : { f[0], f[1], f[2], f[0], f[2], f[3] })
                mesh.push_back(c[i]);
        }
        return mesh;
    }

    SequenceStep FinishStep()
    {
        SequenceStep step;
        step.type = StepType::WaterlineFinishing;
        step.clearance.mode = ClearanceMode::AbsoluteZ;
        step.clearance.absoluteZ = 30;
        step.feeds.plunge = 150;
        step.feeds.cut = 1200;
        step.waterlineFinishing.stepDown = 1.0;
        step.waterlineFinishing.stepoverMode = WaterlineStepoverMode::Distance;
        step.waterlineFinishing.stepoverDistance = 100.0;   // no extra levels unless asked
        step.waterlineFinishing.clearFlats = false;
        return step;
    }

    Tool Ball(double diameter)
    {
        Tool tool;
        tool.type = ToolType::BallMill;
        tool.diameter = diameter;
        tool.overallLength = 40;
        return tool;
    }

    // A 60 x 60 x 20 stock, its footprint the area, and a half sphere of radius 15 in its middle.
    WaterlineRoughToolpathInput Input(const SequenceStep& step, const Tool& tool)
    {
        WaterlineRoughToolpathInput input;
        input.operation = &step;
        input.tool = &tool;
        ProfileChain footprint;
        footprint.ok = true;
        footprint.closed = true;
        footprint.path = StockFootprint(Frame(), { { 0, 0, 0 }, { 60, 60, 20 } });
        input.outlines = { footprint };
        input.partTriangles = HalfSphere(30, 30, 15);
        input.haveStockMaxZ = true;
        input.stockMaxZ = 20;
        input.haveStockMinZ = true;
        input.stockMinZ = 0;
        input.havePartMaxZ = true;
        input.partMaxZ = 15;
        return input;
    }

    std::vector<Vec3> Cuts(const Toolpath& toolpath)
    {
        std::vector<Vec3> points;
        for (const ToolpathMove& move : toolpath.moves)
        {
            if (move.kind == MoveKind::Feed && move.feed == 1200)
                points.push_back(move.to);
        }
        return points;
    }
}

TEST_CASE("a ball nose's laps follow a half sphere at the ball's true distance")
{
    const SequenceStep step = FinishStep();
    const Tool tool = Ball(6);
    const ToolpathResult result = GenerateWaterlineFinishToolpath(Input(step, tool));
    REQUIRE(result.ok);
    const std::vector<Vec3> cuts = Cuts(result.toolpath);
    REQUIRE(cuts.size() > 50);
    std::set<long long> levels;
    for (const Vec3& p : cuts)
    {
        // The ball's centre is 3 above the tip, and 15 + 3 from the sphere's centre.
        const double expected = std::sqrt((std::max)(0.0, 18.0 * 18.0 - (p.z + 3.0) * (p.z + 3.0)));
        CHECK(std::hypot(p.x - 30, p.y - 30) == doctest::Approx(expected).epsilon(0.03));
        levels.insert(std::llround(p.z * 1000));
    }
    CHECK(levels.size() >= 10);     // a lap at each level the sphere reaches
}

TEST_CASE("climb goes round the part clockwise, conventional anticlockwise")
{
    for (CutDirection direction : { CutDirection::Climb, CutDirection::Conventional })
    {
        SequenceStep step = FinishStep();
        step.waterlineFinishing.direction = direction;
        const Tool tool = Ball(6);
        const ToolpathResult result = GenerateWaterlineFinishToolpath(Input(step, tool));
        REQUIRE(result.ok);
        // The turn of the first level's lap.
        const std::vector<Vec3> cuts = Cuts(result.toolpath);
        REQUIRE(cuts.size() > 3);
        double area = 0.0;
        const double z = cuts.front().z;
        for (size_t i = 0; i + 1 < cuts.size() && std::fabs(cuts[i + 1].z - z) < 1e-9; ++i)
            area += (cuts[i].x - 30) * (cuts[i + 1].y - 30) - (cuts[i + 1].x - 30) * (cuts[i].y - 30);
        if (direction == CutDirection::Climb)
            CHECK(area < 0.0);
        else
            CHECK(area > 0.0);
    }
}

TEST_CASE("a small stepover adds levels where the slope is shallow")
{
    SequenceStep step = FinishStep();
    const Tool tool = Ball(6);
    const auto levelCount = [&](double stepover) {
        step.waterlineFinishing.stepoverDistance = stepover;
        const ToolpathResult result = GenerateWaterlineFinishToolpath(Input(step, tool));
        REQUIRE(result.ok);
        std::set<long long> levels;
        for (const Vec3& p : Cuts(result.toolpath))
            levels.insert(std::llround(p.z * 1000));
        return levels.size();
    };
    const size_t plain = levelCount(100.0);
    const size_t fine = levelCount(1.0);
    CHECK(fine > plain);
}

TEST_CASE("a scallop stepdown needs a ball nose, and sets the steps from the ball's radius")
{
    SequenceStep step = FinishStep();
    step.waterlineFinishing.stepdownMode = WaterlineStepdownMode::Scallop;
    step.waterlineFinishing.stepdownScallop = 0.1;
    const Tool ball = Ball(6);
    CHECK(WaterlineStepdown(step.waterlineFinishing, 6, 20, 0, 3) == doctest::Approx(2.0 * std::sqrt(0.1 * 5.9)));
    CHECK(GenerateWaterlineFinishToolpath(Input(step, ball)).ok);

    Tool flat;
    flat.diameter = 6;
    flat.overallLength = 40;
    CHECK_FALSE(GenerateWaterlineFinishToolpath(Input(step, flat)).ok);
}

TEST_CASE("depth first finishes each part down to the bottom before the next")
{
    const Tool tool = Ball(6);
    const auto rises = [&](CutLevelOrder order) {
        SequenceStep step = FinishStep();
        step.waterlineFinishing.order = order;
        WaterlineRoughToolpathInput input = Input(step, tool);
        input.partTriangles = Box({ 8, 20, 0 }, { 20, 40, 12 });
        const std::vector<Vec3> second = Box({ 40, 20, 0 }, { 52, 40, 12 });
        input.partTriangles.insert(input.partTriangles.end(), second.begin(), second.end());
        input.partMaxZ = 12;
        const ToolpathResult result = GenerateWaterlineFinishToolpath(input);
        REQUIRE(result.ok);
        int count = 0;
        double last = 1e9;
        for (const Vec3& p : Cuts(result.toolpath))
        {
            if (p.z > last + 1e-6)
                ++count;
            last = p.z;
        }
        return count;
    };
    CHECK(rises(CutLevelOrder::LevelFirst) == 0);
    CHECK(rises(CutLevelOrder::DepthFirst) == 1);
}

TEST_CASE("a waterline finishing operation saves its settings, with its own defaults")
{
    CHECK(IsOperation(StepType::WaterlineFinishing));
    SequenceStep fresh;
    CHECK(fresh.waterlineFinishing.stockToLeave == 0.0);

    SequenceStep finishing = FinishStep();
    finishing.waterlineFinishing.stockToLeave = 0.1;
    finishing.waterlineFinishing.direction = CutDirection::Mixed;
    finishing.waterlineFinishing.stepdownMode = WaterlineStepdownMode::Scallop;
    finishing.waterlineFinishing.stepdownScallop = 0.02;
    finishing.waterlineFinishing.finishFlats = true;

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "AlibreCAM-finish-test.cam";
    Job job;
    job.assembly.name = "Mould";
    job.assembly.guid = "{guid}";
    SequenceStep origin;
    origin.type = StepType::SetOrigin;
    origin.id = NewStableId();
    origin.name = "Set Origin 1";
    job.sequence.push_back(origin);
    SequenceStep group;
    group.type = StepType::Group;
    group.id = NewStableId();
    group.name = "Folder 1";
    finishing.id = NewStableId();
    finishing.name = "Waterline Finishing 1";
    group.children.push_back(finishing);
    job.sequence.push_back(group);
    REQUIRE(SaveJob(job, path).ok);
    const LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    const SequenceStep& back = loaded.job.sequence[1].children[0];
    CHECK(back.type == StepType::WaterlineFinishing);
    CHECK(SameSettings(finishing, back));
    CHECK(back.waterlineFinishing.stockToLeave == 0.1);
    CHECK(back.waterlineFinishing.direction == CutDirection::Mixed);
    CHECK(back.waterlineFinishing.stepdownMode == WaterlineStepdownMode::Scallop);
    CHECK(back.waterlineFinishing.stepdownScallop == 0.02);
    CHECK(back.waterlineFinishing.finishFlats);
    SequenceStep unticked = back;
    unticked.waterlineFinishing.finishFlats = false;
    CHECK_FALSE(SameSettings(back, unticked));
    CHECK(&WaterlineSettingsOf(back) == &back.waterlineFinishing);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

namespace
{
    // A 40 x 40 x 5 block with a half sphere of radius 10 on its top, the block's
    // footprint the area, and a 20 high stock.
    WaterlineRoughToolpathInput ShelfInput(const SequenceStep& step, const Tool& tool)
    {
        WaterlineRoughToolpathInput input;
        input.operation = &step;
        input.tool = &tool;
        ProfileChain footprint;
        footprint.ok = true;
        footprint.closed = true;
        footprint.path = StockFootprint(Frame(), { { 0, 0, 0 }, { 40, 40, 20 } });
        input.outlines = { footprint };
        input.partTriangles = Box({ 0, 0, 0 }, { 40, 40, 5 });
        std::vector<Vec3> dome = HalfSphere(20, 20, 10);
        for (Vec3& p : dome)
            p.z += 5;
        input.partTriangles.insert(input.partTriangles.end(), dome.begin(), dome.end());
        input.haveStockMaxZ = true;
        input.stockMaxZ = 20;
        input.haveStockMinZ = true;
        input.stockMinZ = 0;
        input.havePartMaxZ = true;
        input.partMaxZ = 15;
        return input;
    }

    // The cutting moves at height z, as segments.
    std::vector<std::pair<Vec3, Vec3>> CutsAt(const Toolpath& toolpath, double z)
    {
        std::vector<std::pair<Vec3, Vec3>> segments;
        for (size_t i = 1; i < toolpath.moves.size(); ++i)
        {
            const ToolpathMove& a = toolpath.moves[i - 1];
            const ToolpathMove& b = toolpath.moves[i];
            if (b.kind == MoveKind::Feed && b.feed == 1200 && std::fabs(a.to.z - z) < 1e-6 &&
                std::fabs(b.to.z - z) < 1e-6)
                segments.push_back({ a.to, b.to });
        }
        return segments;
    }

    double DistanceTo(const std::vector<std::pair<Vec3, Vec3>>& segments, double x, double y)
    {
        double best = 1e9;
        for (const auto& [a, b] : segments)
        {
            const double dx = b.x - a.x, dy = b.y - a.y;
            const double length2 = dx * dx + dy * dy;
            double f = length2 > 0.0 ? ((x - a.x) * dx + (y - a.y) * dy) / length2 : 0.0;
            f = (std::max)(0.0, (std::min)(1.0, f));
            best = (std::min)(best, std::hypot(a.x + dx * f - x, a.y + dy * f - y));
        }
        return best;
    }
}

TEST_CASE("finish flats clears a shelf round a dome, and without it only the dome is lapped")
{
    Tool flat;
    flat.type = ToolType::FlatEndMill;
    flat.diameter = 6;
    flat.overallLength = 40;
    SequenceStep step = FinishStep();
    step.waterlineFinishing.stepoverDistance = 2.0;

    step.waterlineFinishing.finishFlats = false;
    const ToolpathResult plain = GenerateWaterlineFinishToolpath(ShelfInput(step, flat));
    REQUIRE(plain.ok);
    for (const auto& [a, b] : CutsAt(plain.toolpath, 5.0))
        CHECK(std::hypot(b.x - 20, b.y - 20) < 14.5);   // the lap round the dome, 10 + 3 out

    step.waterlineFinishing.finishFlats = true;
    const ToolpathResult flats = GenerateWaterlineFinishToolpath(ShelfInput(step, flat));
    REQUIRE(flats.ok);
    const auto shelf = CutsAt(flats.toolpath, 5.0);
    REQUIRE(!shelf.empty());
    // Every place on the shelf the tool's centre can reach is within half a stepover of a pass.
    int missed = 0;
    for (double x = 1; x <= 39; x += 1)
    {
        for (double y = 1; y <= 39; y += 1)
        {
            if (std::hypot(x - 20, y - 20) < 13.5)
                continue;
            if (DistanceTo(shelf, x, y) > 1.5)
                ++missed;
        }
    }
    CHECK(missed == 0);
    // Nothing on the shelf comes closer to the dome than the tool's radius.
    for (const auto& [a, b] : shelf)
        CHECK(std::hypot(b.x - 20, b.y - 20) > 12.8);
}

TEST_CASE("finish flats leaves the stock to leave on the flat")
{
    Tool flat;
    flat.type = ToolType::FlatEndMill;
    flat.diameter = 6;
    flat.overallLength = 40;
    SequenceStep step = FinishStep();
    step.waterlineFinishing.stepoverDistance = 2.0;
    step.waterlineFinishing.stockToLeave = 0.5;
    step.waterlineFinishing.finishFlats = true;
    const ToolpathResult result = GenerateWaterlineFinishToolpath(ShelfInput(step, flat));
    REQUIRE(result.ok);
    CHECK(CutsAt(result.toolpath, 5.0).empty());
    const auto shelf = CutsAt(result.toolpath, 5.5);
    REQUIRE(!shelf.empty());
    CHECK(DistanceTo(shelf, 2, 2) < 1.5);
}

// Run by name (-tc="waterline finishing timing*" --no-skip) to see how long a realistic job takes.
TEST_CASE("waterline finishing timing on a half sphere" * doctest::skip())
{
    SequenceStep step = FinishStep();
    step.waterlineFinishing.stepDown = 0.25;
    step.waterlineFinishing.stepoverDistance = 0.5;
    const Tool tool = Ball(6);
    const auto t0 = std::chrono::steady_clock::now();
    const ToolpathResult result = GenerateWaterlineFinishToolpath(Input(step, tool));
    const auto t1 = std::chrono::steady_clock::now();
    REQUIRE(result.ok);
    MESSAGE("waterline finishing: " << result.toolpath.moves.size() << " moves in "
            << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() << " ms");
}
