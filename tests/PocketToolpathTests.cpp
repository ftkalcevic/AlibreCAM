// PocketToolpathTests.cpp : tests for pocketing toolpaths.

#include <doctest/doctest.h>

#include <set>

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
        input.outlines = { outline };
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

TEST_CASE("separate loops of edges are separate pockets, cut depth first or level first")
{
    // Two 20 mm squares, 10 apart, as loose edges in the list.
    auto line = [](Vec3 a, Vec3 b) { return PathFromPoints({ a, b }); };
    const std::vector<std::vector<Path>> edges = {
        { line({ 0, 0, 0 }, { 20, 0, 0 }) },   { line({ 30, 0, 0 }, { 50, 0, 0 }) },
        { line({ 20, 0, 0 }, { 20, 20, 0 }) }, { line({ 50, 0, 0 }, { 50, 20, 0 }) },
        { line({ 20, 20, 0 }, { 0, 20, 0 }) }, { line({ 50, 20, 0 }, { 30, 20, 0 }) },
        { line({ 0, 20, 0 }, { 0, 0, 0 }) },   { line({ 30, 20, 0 }, { 30, 0, 0 }) },
    };
    const ProfileChains chains = ChainProfiles(edges, 0);
    REQUIRE(chains.ok);
    REQUIRE(chains.chains.size() == 2);
    CHECK(chains.chains[0].closed);
    CHECK(chains.chains[1].closed);
    CHECK(chains.chains[0].path.front().point.x < 25);      // the first feature's loop comes first

    SequenceStep step = PocketStep(PocketCutType::Offset);
    step.cutLevels = CutLevelSettings();        // levels at -0.4, -0.8 and -1.0
    static Tool tool;
    tool.diameter = 4;
    PocketToolpathInput input;
    input.operation = &step;
    input.tool = &tool;
    input.outlines = chains.chains;

    // Depth first: every level of the left pocket before the right one is touched.
    auto order = [&]() {
        std::vector<std::pair<bool, double>> visits;     // right pocket, level
        for (const ToolpathMove& move : GeneratePocketToolpath(input).toolpath.moves)
        {
            if (move.kind != MoveKind::Feed || move.to.z > 0)
                continue;
            const std::pair<bool, double> visit{ move.to.x > 25, move.to.z };
            if (visits.empty() || visits.back() != visit)
                visits.push_back(visit);
        }
        return visits;
    };
    std::vector<std::pair<bool, double>> visits = order();
    REQUIRE(visits.size() >= 6);
    CHECK_FALSE(visits[0].first);
    CHECK_FALSE(visits[2].first);
    CHECK(visits[2].second == doctest::Approx(-1.0));
    CHECK(visits[3].first);

    // Level first: both pockets at the top level before going down.
    step.cutLevels.order = CutLevelOrder::LevelFirst;
    visits = order();
    REQUIRE(visits.size() >= 6);
    CHECK_FALSE(visits[0].first);
    CHECK(visits[1].first);
    CHECK(visits[1].second == doctest::Approx(-0.4));
    CHECK(visits[2].second == doctest::Approx(-0.8));

    // Every point stays inside one pocket's wall or the other's.
    for (const ToolpathMove& move : GeneratePocketToolpath(input).toolpath.moves)
    {
        if (move.kind != MoveKind::Feed || move.to.z > 0)
            continue;
        const double x = move.to.x > 25 ? move.to.x - 30 : move.to.x;
        CHECK(x >= 2 - 1e-6);
        CHECK(x <= 18 + 1e-6);
    }
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

TEST_CASE("a Helix entry goes down a helix inside the wall, at every level, no steeper than its angle")
{
    for (PocketStart start : { PocketStart::Inside, PocketStart::Outside })
    {
        SequenceStep step = PocketStep(PocketCutType::Offset);
        step.pocketing.start = start;
        step.cutLevels = CutLevelSettings();        // levels at -0.4, -0.8 and -1.0
        step.entryExit.entry = EntryMode::Helix;
        step.entryExit.alongPathAngle = 3.0;
        step.entryExit.alongPathHeight = 1.0;
        step.feeds.engage = 400;
        const ToolpathResult result = Generate(step);
        REQUIRE(result.ok);
        const std::vector<ToolpathMove>& moves = result.toolpath.moves;
        size_t arcs = 0;
        std::set<double> levels;
        const double steepest = std::tan(3.0 * 3.14159265358979323846 / 180.0) + 1e-6;
        for (size_t m = 1; m < moves.size(); ++m)
        {
            const ToolpathMove& move = moves[m];
            if (move.kind != MoveKind::Feed || move.feed != 400.0 || !move.arc)
                continue;
            ++arcs;
            levels.insert(std::round(move.to.z * 1000.0) / 1000.0);
            const Vec3& from = moves[m - 1].to;
            const double run = std::fabs(ArcMoveSweep(from, move)) *
                               std::hypot(from.x - move.centre.x, from.y - move.centre.y);
            CHECK(from.z - move.to.z <= run * steepest);
            // The helix's circle stays the wall's distance inside the 20 x 20 pocket (tool 4: 2 in).
            CHECK(move.to.x >= 2.0 - 1e-6);
            CHECK(move.to.x <= 18.0 + 1e-6);
            CHECK(move.to.y >= 2.0 - 1e-6);
            CHECK(move.to.y <= 18.0 + 1e-6);
        }
        if (start == PocketStart::Outside)
            continue;   // it starts at the wall loop's corner: no circle through it fits, so straight down
        CHECK(arcs > 0);
        CHECK(levels.count(-1.0) == 1);     // down to the last level too
    }
}

TEST_CASE("pocketing rest machining cuts only what the bigger tool left")
{
    // A 30 x 30 pocket 5 deep in a 40 x 40 x 10 stock: a 10 mm tool leaves round
    // corners, which a 4 mm tool then takes.
    ProfileChain outline;
    outline.ok = true;
    outline.closed = true;
    outline.path = PathFromPoints({ { 5, 5, 5 }, { 35, 5, 5 }, { 35, 35, 5 }, { 5, 35, 5 } });

    SequenceStep big = PocketStep(PocketCutType::Offset);
    big.clearance.absoluteZ = 20;
    big.feeds.engage = big.feeds.approach = big.feeds.retract = big.feeds.departure = 500;
    big.pocketing.stepover.percent = 40.0;
    Tool bigTool;
    bigTool.diameter = 10;
    bigTool.fluteLength = 20;
    bigTool.overallLength = 40;
    Tool smallTool = bigTool;
    smallTool.diameter = 4;

    PocketToolpathInput first;
    first.operation = &big;
    first.tool = &bigTool;
    first.outlines = { outline };
    const ToolpathResult roughed = GeneratePocketToolpath(first);
    REQUIRE(roughed.ok);

    SimOperation bigOp;
    bigOp.name = "Big";
    bigOp.profile = ProfileOf(bigTool);
    bigOp.toolpath = roughed.toolpath;
    const StockBox box{ { 0, 0, 0 }, { 40, 40, 10 } };
    Simulation simulation;
    simulation.Start(box, {}, { bigOp }, SimResolution{ true, 0.25 });
    simulation.RunToEnd();
    REQUIRE(simulation.Finished());
    const RemainingStock remaining = RemainingStockHeights(simulation.Stock(), Frame());

    SequenceStep small = big;
    small.pocketing.stepover.percent = 40.0;
    small.pocketing.restMachining = true;
    PocketToolpathInput second = first;
    second.operation = &small;
    second.tool = &smallTool;
    second.remainingStock = &remaining;

    const auto finishedLike = [&](const ToolpathResult& cut) {
        SequenceStep all = small;
        all.pocketing.restMachining = false;
        all.pocketing.adaptive = false;
        PocketToolpathInput whole = second;
        whole.operation = &all;
        whole.remainingStock = nullptr;
        const ToolpathResult everything = GeneratePocketToolpath(whole);
        REQUIRE(everything.ok);

        SimOperation smallOp;
        smallOp.name = "Small";
        smallOp.profile = ProfileOf(smallTool);
        smallOp.toolpath = cut.toolpath;
        SimOperation allOp = smallOp;
        allOp.toolpath = everything.toolpath;
        Simulation both, single;
        both.Start(box, {}, { bigOp, smallOp }, SimResolution{ true, 0.25 });
        while (!both.Finished())
        {
            both.RunToEnd();
            both.ClearWarning();
        }
        single.Start(box, {}, { allOp }, SimResolution{ true, 0.25 });
        single.RunToEnd();
        const double a = static_cast<double>(both.Stock().Count());
        const double b = static_cast<double>(single.Stock().Count());
        MESSAGE("cells left: rest ", a, ", whole ", b);
        CHECK(std::fabs(a - b) < b * 0.005);
    };

    const auto staysInCorners = [&](const ToolpathResult& cut) {
        int rapids = 0;
        for (const ToolpathMove& move : cut.toolpath.moves)
        {
            rapids += (move.kind == MoveKind::Rapid && move.to.z > 19.0) ? 1 : 0;
            if (move.kind == MoveKind::Feed && move.to.z < 5.5)
                CHECK((std::fabs(move.to.x - 20.0) > 9.0 && std::fabs(move.to.y - 20.0) > 9.0));
        }
        MESSAGE("rapids at the clearance plane: ", rapids, " in ", cut.toolpath.moves.size(), " moves");
    };

    SUBCASE("offset cuts")
    {
        const ToolpathResult cut = GeneratePocketToolpath(second);
        REQUIRE(cut.ok);
        staysInCorners(cut);
        finishedLike(cut);
    }

    SUBCASE("linear cuts")
    {
        small.pocketing.cutType = PocketCutType::Linear;
        const ToolpathResult cut = GeneratePocketToolpath(second);
        REQUIRE(cut.ok);
        staysInCorners(cut);
        finishedLike(cut);
    }

    SUBCASE("adaptive clearing")
    {
        small.pocketing.adaptive = true;
        const ToolpathResult cut = GeneratePocketToolpath(second);
        REQUIRE(cut.ok);
        staysInCorners(cut);
        finishedLike(cut);
    }

    SUBCASE("nothing left says so, and nothing given is refused")
    {
        small.pocketing.restMinimum = 20.0;
        CHECK_FALSE(GeneratePocketToolpath(second).ok);
        second.remainingStock = nullptr;
        CHECK_FALSE(GeneratePocketToolpath(second).ok);
    }
}
