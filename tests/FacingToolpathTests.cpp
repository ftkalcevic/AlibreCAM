// FacingToolpathTests.cpp : tests for calculating facing toolpaths.

#include <doctest/doctest.h>

#include <cmath>

#include "FacingToolpath.h"

using namespace camcore;

namespace
{
    // A 100 by 50 face at Z 0, cut with a 10 mm tool: 25% step is 2.5, 50% overtravel 5.
    SequenceStep FacingStep()
    {
        SequenceStep step;
        step.type = StepType::Facing;
        step.boundaries = { PathFromPoints({ { 0, 0, 0 }, { 100, 0, 0 }, { 100, 50, 0 }, { 0, 50, 0 } }) };
        step.facing.cutAngle = 90;      // along X
        step.facing.start = StartCorner::BottomLeft;
        step.clearance.mode = ClearanceMode::AbsoluteZ;
        step.clearance.absoluteZ = 10;
        step.feeds.plunge = 100;
        step.feeds.cut = 1000;

        // One pass, at the face itself.
        step.cutLevels.totalDepth = 0;
        step.cutLevels.roughDepth = 0;
        step.cutLevels.finishDepth = 0;
        return step;
    }

    Tool TenMill()
    {
        Tool tool;
        tool.diameter = 10;
        return tool;
    }

    ToolpathResult Generate(const SequenceStep& step)
    {
        static const Tool tool = TenMill();
        FacingToolpathInput input;
        input.operation = &step;
        input.tool = &tool;
        return GenerateFacingToolpath(input);
    }

    bool Near(const Vec3& point, double x, double y, double z)
    {
        return std::fabs(point.x - x) < 1e-9 && std::fabs(point.y - y) < 1e-9 && std::fabs(point.z - z) < 1e-9;
    }

    // The cutting moves at the cut feed, in order.
    std::vector<ToolpathMove> Cuts(const Toolpath& toolpath)
    {
        std::vector<ToolpathMove> cuts;
        for (const ToolpathMove& move : toolpath.moves)
        {
            if (move.kind == MoveKind::Feed && move.feed == 1000)
                cuts.push_back(move);
        }
        return cuts;
    }
}

TEST_CASE("a cut angle of 90 runs along X, and 0 along Y")
{
    const Vec3 x = PassDirection(90);
    CHECK(x.x == doctest::Approx(1));
    CHECK(std::fabs(x.y) < 1e-12);
    const Vec3 y = PassDirection(0);
    CHECK(y.y == doctest::Approx(1));
}

TEST_CASE("climb passes all run the same way, past both ends by the overtravel")
{
    const ToolpathResult result = Generate(FacingStep());
    REQUIRE(result.ok);

    // From -5 to 55 across, 2.5 apart: 25 passes, each from x -5 to 105.
    const std::vector<ToolpathMove> cuts = Cuts(result.toolpath);
    REQUIRE(cuts.size() == 25);

    // Starting at the bottom and stepping up, climb (material on the right) cuts towards -X.
    const std::vector<ToolpathMove>& moves = result.toolpath.moves;
    CHECK(moves[0].kind == MoveKind::Rapid);
    CHECK(Near(moves[0].to, 105, -5, 10));
    CHECK(Near(moves[1].to, 105, -5, 0));
    CHECK(moves[1].feed == 100);
    CHECK(Near(cuts[0].to, -5, -5, 0));
    CHECK(Near(cuts[24].to, -5, 55, 0));

    // Each pass ends by lifting to the clearance plane.
    CHECK(moves[3].kind == MoveKind::Rapid);
    CHECK(moves[3].to.z == 10);
    CHECK(moves.back().to.z == 10);
}

TEST_CASE("conventional cuts the other way")
{
    SequenceStep step = FacingStep();
    step.facing.direction = CutDirection::Conventional;
    const ToolpathResult result = Generate(step);
    REQUIRE(result.ok);
    CHECK(Near(result.toolpath.moves[0].to, -5, -5, 10));
    CHECK(Near(Cuts(result.toolpath)[0].to, 105, -5, 0));
}

TEST_CASE("mixed goes backwards and forwards without lifting")
{
    SequenceStep step = FacingStep();
    step.facing.direction = CutDirection::Mixed;
    step.facing.start = StartCorner::TopRight;
    const ToolpathResult result = Generate(step);
    REQUIRE(result.ok);

    // From the top right corner: first pass at y 55 towards -X, then step down and back.
    const std::vector<ToolpathMove>& moves = result.toolpath.moves;
    CHECK(Near(moves[0].to, 105, 55, 10));
    CHECK(Near(moves[2].to, -5, 55, 0));
    CHECK(Near(moves[3].to, -5, 52.5, 0));    // the step over, at the cut feed
    CHECK(Near(moves[4].to, 105, 52.5, 0));

    size_t lifts = 0;
    for (const ToolpathMove& move : moves)
        lifts += (move.kind == MoveKind::Rapid) ? 1 : 0;
    CHECK(lifts == 2);      // in at the start and out at the end
}

TEST_CASE("mixed steps over square to the passes when their ends don't line up")
{
    SequenceStep step = FacingStep();
    step.facing.direction = CutDirection::Mixed;
    step.boundaries = { PathFromPoints({ { 0, 0, 0 }, { 100, 0, 0 }, { 50, 50, 0 } }) };
    const ToolpathResult result = Generate(step);
    REQUIRE(result.ok);

    // Every move at the cut depth runs along X or along Y, never diagonally.
    const std::vector<ToolpathMove>& moves = result.toolpath.moves;
    size_t steps = 0;
    for (size_t i = 2; i < moves.size(); ++i)
    {
        const Vec3& a = moves[i - 1].to;
        const Vec3& b = moves[i].to;
        if (a.z != 0 || b.z != 0)
            continue;
        const bool alongX = std::fabs(a.y - b.y) < 1e-9;
        const bool alongY = std::fabs(a.x - b.x) < 1e-9;
        CHECK((alongX || alongY));
        steps += (alongY && !alongX) ? 1 : 0;

        // Nothing goes past the triangle plus the 5 mm overtravel.
        CHECK(b.x >= -5 - 1e-9);
        CHECK(b.x <= 105 + 1e-9);
    }
    CHECK(steps == 24);

    // Going up the triangle each pass is shorter, so the step over is at the end
    // of the longer pass below, and the new pass then starts out in the air.
    // The first pass, y -5, runs right from -5 to 105, so the step to y -2.5 is at x 105.
    // (The bottom pass reads its span a hair inside the triangle.)
    CHECK(moves[2].to.x == doctest::Approx(105).epsilon(1e-5));
    CHECK(moves[2].to.y == doctest::Approx(-5));
    CHECK(moves[3].to.x == moves[2].to.x);
    CHECK(moves[3].to.y == doctest::Approx(-2.5));
    CHECK(moves[4].to.x < moves[3].to.x);
}

TEST_CASE("no overtravel stops the cutter centre on the edge")
{
    SequenceStep step = FacingStep();
    step.facing.overtravel.percent = 0;
    const std::vector<ToolpathMove> cuts = Cuts(Generate(step).toolpath);
    REQUIRE(!cuts.empty());
    CHECK(Near(cuts.front().to, 0, 0, 0));
    CHECK(Near(cuts.back().to, 0, 50, 0));
}

TEST_CASE("a cut angle of 0 runs the passes along Y")
{
    SequenceStep step = FacingStep();
    step.facing.cutAngle = 0;
    const std::vector<ToolpathMove> cuts = Cuts(Generate(step).toolpath);
    REQUIRE(!cuts.empty());

    // Passes now run in Y, and step across X: from -5 to 105, 2.5 apart.
    CHECK(cuts.size() == 45);
    for (const ToolpathMove& cut : cuts)
        CHECK(std::fabs(std::fabs(cut.to.y - 25) - 30) < 1e-9);
}

TEST_CASE("a spiral starts at the corner and works inwards round the rectangle")
{
    SequenceStep step = FacingStep();
    step.facing.cutType = FacingCutType::Spiral;
    const ToolpathResult result = Generate(step);
    REQUIRE(result.ok);

    // Climb turns clockwise from the bottom left: up the left side, along the top.
    const std::vector<ToolpathMove>& moves = result.toolpath.moves;
    CHECK(Near(moves[0].to, -5, -5, 10));
    CHECK(Near(moves[2].to, -5, 55, 0));
    CHECK(Near(moves[3].to, 105, 55, 0));
    CHECK(Near(moves[4].to, 105, -5, 0));
    CHECK(Near(moves[5].to, -2.5, -5, 0));
    CHECK(Near(moves[6].to, -2.5, 52.5, 0));

    // Every cut stays inside the rectangle, and it ends lifting out.
    for (const ToolpathMove& move : moves)
    {
        CHECK(move.to.x >= -5 - 1e-9);
        CHECK(move.to.x <= 105 + 1e-9);
        CHECK(move.to.y >= -5 - 1e-9);
        CHECK(move.to.y <= 55 + 1e-9);
    }
    CHECK(moves.back().kind == MoveKind::Rapid);
}

TEST_CASE("a conventional spiral turns anticlockwise, from any corner")
{
    SequenceStep step = FacingStep();
    step.facing.cutType = FacingCutType::Spiral;
    step.facing.direction = CutDirection::Conventional;
    step.facing.start = StartCorner::TopRight;
    const std::vector<ToolpathMove>& moves = Generate(step).toolpath.moves;

    // Anticlockwise from the top right: along the top first, then down the left side.
    CHECK(Near(moves[0].to, 105, 55, 10));
    CHECK(Near(moves[2].to, -5, 55, 0));
    CHECK(Near(moves[3].to, -5, -5, 0));
}

TEST_CASE("same settings ignores what Generate makes")
{
    SequenceStep a = FacingStep();
    SequenceStep b = a;
    b.boundaries.clear();
    b.toolpath.moves.resize(3);
    CHECK(SameSettings(a, b));

    b.facing.cutAngle = 45;
    CHECK_FALSE(SameSettings(a, b));
    b = a;
    b.name = "Other";
    CHECK_FALSE(SameSettings(a, b));
}

TEST_CASE("facing cuts the whole area at each level, lifting between levels")
{
    SequenceStep step = FacingStep();
    step.cutLevels = CutLevelSettings();    // 0.4, 0.8 rough, then 1.0 finish
    const ToolpathResult result = Generate(step);
    REQUIRE(result.ok);

    // Each level has the 25 passes, at its own depth, in order down.
    const std::vector<ToolpathMove> cuts = Cuts(result.toolpath);
    REQUIRE(cuts.size() == 75);
    CHECK(cuts[0].to.z == doctest::Approx(-0.4));
    CHECK(cuts[25].to.z == doctest::Approx(-0.8));
    CHECK(cuts[74].to.z == doctest::Approx(-1.0));

    // Mixed lifts out once at the end of each level, and goes in again.
    step.facing.direction = CutDirection::Mixed;
    size_t lifts = 0;
    for (const ToolpathMove& move : Generate(step).toolpath.moves)
        lifts += (move.kind == MoveKind::Rapid) ? 1 : 0;
    CHECK(lifts == 6);

    // At Bottom starts above the face and finishes on it.
    step.cutLevels.location = CutGeometryLocation::AtBottom;
    const std::vector<ToolpathMove> bottom = Cuts(Generate(step).toolpath);
    REQUIRE(!bottom.empty());
    CHECK(bottom.front().to.z == doctest::Approx(0.6));
    CHECK(bottom.back().to.z == doctest::Approx(0));

    // The clearance plane must be above the top of the cut, not just the face.
    step.clearance.absoluteZ = 0.5;
    CHECK_FALSE(Generate(step).ok);
}

TEST_CASE("a linear entry comes from outside the first pass, and an exit at 0 runs on in line")
{
    // Climb from the bottom left: the first pass at y -5 cuts towards -X, and the
    // passes step up, so the cut side is below.
    SequenceStep step = FacingStep();
    step.entryExit.entry = EntryMode::Entry2D;
    step.entryExit.engage.length = 10;
    step.entryExit.engage.angle = 90;
    step.entryExit.exit = ExitMode::Exit2D;
    step.entryExit.retract.length = 4;
    step.entryExit.retract.angle = 0;
    const std::vector<ToolpathMove>& moves = Generate(step).toolpath.moves;

    CHECK(Near(moves[0].to, 105, -15, 10));     // square to the cut, 10 below its start
    CHECK(Near(moves[1].to, 105, -15, 0));
    CHECK(Near(moves[2].to, 105, -5, 0));       // the lead in
    CHECK(Near(moves[3].to, -5, -5, 0));        // the pass
    CHECK(Near(moves[4].to, -9, -5, 0));        // in line with it, 4 on
    CHECK(Near(moves[5].to, -9, -5, 10));
}

TEST_CASE("a radial entry is a quarter circle ending tangent to the cut")
{
    SequenceStep step = FacingStep();
    step.entryExit.entry = EntryMode::Entry2D;
    step.entryExit.engage.shape = LeadShape::Radial;
    step.entryExit.engage.radius = 5;
    const std::vector<ToolpathMove>& moves = Generate(step).toolpath.moves;

    // The centre is 5 below the start at (105, -5). The arc starts 5 back along
    // the cut (towards +X) from the centre, and turns anticlockwise up onto the cut.
    CHECK(Near(moves[0].to, 110, -10, 10));
    CHECK(Near(moves[1].to, 110, -10, 0));
    REQUIRE(moves[2].arc);
    CHECK(Near(moves[2].to, 105, -5, 0));
    CHECK(Near(moves[2].centre, 105, -10, 0));
    CHECK_FALSE(moves[2].clockwise);
    CHECK(moves[2].feed == 1000);
    CHECK(Near(moves[3].to, -5, -5, 0));
    CHECK_FALSE(moves[3].arc);
}

TEST_CASE("an exit goes out past the last pass, or the other side when negative")
{
    // Mixed from the bottom left steps up, and its 25th pass, at y 55, ends at x 105.
    SequenceStep step = FacingStep();
    step.facing.direction = CutDirection::Mixed;
    step.entryExit.exit = ExitMode::Exit2D;
    step.entryExit.retract.length = 5;
    step.entryExit.retract.angle = 45;
    const double d = 5 / std::sqrt(2.0);

    std::vector<ToolpathMove> moves = Generate(step).toolpath.moves;
    REQUIRE(moves.size() > 2);
    CHECK(Near(moves[moves.size() - 2].to, 105 + d, 55 + d, 0));

    step.entryExit.retract.angle = -45;
    moves = Generate(step).toolpath.moves;
    CHECK(Near(moves[moves.size() - 2].to, 105 + d, 55 - d, 0));

    // Radial curves up, or down with a negative radius, still running on ahead.
    step.entryExit.retract.shape = LeadShape::Radial;
    step.entryExit.retract.radius = 5;
    moves = Generate(step).toolpath.moves;
    const ToolpathMove& up = moves[moves.size() - 2];
    REQUIRE(up.arc);
    CHECK(Near(up.to, 110, 60, 0));
    CHECK(Near(up.centre, 105, 60, 0));
    CHECK_FALSE(up.clockwise);

    step.entryExit.retract.radius = -5;
    moves = Generate(step).toolpath.moves;
    const ToolpathMove& down = moves[moves.size() - 2];
    REQUIRE(down.arc);
    CHECK(Near(down.to, 110, 50, 0));
    CHECK(Near(down.centre, 105, 50, 0));
    CHECK(down.clockwise);
}

TEST_CASE("a negative radius entry still starts behind the cut, on the other side")
{
    // Climb from the bottom left: the first pass starts at (105, -5) cutting towards -X.
    SequenceStep step = FacingStep();
    step.entryExit.entry = EntryMode::Entry2D;
    step.entryExit.engage.shape = LeadShape::Radial;
    step.entryExit.engage.radius = -5;
    const std::vector<ToolpathMove>& moves = Generate(step).toolpath.moves;
    CHECK(Near(moves[0].to, 110, 0, 10));
    REQUIRE(moves[2].arc);
    CHECK(Near(moves[2].to, 105, -5, 0));
    CHECK(Near(moves[2].centre, 105, 0, 0));
    CHECK(moves[2].clockwise);
}

TEST_CASE("passes end exactly on an arc in the boundary")
{
    // A disc of radius 20 at the origin, as two half arcs.
    SequenceStep step = FacingStep();
    PathVertex right;
    right.point = { 20, 0, 0 };
    right.arc = true;
    PathVertex left;
    left.point = { -20, 0, 0 };
    left.arc = true;
    step.boundaries = { Path{ right, left } };

    const ToolpathResult result = Generate(step);
    REQUIRE(result.ok);
    size_t checked = 0;
    for (const ToolpathMove& cut : Cuts(result.toolpath))
    {
        if (std::fabs(cut.to.y) >= 19.9)
            continue;
        CHECK(std::fabs(cut.to.x) == doctest::Approx(std::sqrt(400 - cut.to.y * cut.to.y) + 5));
        ++checked;
    }
    CHECK(checked > 10);
}

TEST_CASE("mixed can step over on half circles bulging out past the ends")
{
    // From the bottom left: the first pass at y -5 runs to x 105, and the passes are 2.5 apart.
    SequenceStep step = FacingStep();
    step.facing.direction = CutDirection::Mixed;
    step.facing.arcStepOver = true;
    const std::vector<ToolpathMove>& moves = Generate(step).toolpath.moves;

    REQUIRE(moves[3].arc);
    CHECK(Near(moves[2].to, 105, -5, 0));
    CHECK(Near(moves[3].to, 105, -2.5, 0));
    CHECK(Near(moves[3].centre, 105, -3.75, 0));
    CHECK_FALSE(moves[3].clockwise);            // out to +X and round, anticlockwise
    CHECK(Near(moves[4].to, -5, -2.5, 0));
    REQUIRE(moves[5].arc);
    CHECK(Near(moves[5].centre, -5, -1.25, 0));
    CHECK(moves[5].clockwise);                  // out to -X and round, clockwise

    size_t arcs = 0;
    for (const ToolpathMove& move : moves)
        arcs += move.arc ? 1 : 0;
    CHECK(arcs == 24);

    // Off, the steps are straight.
    step.facing.arcStepOver = false;
    for (const ToolpathMove& move : Generate(step).toolpath.moves)
        CHECK_FALSE(move.arc);
}

TEST_CASE("along a 3D path ramps down in line with the first cut")
{
    SequenceStep step = FacingStep();
    step.facing.direction = CutDirection::Mixed;
    step.entryExit.entry = EntryMode::AlongPath3D;
    step.entryExit.alongPathHeight = 2;
    step.entryExit.alongPathAngle = 45;
    const std::vector<ToolpathMove>& moves = Generate(step).toolpath.moves;

    // Mixed from the bottom left starts at (-5, -5) cutting towards +X.
    CHECK(Near(moves[0].to, -7, -5, 10));
    CHECK(Near(moves[1].to, -7, -5, 2));
    CHECK(Near(moves[2].to, -5, -5, 0));

    step.clearance.absoluteZ = 2;
    CHECK_FALSE(Generate(step).ok);
}

TEST_CASE("entries and exits at each level, or only into the first and out of the last")
{
    SequenceStep step = FacingStep();
    step.cutLevels = CutLevelSettings();        // three levels
    step.facing.cutType = FacingCutType::Spiral;
    step.entryExit.entry = EntryMode::Entry2D;
    step.entryExit.exit = ExitMode::Exit2D;

    // A spiral is one cut a level. Count the cut moves at each level's depth.
    auto movesAt = [](const Toolpath& toolpath, double z) {
        size_t count = 0;
        for (const ToolpathMove& move : toolpath.moves)
            count += (move.kind == MoveKind::Feed && move.feed == 1000 && std::fabs(move.to.z - z) < 1e-9) ? 1 : 0;
        return count;
    };
    const Toolpath once = Generate(step).toolpath;
    step.entryExit.eachCutLevel = true;
    const Toolpath each = Generate(step).toolpath;

    // The middle level has its entry and exit only when each level gets them.
    CHECK(movesAt(each, -0.8) == movesAt(once, -0.8) + 2);
    CHECK(movesAt(each, -0.4) == movesAt(once, -0.4) + 1);
    CHECK(movesAt(each, -1.0) == movesAt(once, -1.0) + 1);
}

TEST_CASE("each area is faced from its own corner, depth first or level first")
{
    SequenceStep step = FacingStep();
    step.boundaries.push_back(PathFromPoints({ { 200, 0, 0 }, { 240, 0, 0 }, { 240, 20, 0 }, { 200, 20, 0 } }));
    step.cutLevels = CutLevelSettings();        // -0.4, -0.8, -1.0

    // The second area starts at its own bottom left corner, less the overtravel.
    auto visits = [&]() {
        std::vector<std::pair<bool, double>> order;     // second area, level
        for (const ToolpathMove& move : Generate(step).toolpath.moves)
        {
            if (move.kind != MoveKind::Feed || move.to.z > 0)
                continue;
            const std::pair<bool, double> visit{ move.to.x > 150, move.to.z };
            if (order.empty() || order.back() != visit)
                order.push_back(visit);
        }
        return order;
    };
    std::vector<std::pair<bool, double>> order = visits();
    REQUIRE(order.size() == 6);
    CHECK_FALSE(order[2].first);
    CHECK(order[2].second == doctest::Approx(-1.0));
    CHECK(order[3].first);
    CHECK(order[3].second == doctest::Approx(-0.4));

    step.cutLevels.order = CutLevelOrder::LevelFirst;
    order = visits();
    REQUIRE(order.size() == 6);
    CHECK(order[1].first);
    CHECK(order[1].second == doctest::Approx(-0.4));
    CHECK_FALSE(order[2].first);

    bool cornerStart = false;
    for (const ToolpathMove& move : Generate(step).toolpath.moves)
        cornerStart = cornerStart || (move.kind == MoveKind::Rapid && std::fabs(move.to.x - 245) < 1e-9 &&
                                      std::fabs(move.to.y + 5) < 1e-9);
    CHECK(cornerStart);     // climb from the bottom left starts at the right end of the bottom pass
}

TEST_CASE("a facing toolpath needs a tool, a boundary and room above it")
{
    SequenceStep step = FacingStep();
    FacingToolpathInput input;
    input.operation = &step;
    CHECK_FALSE(GenerateFacingToolpath(input).ok);      // no tool

    step.boundaries.clear();
    CHECK_FALSE(Generate(step).ok);

    step = FacingStep();
    step.clearance.absoluteZ = 0;
    CHECK_FALSE(Generate(step).ok);

    step = FacingStep();
    step.type = StepType::Drill;
    CHECK_FALSE(Generate(step).ok);
}
