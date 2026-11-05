// PocketToolpathTests.cpp : tests for pocketing toolpaths.

#include <doctest/doctest.h>

#include <cmath>

#include "PocketToolpath.h"

using namespace camcore;

namespace
{
    // A 20 by 20 pocket from the origin, cut with a 4 mm tool, 50% stepover:
    // the wall is 2 in, and loops are 2 apart.
    ProfileChain SquarePocket()
    {
        ProfileChain chain;
        chain.ok = true;
        chain.closed = true;
        chain.path = PathFromPoints({ { 0, 0, 0 }, { 20, 0, 0 }, { 20, 20, 0 }, { 0, 20, 0 } });
        return chain;
    }

    SequenceStep PocketStep(PocketCutType type)
    {
        SequenceStep step;
        step.type = StepType::Pocketing;
        step.clearance.mode = ClearanceMode::AbsoluteZ;
        step.clearance.absoluteZ = 10;
        step.feeds.plunge = 100;
        step.feeds.cut = 1000;
        step.cutLevels.totalDepth = 0;      // one level, at the outline
        step.cutLevels.roughDepth = 0;
        step.cutLevels.finishDepth = 0;
        step.pocketing.cutType = type;
        return step;
    }

    ToolpathResult Generate(const SequenceStep& step, const ProfileChain& outline = SquarePocket())
    {
        static Tool tool;
        tool.diameter = 4;
        PocketToolpathInput input;
        input.operation = &step;
        input.tool = &tool;
        input.outline = outline;
        return GeneratePocketToolpath(input);
    }

    // Every point the tool cuts through, arcs sampled.
    Polyline CutPoints(const Toolpath& toolpath)
    {
        Polyline points;
        Vec3 at;
        for (const ToolpathMove& move : toolpath.moves)
        {
            if (move.kind == MoveKind::Feed && move.to.z < 1)
            {
                for (const Vec3& p : MovePoints(at, move))
                    points.push_back(p);
            }
            at = move.to;
        }
        return points;
    }

    void CheckInsideWall(const Toolpath& toolpath)
    {
        for (const Vec3& p : CutPoints(toolpath))
        {
            CHECK(p.x >= 2 - 1e-6);
            CHECK(p.x <= 18 + 1e-6);
            CHECK(p.y >= 2 - 1e-6);
            CHECK(p.y <= 18 + 1e-6);
        }
    }

    size_t Count(const Toolpath& toolpath, MoveKind kind)
    {
        size_t count = 0;
        for (const ToolpathMove& move : toolpath.moves)
            count += (move.kind == kind) ? 1 : 0;
        return count;
    }
}

TEST_CASE("offset cuts follow the wall in loops a stepover apart, from the wall in")
{
    SequenceStep step = PocketStep(PocketCutType::Offset);
    step.pocketing.start = PocketStart::Outside;
    const ToolpathResult result = Generate(step);
    REQUIRE(result.ok);
    CheckInsideWall(result.toolpath);

    // It plunges onto the outermost loop, and the loops are 2 apart: x 2, 4, 6, 8.
    const std::vector<ToolpathMove>& moves = result.toolpath.moves;
    const Vec3 first = moves[1].to;
    CHECK((std::fabs(first.x - 2) < 1e-6 || std::fabs(first.x - 18) < 1e-6 || std::fabs(first.y - 2) < 1e-6 ||
           std::fabs(first.y - 18) < 1e-6));
    bool reached8 = false;
    for (const Vec3& p : CutPoints(result.toolpath))
        reached8 = reached8 || (std::fabs(p.x - 8) < 1e-6 && p.y > 8 - 1e-6 && p.y < 12 + 1e-6);
    CHECK(reached8);

    // Steps from loop to loop stay down: in once and out once.
    CHECK(Count(result.toolpath, MoveKind::Rapid) == 2);
}

TEST_CASE("offset cuts from the inside start in the middle")
{
    SequenceStep step = PocketStep(PocketCutType::Offset);
    step.pocketing.start = PocketStart::Inside;
    const ToolpathResult result = Generate(step);
    REQUIRE(result.ok);
    const Vec3 first = result.toolpath.moves[1].to;
    CHECK(first.x >= 8 - 1e-6);
    CHECK(first.x <= 12 + 1e-6);
    CHECK(first.y >= 8 - 1e-6);
    CHECK(first.y <= 12 + 1e-6);
    CheckInsideWall(result.toolpath);
}

TEST_CASE("climb loops turn anticlockwise inside a pocket, conventional clockwise")
{
    SequenceStep step = PocketStep(PocketCutType::Offset);
    step.pocketing.start = PocketStart::Outside;
    Polyline firstLoop;
    for (const Vec3& p : CutPoints(Generate(step).toolpath))
    {
        if (firstLoop.size() < 4)
            firstLoop.push_back(p);
    }
    CHECK(SignedArea(firstLoop) > 0);

    step.pocketing.direction = CutDirection::Conventional;
    firstLoop.clear();
    for (const Vec3& p : CutPoints(Generate(step).toolpath))
    {
        if (firstLoop.size() < 4)
            firstLoop.push_back(p);
    }
    CHECK(SignedArea(firstLoop) < 0);
}

TEST_CASE("linear cuts make rows a stepover apart from the bottom, then lap the wall")
{
    SequenceStep step = PocketStep(PocketCutType::Linear);
    step.pocketing.cutAngle = 90;       // rows along X
    step.pocketing.direction = CutDirection::Mixed;
    const ToolpathResult result = Generate(step);
    REQUIRE(result.ok);
    CheckInsideWall(result.toolpath);

    // The wall runs 2 to 18, so rows at y 4 to 16, from the bottom.
    const std::vector<ToolpathMove>& moves = result.toolpath.moves;
    CHECK(moves[1].to.y == doctest::Approx(4));
    CHECK((moves[1].to.x == doctest::Approx(2) || moves[1].to.x == doctest::Approx(18)));

    // The zigzag and the lap stay down, and the lap goes round the wall at y 18.
    CHECK(Count(result.toolpath, MoveKind::Rapid) == 2);
    bool lappedTop = false;
    for (const Vec3& p : CutPoints(result.toolpath))
        lappedTop = lappedTop || std::fabs(p.y - 18) < 1e-6;
    CHECK(lappedTop);

    // Start at Top begins at the top row.
    step.pocketing.linearStart = PocketLinearStart::Top;
    CHECK(Generate(step).toolpath.moves[1].to.y == doctest::Approx(16));

    // One way, each row lifts.
    step.pocketing.direction = CutDirection::Climb;
    CHECK(Count(Generate(step).toolpath, MoveKind::Rapid) > 2);
}

TEST_CASE("spiral cuts blend from loop to loop, then lap the wall")
{
    SequenceStep step = PocketStep(PocketCutType::Spiral);
    step.pocketing.start = PocketStart::Outside;
    const ToolpathResult result = Generate(step);
    REQUIRE(result.ok);
    CheckInsideWall(result.toolpath);

    // From the wall, the spiral works in to the middle, then lifts to lap the wall last.
    const Polyline points = CutPoints(result.toolpath);
    REQUIRE(points.size() > 50);
    const Vec3& last = points.back();
    CHECK((std::fabs(last.x - 2) < 1e-6 || std::fabs(last.x - 18) < 1e-6 || std::fabs(last.y - 2) < 1e-6 ||
           std::fabs(last.y - 18) < 1e-6));
    bool middle = false;
    for (const Vec3& p : points)
        middle = middle || (p.x > 8 - 1e-6 && p.x < 12 + 1e-6 && p.y > 8 - 1e-6 && p.y < 12 + 1e-6);
    CHECK(middle);

    // From the inside it starts in the middle and ends on the wall without lifting.
    step.pocketing.start = PocketStart::Inside;
    const ToolpathResult inside = Generate(step);
    REQUIRE(inside.ok);
    const Vec3 first = inside.toolpath.moves[1].to;
    CHECK(first.x >= 8 - 1e-6);
    CHECK(first.x <= 12 + 1e-6);
    CHECK(Count(inside.toolpath, MoveKind::Rapid) == 2);
    CheckInsideWall(inside.toolpath);
}

TEST_CASE("pocketing needs a closed outline the tool fits in, and radial isn't built")
{
    SequenceStep step = PocketStep(PocketCutType::Offset);
    ProfileChain open = SquarePocket();
    open.closed = false;
    CHECK_FALSE(Generate(step, open).ok);

    ProfileChain tiny;
    tiny.ok = true;
    tiny.closed = true;
    tiny.path = PathFromPoints({ { 0, 0, 0 }, { 3, 0, 0 }, { 3, 3, 0 }, { 0, 3, 0 } });
    CHECK_FALSE(Generate(step, tiny).ok);

    step.pocketing.cutType = PocketCutType::Radial;
    CHECK_FALSE(Generate(step).ok);

    // Stock to leave keeps the tool further from the wall.
    step = PocketStep(PocketCutType::Offset);
    step.pocketing.stockToLeave = 1;
    for (const Vec3& p : CutPoints(Generate(step).toolpath))
    {
        CHECK(p.x >= 3 - 1e-6);
        CHECK(p.x <= 17 + 1e-6);
    }
}
