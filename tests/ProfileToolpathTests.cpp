// ProfileToolpathTests.cpp : tests for joining profiles, offsetting them, and profiling toolpaths.

#include <doctest/doctest.h>

#include <cmath>

#include "ProfileToolpath.h"

using namespace camcore;

namespace
{
    Path Line(Vec3 from, Vec3 to) { return PathFromPoints({ from, to }); }

    bool At(const Vec3& point, double x, double y)
    {
        return std::fabs(point.x - x) < 1e-6 && std::fabs(point.y - y) < 1e-6;
    }

    // The anticlockwise 10 by 10 square from the origin, as a closed path.
    Path Square()
    {
        return PathFromPoints({ { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 } });
    }

    double DistanceToSquare(const Vec3& p)
    {
        const double dx = (std::max)({ 0.0 - p.x, 0.0, p.x - 10.0 });
        const double dy = (std::max)({ 0.0 - p.y, 0.0, p.y - 10.0 });
        if (dx > 0 || dy > 0)
            return std::hypot(dx, dy);
        return (std::min)({ p.x, 10.0 - p.x, p.y, 10.0 - p.y });
    }

    SequenceStep ProfilingStep()
    {
        SequenceStep step;
        step.type = StepType::Profiling;
        step.clearance.mode = ClearanceMode::AbsoluteZ;
        step.clearance.absoluteZ = 10;
        step.feeds.plunge = 100;
        step.feeds.cut = 1000;
        step.cutLevels.totalDepth = 0;      // one pass, at the profile
        step.cutLevels.roughDepth = 0;
        step.cutLevels.finishDepth = 0;
        step.profiling.startSide = ProfileSide::Left;
        return step;
    }

    ToolpathResult Generate(const SequenceStep& step, const ProfileChain& profile)
    {
        static Tool tool;
        tool.diameter = 2;
        ProfileToolpathInput input;
        input.operation = &step;
        input.tool = &tool;
        input.profile = profile;
        return GenerateProfileToolpath(input);
    }

    ProfileChain OpenLine(double length)
    {
        return ChainProfile({ { Line({ 0, 0, 0 }, { length, 0, 0 }) } }, 0);
    }
}

TEST_CASE("edges in any order and direction join into a closed profile from the first feature")
{
    // The square's sides, some backwards. The first side's neighbours are the
    // second feature (at the origin) and the third (at (10, 0)), so it runs
    // towards the second: from (10, 0) to the origin.
    const ProfileChain chain = ChainProfile({ { Line({ 0, 0, 0 }, { 10, 0, 0 }) },
                                              { Line({ 0, 10, 0 }, { 0, 0, 0 }) },
                                              { Line({ 10, 0, 0 }, { 10, 10, 0 }) },
                                              { Line({ 10, 10, 0 }, { 0, 10, 0 }) } },
                                            2);
    REQUIRE(chain.ok);
    CHECK(chain.closed);
    REQUIRE(chain.path.size() == 4);
    CHECK(At(chain.path[0].point, 10, 0));
    CHECK(At(chain.path[1].point, 0, 0));
    CHECK(At(chain.path[2].point, 0, 10));
    CHECK(At(chain.path[3].point, 10, 10));
    CHECK(chain.path[0].point.z == 2);
}

TEST_CASE("an open profile starts at the loose end of the earliest feature")
{
    const ProfileChain chain =
        ChainProfile({ { Line({ 10, 10, 0 }, { 10, 0, 0 }) }, { Line({ 0, 0, 0 }, { 10, 0, 0 }) } }, 0);
    REQUIRE(chain.ok);
    CHECK_FALSE(chain.closed);
    REQUIRE(chain.path.size() == 3);
    CHECK(At(chain.path[0].point, 10, 10));
    CHECK(At(chain.path[1].point, 10, 0));
    CHECK(At(chain.path[2].point, 0, 0));
}

TEST_CASE("edges with gaps or branches don't make a profile")
{
    CHECK_FALSE(ChainProfile({ { Line({ 0, 0, 0 }, { 10, 0, 0 }) }, { Line({ 10.5, 0, 0 }, { 20, 0, 0 }) } }, 0).ok);
    CHECK_FALSE(ChainProfile({ { Line({ 0, 0, 0 }, { 10, 0, 0 }) }, { Line({ 0, 0, 0 }, { 0, 10, 0 }) },
                               { Line({ 0, 0, 0 }, { -10, 0, 0 }) } },
                             0)
                     .ok);
    CHECK_FALSE(ChainProfile({}, 0).ok);
}

TEST_CASE("an open profile's offset ends square to its ends")
{
    const std::vector<ProfileRun> runs = OffsetProfile(Line({ 0, 0, 0 }, { 10, 0, 0 }), false, 1);
    REQUIRE(runs.size() == 1);
    CHECK_FALSE(runs[0].closed);
    REQUIRE(runs[0].path.size() == 2);
    CHECK(At(runs[0].path.front().point, 0, 1));
    CHECK(At(runs[0].path.back().point, 10, 1));
}

TEST_CASE("outside a square, the corners are rounded and every point is the offset away")
{
    // Right of an anticlockwise square is outside.
    const std::vector<ProfileRun> runs = OffsetProfile(Square(), true, -1);
    REQUIRE(runs.size() == 1);
    CHECK(runs[0].closed);

    size_t arcs = 0;
    for (const PathVertex& vertex : runs[0].path)
        arcs += vertex.arc ? 1 : 0;
    CHECK(arcs == 4);
    for (const Vec3& point : PathPoints(runs[0].path, true))
        CHECK(DistanceToSquare(point) == doctest::Approx(1).epsilon(1e-6));
}

TEST_CASE("inside a square, the corners are trimmed sharp")
{
    const std::vector<ProfileRun> runs = OffsetProfile(Square(), true, 1);
    REQUIRE(runs.size() == 1);
    CHECK(runs[0].closed);
    REQUIRE(runs[0].path.size() == 4);
    for (const PathVertex& vertex : runs[0].path)
    {
        CHECK_FALSE(vertex.arc);
        CHECK((At(vertex.point, 1, 1) || At(vertex.point, 9, 1) || At(vertex.point, 9, 9) || At(vertex.point, 1, 9)));
    }
}

TEST_CASE("a place too narrow for the tool is left uncut")
{
    // Inside a 10 by 1.5 slot, a tool 1 from each side can't go anywhere.
    const Path slot = PathFromPoints({ { 0, 0, 0 }, { 10, 0, 0 }, { 10, 1.5, 0 }, { 0, 1.5, 0 } });
    CHECK(OffsetProfile(slot, true, 1).empty());

    // A U whose arms are 1.5 apart, cut round the outside: the slot between
    // the arms is too narrow, so that part is left out and nothing goes in it.
    const Path u = PathFromPoints({ { 0, 10, 0 }, { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 5.75, 10, 0 },
                                    { 5.75, 3, 0 }, { 4.25, 3, 0 }, { 4.25, 10, 0 } });
    const std::vector<ProfileRun> runs = OffsetProfile(u, false, -1);     // right of the path is outside
    REQUIRE_FALSE(runs.empty());
    double reach = 0;
    for (const ProfileRun& run : runs)
    {
        for (const Vec3& point : PathPoints(run.path, run.closed))
        {
            // Nowhere is within 1 of the profile.
            double nearest = 1e9;
            const Polyline corners = PathPoints(u, false);
            for (size_t i = 0; i + 1 < corners.size(); ++i)
            {
                const Vec3 a = corners[i], b = corners[i + 1];
                const double lx = b.x - a.x, ly = b.y - a.y;
                double t = ((point.x - a.x) * lx + (point.y - a.y) * ly) / (lx * lx + ly * ly);
                t = (std::max)(0.0, (std::min)(1.0, t));
                nearest = (std::min)(nearest, std::hypot(point.x - a.x - t * lx, point.y - a.y - t * ly));
            }
            CHECK(nearest >= 1 - 1e-3);
            CHECK_FALSE((point.x > 4.25 && point.x < 5.75 && point.y < 10));
            reach = (std::max)(reach, point.x);
        }
    }
    CHECK(reach == doctest::Approx(11));     // it still goes round the outside
}

TEST_CASE("a circle's offset is a circle")
{
    PathVertex right;
    right.point = { 5, 0, 0 };
    right.arc = true;
    PathVertex left;
    left.point = { -5, 0, 0 };
    left.arc = true;
    const std::vector<ProfileRun> runs = OffsetProfile({ right, left }, true, -1);
    REQUIRE(runs.size() == 1);
    for (const Vec3& point : PathPoints(runs[0].path, true))
        CHECK(std::hypot(point.x, point.y) == doctest::Approx(6));
}

TEST_CASE("inside a slot with round ends, the ends' arcs shrink, and too far in leaves nothing")
{
    // A 20 by 10 slot with half circle ends of radius 5, anticlockwise.
    PathVertex a; a.point = { 0, 0, 0 };
    PathVertex b; b.point = { 20, 0, 0 }; b.arc = true; b.centre = { 20, 5, 0 };
    PathVertex c; c.point = { 20, 10, 0 };
    PathVertex d; d.point = { 0, 10, 0 }; d.arc = true; d.centre = { 0, 5, 0 };
    const Path slot{ a, b, c, d };

    const std::vector<ProfileRun> runs = OffsetProfile(slot, true, 2);
    REQUIRE(runs.size() == 1);
    CHECK(runs[0].closed);
    for (const Vec3& point : PathPoints(runs[0].path, true))
    {
        // 2 in from the slot: 3 from its centre line, or from the end centres.
        const double along = (std::max)(0.0, (std::min)(20.0, point.x));
        CHECK(std::hypot(point.x - along, point.y - 5) == doctest::Approx(3));
    }

    CHECK(OffsetProfile(slot, true, 6).empty());
}

TEST_CASE("the tool runs beside the profile on the chosen side")
{
    SequenceStep step = ProfilingStep();
    std::vector<ToolpathMove> moves = Generate(step, OpenLine(10)).toolpath.moves;
    REQUIRE(moves.size() == 4);
    CHECK(At(moves[0].to, 0, 1));
    CHECK(moves[0].to.z == 10);
    CHECK(At(moves[1].to, 0, 1));
    CHECK(moves[1].to.z == 0);
    CHECK(At(moves[2].to, 10, 1));
    CHECK(moves[2].feed == 1000);
    CHECK(moves[3].to.z == 10);

    step.profiling.startSide = ProfileSide::Right;
    moves = Generate(step, OpenLine(10)).toolpath.moves;
    CHECK(At(moves[1].to, 0, -1));
    CHECK(At(moves[2].to, 10, -1));

    // Stock to leave moves it further out.
    step.profiling.stockToLeave = 0.5;
    moves = Generate(step, OpenLine(10)).toolpath.moves;
    CHECK(At(moves[2].to, 10, -1.5));
}

TEST_CASE("climb keeps the profile on the cutter's right, conventional on its left")
{
    SequenceStep step = ProfilingStep();
    step.profiling.direction = CutDirection::Climb;
    std::vector<ToolpathMove> moves = Generate(step, OpenLine(10)).toolpath.moves;
    CHECK(At(moves[1].to, 0, 1));       // on the left, going the profile's way
    CHECK(At(moves[2].to, 10, 1));

    step.profiling.startSide = ProfileSide::Right;
    moves = Generate(step, OpenLine(10)).toolpath.moves;
    CHECK(At(moves[1].to, 10, -1));     // on the right, going back
    CHECK(At(moves[2].to, 0, -1));

    step.profiling.direction = CutDirection::Conventional;
    moves = Generate(step, OpenLine(10)).toolpath.moves;
    CHECK(At(moves[1].to, 0, -1));

    // Climbing round the outside of an anticlockwise square goes clockwise.
    step = ProfilingStep();
    step.profiling.direction = CutDirection::Climb;
    step.profiling.useClosedSide = true;
    step.profiling.closedSide = ClosedProfileSide::Outside;
    ProfileChain square;
    square.ok = true;
    square.closed = true;
    square.path = Square();
    const Toolpath toolpath = Generate(step, square).toolpath;
    Polyline points;
    for (const ToolpathMove& move : toolpath.moves)
    {
        if (move.kind == MoveKind::Feed && move.feed == 1000)
            points.push_back(move.to);
    }
    CHECK(SignedArea(points) < 0);
}

TEST_CASE("a cut width steps in from the widest pass to the one beside the profile")
{
    SequenceStep step = ProfilingStep();
    step.profiling.cutWidth = 2;
    step.profiling.stepPerCut = 1;
    std::vector<double> ys;
    for (const ToolpathMove& move : Generate(step, OpenLine(10)).toolpath.moves)
    {
        if (move.kind == MoveKind::Feed && move.feed == 1000)
            ys.push_back(move.to.y);
    }
    REQUIRE(ys.size() == 3);
    CHECK(ys[0] == doctest::Approx(3));
    CHECK(ys[1] == doctest::Approx(2));
    CHECK(ys[2] == doctest::Approx(1));
}

TEST_CASE("tabs lift the tool over the tab length plus its diameter, on the levels below the tab top")
{
    // Levels at -0.4, -0.8 and -1.0. A 0.5 high tab's top is -0.5, so the lower two get it.
    SequenceStep step = ProfilingStep();
    step.cutLevels = CutLevelSettings();
    step.profiling.useTabs = true;
    step.profiling.tabCount = 1;
    step.profiling.tabLength = 4;
    step.profiling.tabHeight = 0.5;
    const std::vector<ToolpathMove> moves = Generate(step, OpenLine(100)).toolpath.moves;

    // Find the level at -0.8: in at x 0, along to 47, up, over to 53, down, on to 100.
    size_t i = 0;
    while (i < moves.size() && !(moves[i].kind == MoveKind::Feed && std::fabs(moves[i].to.z + 0.8) < 1e-9))
        ++i;
    REQUIRE(i + 5 < moves.size());
    CHECK(At(moves[i].to, 0, 1));
    CHECK(At(moves[i + 1].to, 47, 1));
    CHECK(At(moves[i + 2].to, 47, 1));
    CHECK(moves[i + 2].to.z == doctest::Approx(-0.5));
    CHECK(moves[i + 2].feed == 100);
    CHECK(At(moves[i + 3].to, 53, 1));
    CHECK(moves[i + 3].to.z == doctest::Approx(-0.5));
    CHECK(moves[i + 4].to.z == doctest::Approx(-0.8));
    CHECK(At(moves[i + 5].to, 100, 1));

    // The top level cuts straight through.
    size_t lifts = 0;
    for (const ToolpathMove& move : moves)
        lifts += (std::fabs(move.to.z + 0.5) < 1e-9) ? 1 : 0;
    CHECK(lifts == 4);      // up and over, on each of the two lower levels
}

TEST_CASE("a profiling toolpath needs a tool, a profile and room for the tool")
{
    SequenceStep step = ProfilingStep();
    ProfileToolpathInput input;
    input.operation = &step;
    input.profile = OpenLine(10);
    CHECK_FALSE(GenerateProfileToolpath(input).ok);     // no tool

    CHECK_FALSE(Generate(step, ProfileChain()).ok);

    step.profiling.useClosedSide = true;
    step.profiling.closedSide = ClosedProfileSide::Inside;
    ProfileChain slot;
    slot.ok = true;
    slot.closed = true;
    slot.path = PathFromPoints({ { 0, 0, 0 }, { 10, 0, 0 }, { 10, 1.5, 0 }, { 0, 1.5, 0 } });
    CHECK_FALSE(Generate(step, slot).ok);
}
