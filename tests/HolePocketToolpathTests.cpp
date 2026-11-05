// HolePocketToolpathTests.cpp : tests for hole pocketing toolpaths.

#include <doctest/doctest.h>

#include <cmath>
#include <set>

#include "HolePocketToolpath.h"

using namespace camcore;

namespace
{
    // A 20 mm circle at the origin, cut 10 deep with a 6 mm tool: the wall circle is
    // 7 from the centre, the helix 1.5, the stepover 3, and the step down 1.5 (7 levels).
    PickedGeometry Circle(double x, double y, double radius)
    {
        PickedGeometry pick;
        pick.kind = PickKind::CircleEdge;
        pick.point = { x, y, 0 };
        pick.direction = { 0, 0, 1 };
        pick.radius = radius;
        return pick;
    }

    SequenceStep HoleStep()
    {
        SequenceStep step;
        step.type = StepType::HolePocketing;
        step.clearance.mode = ClearanceMode::AbsoluteZ;
        step.clearance.absoluteZ = 10;
        step.features.push_back(Circle(0, 0, 10));
        return step;
    }

    ToolpathResult Generate(const SequenceStep& step, double toolDiameter = 6)
    {
        static Tool tool;
        tool.diameter = toolDiameter;
        HolePocketToolpathInput input;
        input.operation = &step;
        input.tool = &tool;
        return GenerateHolePocketToolpath(input);
    }

    // Every point the tool passes through, arcs sampled.
    Polyline Points(const Toolpath& toolpath)
    {
        Polyline points;
        Vec3 at;
        for (const ToolpathMove& move : toolpath.moves)
        {
            for (const Vec3& p : MovePoints(at, move))
                points.push_back(p);
            at = move.to;
        }
        return points;
    }

    size_t WallLaps(const Toolpath& toolpath, double radius)
    {
        // Half turns about the centre at the wall's radius, flat.
        size_t halves = 0;
        Vec3 at;
        for (const ToolpathMove& move : toolpath.moves)
        {
            if (move.arc && std::fabs(move.centre.x) < 1e-9 && std::fabs(move.centre.y) < 1e-9 &&
                std::fabs(std::hypot(move.to.x, move.to.y) - radius) < 1e-9 && std::fabs(move.to.z - at.z) < 1e-9)
                ++halves;
            at = move.to;
        }
        return halves;
    }
}

TEST_CASE("a hole is helixed into and spiralled out to its wall at each level")
{
    const SequenceStep step = HoleStep();
    const ToolpathResult result = Generate(step);
    REQUIRE(result.ok);
    const std::vector<ToolpathMove>& moves = result.toolpath.moves;

    // Across at the clearance plane to the helix start, then down at rapid to the approach height.
    CHECK(moves[0].kind == MoveKind::Rapid);
    CHECK(moves[0].to.x == doctest::Approx(1.5));
    CHECK(moves[0].to.z == doctest::Approx(10));
    CHECK(moves[1].kind == MoveKind::Rapid);
    CHECK(moves[1].to.z == doctest::Approx(1));

    // The helix descends at the engage feed, going round anticlockwise for climb.
    CHECK(moves[2].arc);
    CHECK(moves[2].to.z < moves[1].to.z);
    CHECK(moves[2].feed == step.feeds.engage);
    CHECK_FALSE(moves[2].clockwise);
    CHECK(std::hypot(moves[2].to.x, moves[2].to.y) == doctest::Approx(1.5));

    // Nowhere goes past the wall circle or below the hole's depth, and it ends up at the clearance plane.
    for (const Vec3& p : Points(result.toolpath))
    {
        CHECK(std::hypot(p.x, p.y) <= 7 + 1e-6);
        CHECK(p.z >= -10 - 1e-9);
    }
    CHECK(moves.back().to.z == doctest::Approx(10));
    CHECK(std::hypot(moves.back().to.x, moves.back().to.y) < 1e-9);

    // Seven flat levels, the last at the bottom, each lapping the wall once.
    std::set<long long> flats;
    Vec3 at;
    for (const ToolpathMove& move : moves)
    {
        if (move.kind == MoveKind::Feed && move.to.z < 0 && std::fabs(move.to.z - at.z) < 1e-9)
            flats.insert(std::llround(move.to.z * 1000));
        at = move.to;
    }
    CHECK(flats.size() == 7);
    CHECK(*flats.begin() == -10000);
    CHECK(WallLaps(result.toolpath, 7) == 14);
}

TEST_CASE("a clean up pass laps the wall again at each level, and conventional turns clockwise")
{
    SequenceStep step = HoleStep();
    step.holePocketing.cleanUpEachLevel = true;
    step.holePocketing.direction = CutDirection::Conventional;
    const ToolpathResult result = Generate(step);
    REQUIRE(result.ok);
    CHECK(WallLaps(result.toolpath, 7) == 28);
    for (const ToolpathMove& move : result.toolpath.moves)
    {
        if (move.arc)
            CHECK(move.clockwise);
    }
}

TEST_CASE("the step down and helix pitch choices set the levels and the helix's descent")
{
    SequenceStep step = HoleStep();
    step.holePocketing.stepDown = HoleStepDown::Levels;
    step.holePocketing.stepDownLevels = 2;
    step.holePocketing.pitch = HelixPitch::Height;
    step.holePocketing.pitchHeight = 2;
    const std::vector<ToolpathMove> moves = Generate(step).toolpath.moves;

    // From 1 above the top to -5 is 6 down at 2 a turn: three turns, six half turns.
    size_t helixHalves = 0;
    for (size_t i = 2; i < moves.size() && moves[i].feed == step.feeds.engage; ++i)
        ++helixHalves;
    CHECK(helixHalves == 6);
    CHECK(moves[7].to.z == doctest::Approx(-5));
}

TEST_CASE("each hole is cut in turn, sized by its circle or the hole diameter")
{
    SequenceStep step = HoleStep();
    step.features.push_back(Circle(50, 0, 6));
    const ToolpathResult result = Generate(step);
    REQUIRE(result.ok);
    bool second = false;
    for (const Vec3& p : Points(result.toolpath))
    {
        if (p.x > 25)
        {
            second = true;
            CHECK(std::hypot(p.x - 50, p.y) <= 3 + 1e-6);
        }
    }
    CHECK(second);

    // A point has no diameter of its own, so the hole diameter is needed.
    step = HoleStep();
    PickedGeometry point;
    point.kind = PickKind::DesignPoint;
    step.features = { point };
    CHECK_FALSE(Generate(step).ok);
    step.holePocketing.useArcDiameter = false;
    step.holePocketing.diameter = 12;
    const ToolpathResult typed = Generate(step);
    REQUIRE(typed.ok);
    for (const Vec3& p : Points(typed.toolpath))
        CHECK(std::hypot(p.x, p.y) <= 3 + 1e-6);
}

TEST_CASE("a hole pocketing toolpath needs a tool that fits and room above the hole")
{
    SequenceStep step = HoleStep();
    CHECK_FALSE(Generate(step, 22).ok);         // wider than the hole

    step.clearance.absoluteZ = 0.5;             // below the approach height
    CHECK_FALSE(Generate(step).ok);

    step = HoleStep();
    step.features.clear();
    CHECK_FALSE(Generate(step).ok);

    // A tool the hole's size plunges straight down.
    step = HoleStep();
    const ToolpathResult exact = Generate(step, 20);
    REQUIRE(exact.ok);
    CHECK(exact.toolpath.moves[2].kind == MoveKind::Feed);
    CHECK_FALSE(exact.toolpath.moves[2].arc);
}
