// ChamferToolpathTests.cpp : tests for chamfer toolpaths.

#include <doctest/doctest.h>

#include <cmath>
#include <set>

#include "ProfileToolpath.h"

using namespace camcore;

namespace
{
    Path Line(Vec3 from, Vec3 to) { return PathFromPoints({ from, to }); }

    // The 10 by 10 square from the origin at height z, as a closed profile.
    ProfileChain Square(double z)
    {
        return ChainProfile({ { Line({ 0, 0, z }, { 10, 0, z }) },
                              { Line({ 10, 0, z }, { 10, 10, z }) },
                              { Line({ 10, 10, z }, { 0, 10, z }) },
                              { Line({ 0, 10, z }, { 0, 0, z }) } },
                            z);
    }

    // How far a point is outside the square, seen from above.
    double OutsideSquare(const Vec3& p)
    {
        const double dx = (std::max)({ 0.0 - p.x, 0.0, p.x - 10.0 });
        const double dy = (std::max)({ 0.0 - p.y, 0.0, p.y - 10.0 });
        return std::hypot(dx, dy);
    }

    Tool VMill(double includedAngle, double tipDiameter)
    {
        Tool tool;
        tool.type = ToolType::VMill;
        tool.diameter = 12;
        tool.includedAngle = includedAngle;
        tool.tipDiameter = tipDiameter;
        return tool;
    }

    SequenceStep ChamferStep(double width, double clearance, double maxDepth)
    {
        SequenceStep step;
        step.type = StepType::Chamfer;
        step.clearance.mode = ClearanceMode::AbsoluteZ;
        step.clearance.absoluteZ = 20;
        step.feeds.plunge = 100;
        step.feeds.cut = 1000;
        step.profiling.useClosedSide = true;
        step.profiling.closedSide = ClosedProfileSide::Outside;
        step.chamfer.width = width;
        step.chamfer.tipClearance = clearance;
        step.chamfer.maxDepthPerCut = maxDepth;
        return step;
    }

    ToolpathResult Generate(const SequenceStep& step, const Tool& tool)
    {
        ProfileToolpathInput input;
        input.operation = &step;
        input.tool = &tool;
        input.profiles = { Square(5) };
        return GenerateChamferToolpath(input);
    }

    // The heights of the feed moves, rounded to 0.001.
    std::set<long> FeedHeights(const Toolpath& toolpath)
    {
        std::set<long> heights;
        for (const ToolpathMove& move : toolpath.moves)
        {
            if (move.kind == MoveKind::Feed)
                heights.insert(std::lround(move.to.z * 1000));
        }
        return heights;
    }

    // The distances outside the square of the feed moves at a height, rounded to 0.001.
    std::set<long> DistancesAt(const Toolpath& toolpath, double z)
    {
        std::set<long> distances;
        for (const ToolpathMove& move : toolpath.moves)
        {
            if (move.kind == MoveKind::Feed && std::fabs(move.to.z - z) < 1e-6)
                distances.insert(std::lround(OutsideSquare(move.to) * 1000));
        }
        return distances;
    }
}

TEST_CASE("a chamfer's depth and the tool's offset come from the tool's angle and tip")
{
    ChamferSettings chamfer;
    chamfer.width = 1;
    chamfer.tipClearance = 0;
    CHECK(ChamferDepth(VMill(90, 0), chamfer) == doctest::Approx(1));
    CHECK(ChamferOffset(VMill(90, 0), chamfer, 0) == doctest::Approx(0));
    CHECK(ChamferDepth(VMill(60, 0), chamfer) == doctest::Approx(1 / std::tan(30 * 3.14159265358979 / 180)));

    // A 1 mm tip and the tip 0.5 below the chamfer: half the tip, plus 0.5 across at 45 degrees.
    chamfer.tipClearance = 0.5;
    CHECK(ChamferOffset(VMill(90, 1), chamfer, 0) == doctest::Approx(1.0));
    CHECK(ChamferOffset(VMill(90, 1), chamfer, 0.1) == doctest::Approx(1.1));
}

TEST_CASE("a chamfer's depth is cut in equal passes, the tip the tooltip clearance below each")
{
    // A 90 degree V-mill, width 1: 1 deep, in two passes of 0.5. With a clearance
    // of 0.2 the tip is 0.7, then 1.2, below the top at 5.
    const SequenceStep step = ChamferStep(1, 0.2, 0.5);
    const ToolpathResult result = Generate(step, VMill(90, 0));
    REQUIRE_MESSAGE(result.ok, result.problem);

    const std::set<long> heights = FeedHeights(result.toolpath);
    CHECK(heights.count(4300) == 1);
    CHECK(heights.count(3800) == 1);
    CHECK(heights.count(4600) == 0);        // the clearance isn't a pass of its own
    CHECK(*heights.begin() == 3800);        // nothing deeper

    // Every pass runs outside the square, 0.2 from its edge.
    for (double z : { 4.3, 3.8 })
    {
        const std::set<long> distances = DistancesAt(result.toolpath, z);
        REQUIRE_FALSE(distances.empty());
        CHECK(*distances.begin() == 200);
        CHECK(*distances.rbegin() == 200);
    }
}

TEST_CASE("a chamfer's stepover adds passes further out, at each depth")
{
    SequenceStep step = ChamferStep(1, 0.2, 2);     // one depth pass
    step.profiling.cutWidth = 1;
    step.profiling.stepPerCut = 0.5;
    const ToolpathResult result = Generate(step, VMill(90, 0));
    REQUIRE_MESSAGE(result.ok, result.problem);

    const std::set<long> distances = DistancesAt(result.toolpath, 3.8);
    CHECK(distances == std::set<long>{ 200, 700, 1200 });
}

TEST_CASE("a chamfer is refused without a V-mill or chamfer mill, or with bad settings")
{
    const SequenceStep step = ChamferStep(1, 0.2, 0.5);

    Tool flat;
    flat.type = ToolType::FlatEndMill;
    flat.diameter = 6;
    const ToolpathResult wrongTool = Generate(step, flat);
    CHECK_FALSE(wrongTool.ok);
    CHECK(wrongTool.problem.find("Flat end mill") != std::string::npos);

    CHECK_FALSE(Generate(ChamferStep(0, 0.2, 0.5), VMill(90, 0)).ok);
    CHECK_FALSE(Generate(ChamferStep(1, -1, 0.5), VMill(90, 0)).ok);
    CHECK_FALSE(Generate(step, VMill(180, 0)).ok);

    // A chamfer mill works the same way.
    Tool chamferMill = VMill(90, 1);
    chamferMill.type = ToolType::ChamferMill;
    CHECK(Generate(step, chamferMill).ok);
}
