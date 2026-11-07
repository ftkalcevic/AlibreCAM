// ParallelFinishToolpathTests.cpp : tests for Parallel Finishing's rows.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>

#include "CamJob.h"
#include "DropCutter.h"
#include "Stock.h"
#include "WaterlineRoughToolpath.h"

using namespace camcore;

namespace
{
    const double kPi = 3.14159265358979323846;

    // A half sphere of radius r on z = 0 at (cx, cy), facets outward.
    std::vector<Vec3> Dome(double cx, double cy, double r, int around = 96, int up = 48)
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

    SequenceStep ParallelStep()
    {
        SequenceStep step;
        step.type = StepType::ParallelFinishing;
        step.clearance.mode = ClearanceMode::AbsoluteZ;
        step.clearance.absoluteZ = 30;
        step.feeds.plunge = 150;
        step.feeds.cut = 1200;
        step.parallelFinishing.stepoverMode = WaterlineStepoverMode::Distance;
        step.parallelFinishing.stepoverDistance = 1.0;
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
        input.partTriangles = Dome(30, 30, 15);
        input.haveStockMaxZ = true;
        input.stockMaxZ = 20;
        input.haveStockMinZ = true;
        input.stockMinZ = 0;
        input.havePartMaxZ = true;
        input.partMaxZ = 15;
        return input;
    }

    // The tip's height for a ball of radius 3 over the dome, d from its middle.
    double BallTip(double d)
    {
        return (d < 18.0) ? std::sqrt(18.0 * 18.0 - d * d) - 3.0 : 0.0;
    }

    std::vector<std::pair<Vec3, Vec3>> CutSegments(const Toolpath& toolpath)
    {
        std::vector<std::pair<Vec3, Vec3>> segments;
        for (size_t i = 1; i < toolpath.moves.size(); ++i)
        {
            if (toolpath.moves[i].kind == MoveKind::Feed && toolpath.moves[i].feed == 1200)
                segments.push_back({ toolpath.moves[i - 1].to, toolpath.moves[i].to });
        }
        return segments;
    }

    int Rapids(const Toolpath& toolpath)
    {
        int count = 0;
        for (const ToolpathMove& move : toolpath.moves)
            count += (move.kind == MoveKind::Rapid) ? 1 : 0;
        return count;
    }
}

TEST_CASE("a ball nose's rows follow a half sphere without cutting into it")
{
    const SequenceStep step = ParallelStep();
    const Tool tool = Ball(6);
    const ToolpathResult result = GenerateParallelFinishToolpath(Input(step, tool));
    REQUIRE(result.ok);
    const auto segments = CutSegments(result.toolpath);
    REQUIRE(segments.size() > 100);
    int checked = 0;
    for (const auto& [a, b] : segments)
    {
        // Along each cut, never below the ball's true height, and close to it where the dome is gentle.
        for (double f : { 0.0, 0.25, 0.5, 0.75, 1.0 })
        {
            const Vec3 p = a + (b - a) * f;
            const double d = std::hypot(p.x - 30, p.y - 30);
            const double expected = BallTip(d);
            // The dome's facets are a little inside the sphere (0.008 at its rim, where
            // the tip's height changes many times faster than across), and the moves
            // are simplified to 0.01, so the tip is a little low at most.
            if (d < 17.0)
                CHECK(p.z >= expected - 0.05);
            if (d < 12.0)
            {
                CHECK(p.z <= expected + 0.015);
                ++checked;
            }
        }
    }
    CHECK(checked > 100);
}

TEST_CASE("the rows are smooth over a dome: no steps")
{
    SequenceStep step = ParallelStep();
    step.parallelFinishing.direction = CutDirection::Climb;
    const Tool tool = Ball(6);
    const ToolpathResult result = GenerateParallelFinishToolpath(Input(step, tool));
    REQUIRE(result.ok);
    // Along a row over the dome, the slope changes smoothly: no cut turns up and
    // down again over a short distance.
    const auto segments = CutSegments(result.toolpath);
    int wobbles = 0;
    for (size_t i = 1; i < segments.size(); ++i)
    {
        const auto& [a, b] = segments[i - 1];
        const auto& [c, e] = segments[i];
        if (!(b.x == c.x && b.y == c.y && b.z == c.z))
            continue;
        if (std::hypot(b.x - 30, b.y - 30) > 17.0)
            continue;
        const double run1 = std::hypot(b.x - a.x, b.y - a.y), run2 = std::hypot(e.x - c.x, e.y - c.y);
        if (run1 < 1e-9 || run2 < 1e-9)
            continue;
        const double slope1 = (b.z - a.z) / run1, slope2 = (e.z - c.z) / run2;
        // Over a dome, the slope along the way the tool goes only falls.
        if (slope2 - slope1 > 0.05)
            ++wobbles;
    }
    CHECK(wobbles == 0);
}

TEST_CASE("a ball's contacts are moved onto the true surface")
{
    SequenceStep step = ParallelStep();
    step.parallelFinishing.direction = CutDirection::Climb;
    const Tool tool = Ball(6);
    WaterlineRoughToolpathInput input = Input(step, tool);
    // A coarse dome, so its facets are well inside the sphere (0.13 mm at most).
    input.partTriangles = Dome(30, 30, 15, 24, 12);
    const auto worst = [&](const ToolpathResult& result) {
        double error = 0.0;
        for (const ToolpathMove& move : result.toolpath.moves)
        {
            const double d = std::hypot(move.to.x - 30, move.to.y - 30);
            if (move.kind == MoveKind::Feed && move.feed == 1200 && d < 16.5)
                error = (std::max)(error, std::fabs(move.to.z - BallTip(d)));
        }
        return error;
    };

    const ToolpathResult facets = GenerateParallelFinishToolpath(input);
    REQUIRE(facets.ok);
    CHECK(facets.refinedPoints == 0);
    CHECK(worst(facets) > 0.05);

    input.partTriangleFaces.assign(input.partTriangles.size() / 3, 0);
    size_t calls = 0;
    input.closestPoint = [&](int face, const Vec3& p, Vec3& closest, bool& onFace) {
        ++calls;
        const Vec3 centre{ 30, 30, 0 };
        const Vec3 d = p - centre;
        const double length = Length(d);
        if (face != 0 || length < 1e-12)
            return false;
        closest = centre + d * (15.0 / length);
        onFace = closest.z >= -1e-9;
        return true;
    };
    const ToolpathResult exact = GenerateParallelFinishToolpath(input);
    REQUIRE(exact.ok);
    CHECK(exact.refinedPoints > 1000);
    CHECK(exact.refineCalls == calls);
    CHECK(worst(exact) < 0.002);
    CHECK(exact.refinedPoints + exact.refineNoAnswer + exact.refineOnSurface + exact.refineSideContact +
              exact.refineUnsettled + exact.refineOffFace + exact.refineTooFar ==
          exact.refineTries);

    // When Alibre can't answer, every point keeps the triangles' answer and says why.
    input.closestPoint = [](int, const Vec3&, Vec3&, bool&) { return false; };
    const ToolpathResult unanswered = GenerateParallelFinishToolpath(input);
    REQUIRE(unanswered.ok);
    CHECK(unanswered.refinedPoints == 0);
    CHECK(unanswered.refineNoAnswer == unanswered.refineTries);
    CHECK(unanswered.refineTries > 1000);
}

TEST_CASE("a refined contact never goes below another face")
{
    // The dome (face 0) with a thin plate (face 1) just above its top, along X.
    SequenceStep step = ParallelStep();
    const Tool tool = Ball(6);
    WaterlineRoughToolpathInput input = Input(step, tool);
    input.partTriangles = Dome(30, 30, 15);
    input.partTriangleFaces.assign(input.partTriangles.size() / 3, 0);
    const std::vector<Vec3> plate = { { 0, 29, 15.2 }, { 60, 29, 15.2 }, { 60, 31, 15.2 },
                                      { 0, 29, 15.2 }, { 60, 31, 15.2 }, { 0, 31, 15.2 } };
    input.partTriangles.insert(input.partTriangles.end(), plate.begin(), plate.end());
    input.partTriangleFaces.push_back(1);
    input.partTriangleFaces.push_back(1);
    input.closestPoint = [&](int face, const Vec3& p, Vec3& closest, bool& onFace) {
        if (face != 0)
        {
            closest = { p.x, (std::max)(29.0, (std::min)(31.0, p.y)), 15.2 };
            onFace = p.x >= 0 && p.x <= 60;
            return true;
        }
        const Vec3 d = p - Vec3{ 30, 30, 0 };
        closest = Vec3{ 30, 30, 0 } + d * (15.0 / Length(d));
        onFace = closest.z >= -1e-9;
        return true;
    };
    const ToolpathResult result = GenerateParallelFinishToolpath(input);
    REQUIRE(result.ok);
    for (const ToolpathMove& move : result.toolpath.moves)
    {
        // Over the plate the ball can't be lower than resting on it.
        if (move.kind == MoveKind::Feed && std::fabs(move.to.y - 30) < 0.5 && std::fabs(move.to.x - 30) < 5)
            CHECK(move.to.z >= 15.2 - 1e-6);
    }
}

TEST_CASE("the drop cutter is exact for flat, ball, V and stock to leave")
{
    // A big flat triangle at z = 5, edge along y = 0, the part on y < 0.
    const std::vector<Vec3> flatFace = { { -50, -50, 5 }, { 50, -50, 5 }, { 0, 0, 5 } , { -50, -50, 5 }, { 0, 0, 5 }, { -50, 0, 5 } };
    ToolProfile ball;
    ball.type = ToolType::BallMill;
    ball.radius = 3;
    ball.length = 40;
    double tip = 0.0;
    {
        const DropCutter cutter(flatFace, ball, 0.0);
        REQUIRE(cutter.TipAt(-10, -10, tip));
        CHECK(tip == doctest::Approx(5.0));
        // Off the edge by 1: the ball rolls down by r - sqrt(r^2 - 1).
        REQUIRE(cutter.TipAt(-10, 1, tip));
        CHECK(tip == doctest::Approx(5.0 - (3.0 - std::sqrt(8.0))).epsilon(1e-6));
        CHECK_FALSE(cutter.TipAt(-10, 3.5, tip));
    }
    {
        const DropCutter cutter(flatFace, ball, 0.5);
        REQUIRE(cutter.TipAt(-10, -10, tip));
        CHECK(tip == doctest::Approx(5.5));
    }
    ToolProfile flat;
    flat.radius = 3;
    flat.length = 40;
    {
        const DropCutter cutter(flatFace, flat, 0.0);
        REQUIRE(cutter.TipAt(-10, 2.9, tip));
        CHECK(tip == doctest::Approx(5.0));
    }
    // A plane sloping up at 30 degrees along +X: the ball's centre is r / cos above it.
    const double t30 = std::tan(kPi / 6.0);
    const std::vector<Vec3> slope = { { -50, -50, -50 * t30 }, { 50, -50, 50 * t30 }, { 50, 50, 50 * t30 },
                                      { -50, -50, -50 * t30 }, { 50, 50, 50 * t30 }, { -50, 50, -50 * t30 } };
    {
        const DropCutter cutter(slope, ball, 0.0);
        REQUIRE(cutter.TipAt(0, 0, tip));
        CHECK(tip == doctest::Approx(3.0 / std::cos(kPi / 6.0) - 3.0).epsilon(1e-6));
    }
    // A V of 90 degrees over a spike: the tip comes down by the distance.
    ToolProfile v;
    v.type = ToolType::VMill;
    v.radius = 5;
    v.length = 40;
    v.slope = 1.0;
    const std::vector<Vec3> spike = { { 0, 0, 10 }, { 0.001, 0, 0 }, { 0, 0.001, 0 } };
    {
        const DropCutter cutter(spike, v, 0.0);
        REQUIRE(cutter.TipAt(2, 0, tip));
        CHECK(tip == doctest::Approx(8.0).epsilon(1e-3));
    }
}

TEST_CASE("rows run along the angle of cuts")
{
    for (double angle : { 0.0, 90.0 })
    {
        SequenceStep step = ParallelStep();
        step.parallelFinishing.cutAngle = angle;
        const Tool tool = Ball(6);
        const ToolpathResult result = GenerateParallelFinishToolpath(Input(step, tool));
        REQUIRE(result.ok);
        double alongX = 0.0, alongY = 0.0;
        for (const auto& [a, b] : CutSegments(result.toolpath))
        {
            alongX += std::fabs(b.x - a.x);
            alongY += std::fabs(b.y - a.y);
        }
        if (angle == 0.0)
            CHECK(alongY > 10.0 * alongX);      // 0 runs along Y
        else
            CHECK(alongX > 10.0 * alongY);      // 90 along X
    }
}

TEST_CASE("one way rows lift between them, mixed zigzags staying down")
{
    const Tool tool = Ball(6);
    const auto rapids = [&](CutDirection direction) {
        SequenceStep step = ParallelStep();
        step.parallelFinishing.direction = direction;
        const ToolpathResult result = GenerateParallelFinishToolpath(Input(step, tool));
        REQUIRE(result.ok);
        return Rapids(result.toolpath);
    };
    const int climb = rapids(CutDirection::Climb);
    const int conventional = rapids(CutDirection::Conventional);
    const int mixed = rapids(CutDirection::Mixed);
    CHECK(climb > 50);
    CHECK(conventional > 50);
    CHECK(mixed < 10);
}

TEST_CASE("climb and conventional cut the rows opposite ways")
{
    const Tool tool = Ball(6);
    const auto firstWay = [&](CutDirection direction) {
        SequenceStep step = ParallelStep();
        step.parallelFinishing.direction = direction;
        const ToolpathResult result = GenerateParallelFinishToolpath(Input(step, tool));
        REQUIRE(result.ok);
        const auto segments = CutSegments(result.toolpath);
        REQUIRE(!segments.empty());
        return segments.front().second.y - segments.front().first.y;
    };
    CHECK(firstWay(CutDirection::Climb) * firstWay(CutDirection::Conventional) < 0.0);
}

TEST_CASE("the tool's centre goes to the area's edge, and past it by the overtravel")
{
    const Tool tool = Ball(6);
    const auto lowestX = [&](double overtravel) {
        SequenceStep step = ParallelStep();
        step.parallelFinishing.extension = overtravel;
        const ToolpathResult result = GenerateParallelFinishToolpath(Input(step, tool));
        REQUIRE(result.ok);
        double lowest = 1e9;
        for (const auto& [a, b] : CutSegments(result.toolpath))
            lowest = (std::min)(lowest, (std::min)(a.x, b.x));
        return lowest;
    };
    CHECK(lowestX(0.0) > -0.5);
    CHECK(lowestX(0.0) < 0.5);
    CHECK(lowestX(3.0) < -2.5);
}

TEST_CASE("parallel finishing is one pass whatever the stepdown says")
{
    const Tool tool = Ball(6);
    SequenceStep single = ParallelStep();
    const ToolpathResult one = GenerateParallelFinishToolpath(Input(single, tool));
    REQUIRE(one.ok);
    for (WaterlineStepdownMode mode : { WaterlineStepdownMode::Distance, WaterlineStepdownMode::Percent,
                                        WaterlineStepdownMode::Scallop })
    {
        SequenceStep step = ParallelStep();
        step.parallelFinishing.stepdownMode = mode;
        step.parallelFinishing.stepDown = 5.0;
        step.parallelFinishing.stepdownPercent = 25.0;
        Tool flat;                          // a scallop stepdown isn't refused either
        flat.diameter = 6;
        flat.overallLength = 40;
        const ToolpathResult result =
            GenerateParallelFinishToolpath(Input(step, mode == WaterlineStepdownMode::Scallop ? flat : tool));
        REQUIRE(result.ok);
        if (mode != WaterlineStepdownMode::Scallop)
            CHECK(result.toolpath.moves.size() == one.toolpath.moves.size());
    }
    // Every row crosses the dome once: no cut is ever flat at a level above the stock's bottom.
    for (const ToolpathMove& move : one.toolpath.moves)
    {
        if (move.kind == MoveKind::Feed && std::fabs(move.to.z - 10.0) < 1e-9)
            CHECK(std::hypot(move.to.x - 30, move.to.y - 30) < 15.0);
    }
}

TEST_CASE("a scallop stepover needs a ball nose")
{
    SequenceStep step = ParallelStep();
    step.parallelFinishing.stepoverMode = WaterlineStepoverMode::Scallop;
    step.parallelFinishing.scallop = 0.01;
    const Tool ball = Ball(6);
    CHECK(GenerateParallelFinishToolpath(Input(step, ball)).ok);
    Tool flat;
    flat.diameter = 6;
    flat.overallLength = 40;
    const ToolpathResult refused = GenerateParallelFinishToolpath(Input(step, flat));
    CHECK_FALSE(refused.ok);
    CHECK(refused.problem.find("ball") != std::string::npos);
}

TEST_CASE("a parallel finishing operation saves its settings, with its own defaults")
{
    CHECK(IsOperation(StepType::ParallelFinishing));
    SequenceStep fresh;
    CHECK(fresh.parallelFinishing.direction == CutDirection::Mixed);
    CHECK(fresh.parallelFinishing.stepdownMode == WaterlineStepdownMode::Levels);
    CHECK(fresh.parallelFinishing.levels == 1);

    SequenceStep parallel = ParallelStep();
    parallel.parallelFinishing.stockToLeave = 0.2;
    parallel.parallelFinishing.extension = 2.5;
    parallel.parallelFinishing.cutAngle = 45;
    parallel.parallelFinishing.linearStart = PocketLinearStart::Top;
    parallel.parallelFinishing.direction = CutDirection::Conventional;

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "AlibreCAM-parallel-test.cam";
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
    parallel.id = NewStableId();
    parallel.name = "Parallel Finishing 1";
    group.children.push_back(parallel);
    job.sequence.push_back(group);
    REQUIRE(SaveJob(job, path).ok);
    const LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    const SequenceStep& back = loaded.job.sequence[1].children[0];
    CHECK(back.type == StepType::ParallelFinishing);
    CHECK(SameSettings(parallel, back));
    CHECK(back.parallelFinishing.extension == 2.5);
    CHECK(back.parallelFinishing.cutAngle == 45);
    CHECK(back.parallelFinishing.linearStart == PocketLinearStart::Top);
    CHECK(&WaterlineSettingsOf(back) == &back.parallelFinishing);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

// Run by name (-tc="parallel finishing timing*" --no-skip) to see how long a realistic job takes.
TEST_CASE("parallel finishing timing on a half sphere" * doctest::skip())
{
    SequenceStep step = ParallelStep();
    step.parallelFinishing.stepoverDistance = 0.3;
    const Tool tool = Ball(6);
    const auto t0 = std::chrono::steady_clock::now();
    const ToolpathResult result = GenerateParallelFinishToolpath(Input(step, tool));
    const auto t1 = std::chrono::steady_clock::now();
    REQUIRE(result.ok);
    MESSAGE("parallel finishing: " << result.toolpath.moves.size() << " moves in "
            << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() << " ms");
}
