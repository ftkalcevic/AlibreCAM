// ToolpathTests.cpp : tests for calculating drill toolpaths, and saving toolpaths.

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <string>

#include "CamJob.h"
#include "DrillToolpath.h"

namespace fs = std::filesystem;
using namespace camcore;

namespace
{
    PickedGeometry Hole(double x, double y, double z)
    {
        PickedGeometry pick;
        pick.kind = PickKind::CircleEdge;
        pick.point = { x, y, z };
        pick.direction = { 0, 0, 1 };
        pick.radius = 2.5;
        return pick;
    }

    SequenceStep DrillStep()
    {
        SequenceStep step;
        step.type = StepType::Drill;
        step.drill.depth = 10;
        step.drill.approachDistance = 2;
        step.feeds.plunge = 100;
        step.feeds.retract = 400;
        step.feeds.spindleRpm = 6000;
        step.clearance.mode = ClearanceMode::AbsoluteZ;
        step.clearance.absoluteZ = 20;
        step.features.push_back(Hole(10, 0, 0));
        return step;
    }

    Tool FiveMillDrill()
    {
        Tool tool;
        tool.type = ToolType::Drill;
        tool.diameter = 5;
        tool.pointAngle = 90;
        return tool;
    }

    ToolpathResult Generate(const SequenceStep& step, const Tool* pTool)
    {
        DrillToolpathInput input;
        input.operation = &step;
        input.tool = pTool;
        input.havePartMaxZ = true;
        input.partMaxZ = 3;
        return GenerateDrillToolpath(input);
    }

    // Moves as "R z", "F z@feed" or "D seconds", ignoring X and Y.
    std::string Describe(const Toolpath& toolpath)
    {
        std::string text;
        char buffer[64];
        for (const ToolpathMove& move : toolpath.moves)
        {
            if (move.kind == MoveKind::Rapid)
                std::snprintf(buffer, sizeof(buffer), "R%g", move.to.z);
            else if (move.kind == MoveKind::Feed)
                std::snprintf(buffer, sizeof(buffer), "F%g@%g", move.to.z, move.feed);
            else
                std::snprintf(buffer, sizeof(buffer), "D%g", move.seconds);
            text += (text.empty() ? "" : " ") + std::string(buffer);
        }
        return text;
    }
}

TEST_CASE("a single hole: across, down to the approach height, drill, retract, up")
{
    const Tool tool = FiveMillDrill();
    const ToolpathResult result = Generate(DrillStep(), &tool);
    REQUIRE(result.ok);
    CHECK(Describe(result.toolpath) == "R20 R2 F-10@100 F2@400 R20");
    CHECK(result.toolpath.moves[0].to.x == 10);
}

TEST_CASE("pecks return to the approach height, then come back down near where they stopped")
{
    SequenceStep step = DrillStep();
    step.drill.pecking = true;
    step.drill.peckDepth = 4;

    const Tool tool = FiveMillDrill();
    const ToolpathResult result = Generate(step, &tool);
    REQUIRE(result.ok);
    CHECK(Describe(result.toolpath) == "R20 R2 F-4@100 R2 R-3.5 F-8@100 R2 R-7.5 F-10@100 F2@400 R20");
}

TEST_CASE("a dwell by time, or by revolutions at the spindle speed")
{
    const Tool tool = FiveMillDrill();

    SequenceStep step = DrillStep();
    step.drill.dwell = DwellMode::Seconds;
    step.drill.dwellSeconds = 0.5;
    CHECK(Describe(Generate(step, &tool).toolpath) == "R20 R2 F-10@100 D0.5 F2@400 R20");

    step.drill.dwell = DwellMode::Revolutions;
    step.drill.dwellRevolutions = 3;    // 3 turns at 6000 RPM is 0.03 s
    CHECK(Describe(Generate(step, &tool).toolpath) == "R20 R2 F-10@100 D0.03 F2@400 R20");
}

TEST_CASE("the drill tip adds the point's length")
{
    const Tool tool = FiveMillDrill();
    CHECK(DrillPointLength(tool) == doctest::Approx(2.5));     // 90 degree point, 2.5 mm radius

    Tool mill = tool;
    mill.type = ToolType::FlatEndMill;
    CHECK(DrillPointLength(mill) == 0);

    SequenceStep step = DrillStep();
    step.drill.addTipToDepth = true;
    CHECK(Describe(Generate(step, &tool).toolpath) == "R20 R2 F-12.5@100 F2@400 R20");
}

TEST_CASE("geometry at the bottom puts the hole's top a depth above it")
{
    SequenceStep step = DrillStep();
    step.drill.location = GeometryLocation::AtBottom;
    step.features[0] = Hole(10, 0, -10);

    const Tool tool = FiveMillDrill();
    CHECK(Describe(Generate(step, &tool).toolpath) == "R20 R2 F-10@100 F2@400 R20");
}

TEST_CASE("holes are drilled in the list's order, moving across at the clearance plane")
{
    SequenceStep step = DrillStep();
    step.features.push_back(Hole(30, 5, 0));

    const Tool tool = FiveMillDrill();
    ToolpathResult result = Generate(step, &tool);
    REQUIRE(result.ok);
    REQUIRE(result.toolpath.moves.size() == 10);
    CHECK(result.toolpath.moves[5].kind == MoveKind::Rapid);
    CHECK(result.toolpath.moves[5].to.x == 30);
    CHECK(result.toolpath.moves[5].to.z == 20);

    // With a transfer feed, the move across is fed. The first move is always rapid.
    step.feeds.transferAtRapid = false;
    step.feeds.transfer = 2000;
    result = Generate(step, &tool);
    CHECK(result.toolpath.moves[0].kind == MoveKind::Rapid);
    CHECK(result.toolpath.moves[5].kind == MoveKind::Feed);
    CHECK(result.toolpath.moves[5].feed == 2000);
}

TEST_CASE("the clearance plane comes from the parts, a fixed Z, or not yet from stock")
{
    double height = 0;
    std::string problem;
    ClearanceSettings clearance;

    CHECK(ClearanceHeight(clearance, true, 3, height, problem));
    CHECK(height == 3 + kAutomaticClearanceAboveParts);

    clearance.mode = ClearanceMode::PartMaxZ;
    clearance.partDistance = 7;
    CHECK(ClearanceHeight(clearance, true, 3, height, problem));
    CHECK(height == 10);
    CHECK_FALSE(ClearanceHeight(clearance, false, 0, height, problem));

    clearance.mode = ClearanceMode::AbsoluteZ;
    clearance.absoluteZ = -4;
    CHECK(ClearanceHeight(clearance, false, 0, height, problem));
    CHECK(height == -4);

    clearance.mode = ClearanceMode::StockMaxZ;
    CHECK_FALSE(ClearanceHeight(clearance, true, 3, height, problem));
    CHECK_FALSE(problem.empty());
}

TEST_CASE("a drill toolpath needs a tool, features, valid settings and room above the holes")
{
    const Tool tool = FiveMillDrill();

    CHECK_FALSE(Generate(DrillStep(), nullptr).ok);

    SequenceStep step = DrillStep();
    step.features.clear();
    CHECK_FALSE(Generate(step, &tool).ok);

    step = DrillStep();
    step.drill.depth = 0;
    CHECK_FALSE(Generate(step, &tool).ok);

    step = DrillStep();
    step.clearance.absoluteZ = 1;       // below the 2 mm approach height
    const ToolpathResult result = Generate(step, &tool);
    CHECK_FALSE(result.ok);
    CHECK_FALSE(result.problem.empty());

    step = DrillStep();
    step.type = StepType::Facing;
    CHECK_FALSE(Generate(step, &tool).ok);
}

TEST_CASE("a hole is measured in the origin's frame")
{
    // A side origin at x = 100, drilling along -X.
    SequenceStep step = DrillStep();
    step.features[0] = Hole(100, 5, 6);
    step.features[0].direction = { 1, 0, 0 };

    DrillToolpathInput input;
    input.operation = &step;
    const Tool tool = FiveMillDrill();
    input.tool = &tool;
    input.frame.origin = { 100, 0, 0 };
    input.frame.xAxis = { 0, 1, 0 };
    input.frame.yAxis = { 0, 0, 1 };
    input.frame.zAxis = { 1, 0, 0 };

    const ToolpathResult result = GenerateDrillToolpath(input);
    REQUIRE(result.ok);
    CHECK(result.toolpath.moves[0].to.x == doctest::Approx(5));
    CHECK(result.toolpath.moves[0].to.y == doctest::Approx(6));
    CHECK(Describe(result.toolpath) == "R20 R2 F-10@100 F2@400 R20");
}

TEST_CASE("a generated toolpath loads back unchanged")
{
    static std::atomic<int> counter{ 0 };
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path path =
        fs::temp_directory_path() / ("camcore_toolpath_" + std::to_string(stamp) + "_" + std::to_string(counter++) + ".cam");

    Job job;
    SequenceStep origin;
    origin.id = NewStableId();
    origin.name = "O1";
    job.sequence.push_back(origin);

    SequenceStep group;
    group.type = StepType::Group;
    group.id = NewStableId();
    group.name = "F1";

    SequenceStep step = DrillStep();
    step.id = NewStableId();
    step.name = "D1";
    step.drill.dwell = DwellMode::Seconds;
    const Tool tool = FiveMillDrill();
    step.toolpath = Generate(step, &tool).toolpath;
    group.children.push_back(step);
    job.sequence.push_back(group);

    REQUIRE(SaveJob(job, path).ok);
    const LoadResult loaded = LoadJob(path);
    std::error_code ignored;
    fs::remove(path, ignored);

    REQUIRE(loaded.status == LoadStatus::Ok);
    CHECK(Describe(loaded.job.sequence[1].children[0].toolpath) == Describe(step.toolpath));
    CHECK(loaded.job.sequence[1].children[0].toolpath.moves[0].to.x == 10);
}
