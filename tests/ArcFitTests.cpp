// ArcFitTests.cpp : tests for fitting arcs to straight moves, and arc moves in each plane.

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "ArcFit.h"
#include "CamJob.h"
#include "PocketToolpath.h"
#include "ScriptedPost.h"
#include "Simulation.h"

using namespace camcore;

namespace
{
    const double kPi = 3.14159265358979323846;

    ToolpathMove Feed(const Vec3& to, double feed = 1000.0)
    {
        ToolpathMove move;
        move.kind = MoveKind::Feed;
        move.to = to;
        move.feed = feed;
        return move;
    }

    // A rapid to the start, then straight feeds round a circle in a plane.
    Toolpath CircleMoves(ArcPlane plane, double radius, double fromAngle, double toAngle, int segments,
                         double noise = 0.0, double w = 2.0)
    {
        Toolpath toolpath;
        for (int k = 0; k <= segments; ++k)
        {
            const double a = fromAngle + (toAngle - fromAngle) * k / segments;
            const double r = radius + ((k % 2) ? noise : -noise);
            const Vec3 p = FromPlane(plane, 10.0 + r * std::cos(a), 20.0 + r * std::sin(a), w);
            if (k == 0)
            {
                ToolpathMove rapid;
                rapid.to = p;
                toolpath.moves.push_back(rapid);
            }
            else
            {
                toolpath.moves.push_back(Feed(p));
            }
        }
        return toolpath;
    }

    // How far the fitted toolpath's arcs stray from the circle they came from.
    double WorstOffCircle(const Toolpath& toolpath, ArcPlane plane, double radius)
    {
        double worst = 0.0;
        Vec3 at = toolpath.moves.front().to;
        for (size_t m = 1; m < toolpath.moves.size(); ++m)
        {
            for (const Vec3& p : ArcMovePoints(at, toolpath.moves[m], 1.0 * kPi / 180.0))
            {
                double u, v, w;
                ToPlane(plane, p, u, v, w);
                worst = (std::max)(worst, std::fabs(std::hypot(u - 10.0, v - 20.0) - radius));
            }
            at = toolpath.moves[m].to;
        }
        return worst;
    }
}

TEST_CASE("a circle of straight moves becomes half turns of arc, ending where it did")
{
    const double r = 15.0;
    Toolpath toolpath = CircleMoves(ArcPlane::XY, r, 0.0, 2.0 * kPi * 0.999, 400);
    const Vec3 end = toolpath.moves.back().to;
    ArcFitting fitting;
    const ArcFitCounts counts = FitArcs(toolpath, fitting);
    CHECK(counts.linesReplaced == 400);
    CHECK(toolpath.moves.size() <= 1 + 2);  // the rapid, then two half turns
    for (size_t m = 1; m < toolpath.moves.size(); ++m)
    {
        CHECK(toolpath.moves[m].arc);
        CHECK(toolpath.moves[m].plane == ArcPlane::XY);
        CHECK_FALSE(toolpath.moves[m].clockwise);
    }
    CHECK(toolpath.moves.back().to.x == end.x);
    CHECK(toolpath.moves.back().to.y == end.y);
    CHECK(WorstOffCircle(toolpath, ArcPlane::XY, r) < 1e-6);
}

TEST_CASE("arcs are fitted in the ZX and YZ planes, and turned either way")
{
    for (ArcPlane plane : { ArcPlane::ZX, ArcPlane::YZ })
    {
        Toolpath toolpath = CircleMoves(plane, 8.0, kPi, 0.0, 120);     // clockwise, half a turn
        FitArcs(toolpath, ArcFitting());
        REQUIRE(toolpath.moves.size() == 2);
        CHECK(toolpath.moves[1].arc);
        CHECK(toolpath.moves[1].plane == plane);
        CHECK(toolpath.moves[1].clockwise);
        CHECK(std::fabs(ArcMoveSweep(toolpath.moves[0].to, toolpath.moves[1]) + kPi) < 1e-6);
        CHECK(WorstOffCircle(toolpath, plane, 8.0) < 1e-6);
    }
}

TEST_CASE("straight moves that don't follow a circle closely enough stay straight")
{
    ArcFitting fitting;

    // A zigzag.
    Toolpath zigzag;
    zigzag.moves.push_back(ToolpathMove());
    for (int k = 1; k <= 20; ++k)
        zigzag.moves.push_back(Feed({ k * 1.0, (k % 2) ? 1.0 : 0.0, 0.0 }));
    const size_t zigzagMoves = zigzag.moves.size();
    FitArcs(zigzag, fitting);
    CHECK(zigzag.moves.size() == zigzagMoves);

    // Noise over the tolerance.
    Toolpath noisy = CircleMoves(ArcPlane::XY, 10.0, 0.0, kPi, 60, 0.02);
    FitArcs(noisy, fitting);
    CHECK(noisy.moves.size() == 61);

    // Under it, though, they fit.
    Toolpath close = CircleMoves(ArcPlane::XY, 10.0, 0.0, kPi, 60, 0.002);
    FitArcs(close, fitting);
    CHECK(close.moves.size() < 10);

    // A helix in straight moves: not flat in any plane.
    Toolpath helix;
    helix.moves.push_back(ToolpathMove());
    for (int k = 0; k <= 60; ++k)
    {
        const double a = k * kPi / 30.0;
        helix.moves.push_back(Feed({ 5.0 * std::cos(a), 5.0 * std::sin(a), -0.05 * k }));
    }
    FitArcs(helix, fitting);
    CHECK(helix.moves.size() == 62);

    // A plane turned off, and arcs turned off.
    ArcFitting noZx = fitting;
    noZx.zx = false;
    Toolpath zx = CircleMoves(ArcPlane::ZX, 8.0, 0.0, kPi, 60);
    FitArcs(zx, noZx);
    CHECK(zx.moves.size() == 61);
    ArcFitting off = fitting;
    off.enabled = false;
    Toolpath xy = CircleMoves(ArcPlane::XY, 8.0, 0.0, kPi, 60);
    FitArcs(xy, off);
    CHECK(xy.moves.size() == 61);

    // Different feeds aren't joined.
    Toolpath feeds = CircleMoves(ArcPlane::XY, 8.0, 0.0, kPi, 60);
    for (size_t m = 1; m < feeds.moves.size(); ++m)
        feeds.moves[m].feed = (m % 2) ? 1000.0 : 500.0;
    FitArcs(feeds, fitting);
    CHECK(feeds.moves.size() == 61);
}

TEST_CASE("fitting a fitted toolpath again changes nothing, and lines either side are kept")
{
    Toolpath toolpath;
    toolpath.moves.push_back(ToolpathMove());
    toolpath.moves.push_back(Feed({ 0, 0, -1 }, 300.0));        // a plunge
    toolpath.moves.push_back(Feed({ 10, 0, -1 }));               // a line
    Toolpath arc = CircleMoves(ArcPlane::XY, 10.0, -kPi / 2.0, 0.0, 30, 0.0, -1.0);
    for (size_t m = 1; m < arc.moves.size(); ++m)
        toolpath.moves.push_back(Feed({ arc.moves[m].to.x - 10.0 + 10.0, arc.moves[m].to.y - 20.0 + 10.0, -1.0 }));
    toolpath.moves.push_back(Feed({ 30, 30, -1 }));
    FitArcs(toolpath, ArcFitting());
    const std::vector<ToolpathMove> once = toolpath.moves;
    size_t arcs = 0;
    for (const ToolpathMove& move : once)
        arcs += move.arc ? 1 : 0;
    CHECK(arcs >= 1);
    CHECK(once[1].to.z == -1.0);        // the plunge is still there
    FitArcs(toolpath, ArcFitting());
    REQUIRE(toolpath.moves.size() == once.size());
    for (size_t m = 0; m < once.size(); ++m)
    {
        CHECK(toolpath.moves[m].arc == once[m].arc);
        CHECK(toolpath.moves[m].to.x == once[m].to.x);
    }
}

TEST_CASE("arcs in each plane save and load")
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "camcore_arcplanes.cam";
    Job job;
    SequenceStep group;
    group.type = StepType::Group;
    group.id = NewStableId();
    group.name = "F1";
    SequenceStep op;
    op.type = StepType::ParallelFinishing;
    op.id = NewStableId();
    op.name = "P";
    op.toolpath = CircleMoves(ArcPlane::ZX, 5.0, 0.0, kPi, 30);
    Toolpath yz = CircleMoves(ArcPlane::YZ, 5.0, 0.0, kPi, 30);
    Toolpath xy = CircleMoves(ArcPlane::XY, 5.0, 0.0, kPi, 30);
    FitArcs(op.toolpath, ArcFitting());
    FitArcs(yz, ArcFitting());
    FitArcs(xy, ArcFitting());
    op.toolpath.moves.insert(op.toolpath.moves.end(), yz.moves.begin(), yz.moves.end());
    op.toolpath.moves.insert(op.toolpath.moves.end(), xy.moves.begin(), xy.moves.end());
    group.children.push_back(op);
    job.sequence.push_back(group);
    REQUIRE(SaveJob(job, path).ok);
    const LoadResult loaded = LoadJob(path);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    REQUIRE(loaded.status == LoadStatus::Ok);
    const std::vector<ToolpathMove>& back = loaded.job.sequence[0].children[0].toolpath.moves;
    REQUIRE(back.size() == op.toolpath.moves.size());
    for (size_t m = 0; m < back.size(); ++m)
    {
        CHECK(back[m].arc == op.toolpath.moves[m].arc);
        CHECK(back[m].plane == op.toolpath.moves[m].plane);
        CHECK(back[m].clockwise == op.toolpath.moves[m].clockwise);
    }
}

TEST_CASE("an ZX arc cuts the stock as its straight moves do")
{
    // A half circle in ZX, dipping 3 into a block, cut with a 2 mm ball: fitted or not, the same cells.
    Tool tool;
    tool.type = ToolType::BallMill;
    tool.diameter = 2.0;
    tool.fluteLength = 10.0;
    tool.overallLength = 30.0;
    Toolpath segments = CircleMoves(ArcPlane::ZX, 4.0, kPi, 2.0 * kPi, 90, 0.0, 5.0);   // x 16 to 20, z 6 to 14, at y 5
    Toolpath fitted = segments;
    FitArcs(fitted, ArcFitting());
    REQUIRE(fitted.moves.size() < segments.moves.size());
    long long counts[2] = { 0, 0 };
    for (int which = 0; which < 2; ++which)
    {
        SimOperation operation;
        operation.name = "Op";
        operation.profile = ProfileOf(tool);
        operation.toolpath = which == 0 ? segments : fitted;
        Simulation simulation;
        simulation.Start({ { 10, 0, 0 }, { 30, 10, 9 } }, {}, { operation }, SimResolution{ false, 0.1 });
        simulation.RunToEnd();
        counts[which] = simulation.Stock().Count();
    }
    CHECK(counts[0] < 20 * 10 * 9 * 1000);  // something was cut
    CHECK(std::llabs(counts[0] - counts[1]) < counts[0] / 500 + 200);
}

namespace
{
    std::string LinuxCnc()
    {
        const std::filesystem::path path =
            std::filesystem::path(__FILE__).parent_path().parent_path() / "posts" / "LinuxCNC.post";
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    // A clockwise half circle in ZX from (0, 0, 0) to (10, 0, 0) dipping to Z -5,
    // then an anticlockwise one in XY back to the start.
    SequenceStep ArcOperation()
    {
        SequenceStep step;
        step.type = StepType::ParallelFinishing;
        step.name = "Arcs";
        ToolpathMove start;
        start.to = { 0, 0, 0 };
        ToolpathMove zx = Feed({ 10, 0, 0 });
        zx.arc = true;
        zx.plane = ArcPlane::ZX;
        zx.centre = { 5, 0, 0 };
        zx.clockwise = true;
        ToolpathMove xy = Feed({ 0, 0, 0 });
        xy.arc = true;
        xy.centre = { 5, 0, 0 };
        step.toolpath.moves = { start, zx, xy };
        return step;
    }

    std::string Post(const SequenceStep& step, const Frame& frame, const ArcFitting& arcs)
    {
        const PostScriptRead read = ReadPostScript(LinuxCnc());
        REQUIRE(read.ok);
        static Tool tool;
        tool.id = "t";
        tool.diameter = 6;
        PostOperation entry;
        entry.frame = frame;
        entry.operation = &step;
        entry.tool = &tool;
        const PostOutput out = WriteScriptedPost(read.script, "Job", frame, { entry }, "", arcs);
        REQUIRE(out.ok);
        return out.text;
    }

    bool Has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
}

TEST_CASE("arcs post in their own planes, and as straight moves where they can't")
{
    const SequenceStep step = ArcOperation();
    // The ZX arc dips down: its point a quarter of the way round is at Z -5.
    const std::vector<Vec3> points = ArcMovePoints(step.toolpath.moves[0].to, step.toolpath.moves[1], kPi / 2.0);
    REQUIRE(points.size() == 2);
    CHECK(std::fabs(points[0].x - 5.0) < 1e-9);
    CHECK(std::fabs(points[0].z + 5.0) < 1e-9);

    const std::string g = Post(step, Frame(), ArcFitting());
    CHECK(Has(g, "G18\nG2 X10 I5 K0"));
    CHECK(Has(g, "G17\nG3 X0 I-5 J0"));

    // The origin turned a quarter about Z (the work zero is the work zero, not the
    // operation's origin): the ZX arc's plane is the work zero's YZ, seen from -X.
    Frame turned;
    turned.xAxis = { 0, 1, 0 };
    turned.yAxis = { -1, 0, 0 };
    {
        const PostScriptRead read = ReadPostScript(LinuxCnc());
        REQUIRE(read.ok);
        static Tool tool;
        tool.id = "t";
        PostOperation entry;
        entry.frame = turned;
        entry.operation = &step;
        entry.tool = &tool;
        const PostOutput out = WriteScriptedPost(read.script, "Job", Frame(), { entry }, "", ArcFitting());
        REQUIRE(out.ok);
        CHECK(Has(out.text, "G19\nG3 Y10 J5 K0"));
    }

    // Turned 30 degrees: no machine plane, so straight moves.
    Frame skew;
    skew.xAxis = { std::cos(kPi / 6.0), std::sin(kPi / 6.0), 0 };
    skew.yAxis = { -std::sin(kPi / 6.0), std::cos(kPi / 6.0), 0 };
    {
        const PostScriptRead read = ReadPostScript(LinuxCnc());
        static Tool tool;
        tool.id = "t";
        PostOperation entry;
        entry.frame = skew;
        entry.operation = &step;
        entry.tool = &tool;
        const PostOutput out = WriteScriptedPost(read.script, "Job", Frame(), { entry }, "", ArcFitting());
        REQUIRE(out.ok);
        CHECK_FALSE(Has(out.text, "G18"));
        CHECK_FALSE(Has(out.text, "G19"));
        CHECK(Has(out.text, "G3"));         // the XY arc is still an arc
    }

    // ZX turned off: the ZX arc in lines; arcs off: none at all.
    ArcFitting noZx;
    noZx.zx = false;
    const std::string lines = Post(step, Frame(), noZx);
    CHECK_FALSE(Has(lines, "G18"));
    CHECK_FALSE(Has(lines, "G2 "));
    CHECK(Has(lines, "G3"));
    ArcFitting off;
    off.enabled = false;
    const std::string none = Post(step, Frame(), off);
    CHECK_FALSE(Has(none, "G2 "));
    CHECK_FALSE(Has(none, "G3 "));
    CHECK(Has(none, "G1 "));
}

TEST_CASE("a real toolpath gets shorter with arcs, and stays within the tolerance of its straight moves")
{
    // Adaptive pocketing of a round pocket: curved fronts all over.
    SequenceStep step;
    step.type = StepType::Pocketing;
    step.clearance.mode = ClearanceMode::AbsoluteZ;
    step.clearance.absoluteZ = 10;
    step.cutLevels.totalDepth = 0;
    step.cutLevels.roughDepth = 0;
    step.cutLevels.finishDepth = 0;
    step.pocketing.adaptive = true;
    step.pocketing.stepover = { true, 15.0, 1.0 };
    Tool tool;
    tool.diameter = 6.0;
    tool.fluteLength = 20;
    tool.overallLength = 40;
    ProfileChain outline;
    outline.ok = true;
    outline.closed = true;
    Polyline round;
    for (int k = 0; k < 72; ++k)
        round.push_back({ 20.0 * std::cos(k * kPi / 36.0), 20.0 * std::sin(k * kPi / 36.0), 0.0 });
    outline.path = PathFromPoints(round);
    PocketToolpathInput input;
    input.operation = &step;
    input.tool = &tool;
    input.outlines = { outline };
    const ToolpathResult result = GeneratePocketToolpath(input);
    REQUIRE(result.ok);

    Toolpath fitted = result.toolpath;
    ArcFitting fitting;
    const ArcFitCounts counts = FitArcs(fitted, fitting);
    std::printf("arc fitting a pocket: %zu moves to %zu, %zu arcs\n", counts.movesBefore, counts.movesAfter, counts.arcsMade);
    CHECK(counts.arcsMade > 0);
    CHECK(counts.movesAfter < counts.movesBefore);

    // Every point along the fitted path is within the tolerance of the original path.
    std::vector<std::pair<Vec3, Vec3>> segments;
    for (size_t m = 1; m < result.toolpath.moves.size(); ++m)
    {
        Vec3 at = result.toolpath.moves[m - 1].to;
        for (const Vec3& p : MovePoints(at, result.toolpath.moves[m]))
        {
            segments.push_back({ at, p });
            at = p;
        }
    }
    const auto distance = [&](const Vec3& p) {
        double best = 1e18;
        for (const auto& s : segments)
        {
            const Vec3 d = s.second - s.first;
            const double length2 = Dot(d, d);
            const double t = length2 > 0.0 ? (std::max)(0.0, (std::min)(1.0, Dot(p - s.first, d) / length2)) : 0.0;
            best = (std::min)(best, Length(p - (s.first + d * t)));
        }
        return best;
    };
    double worst = 0.0;
    for (size_t m = 1; m < fitted.moves.size(); ++m)
    {
        if (!fitted.moves[m].arc)
            continue;
        for (const Vec3& p : ArcMovePoints(fitted.moves[m - 1].to, fitted.moves[m], 2.0 * kPi / 180.0))
            worst = (std::max)(worst, distance(p));
    }
    CHECK(worst <= fitting.tolerance + 1e-6);
}
