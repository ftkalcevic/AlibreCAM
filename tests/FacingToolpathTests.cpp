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
        step.boundary = { { 0, 0, 0 }, { 100, 0, 0 }, { 100, 50, 0 }, { 0, 50, 0 } };
        step.facing.cutAngle = 90;      // along X
        step.facing.start = StartCorner::BottomLeft;
        step.clearance.mode = ClearanceMode::AbsoluteZ;
        step.clearance.absoluteZ = 10;
        step.feeds.plunge = 100;
        step.feeds.cut = 1000;
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

TEST_CASE("a facing toolpath needs a tool, a boundary and room above it")
{
    SequenceStep step = FacingStep();
    FacingToolpathInput input;
    input.operation = &step;
    CHECK_FALSE(GenerateFacingToolpath(input).ok);      // no tool

    step.boundary.clear();
    CHECK_FALSE(Generate(step).ok);

    step = FacingStep();
    step.clearance.absoluteZ = 0;
    CHECK_FALSE(Generate(step).ok);

    step = FacingStep();
    step.type = StepType::Drill;
    CHECK_FALSE(Generate(step).ok);
}
