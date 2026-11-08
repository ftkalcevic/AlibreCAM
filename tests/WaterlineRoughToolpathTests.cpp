// WaterlineRoughToolpathTests.cpp : tests for Waterline Roughing's levels, contours and toolpath.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>

#include "CamJob.h"
#include "Stock.h"
#include "WaterlineRoughToolpath.h"

using namespace camcore;

namespace
{
    // The 12 triangles of a box, facing out.
    std::vector<Vec3> Box(const Vec3& a, const Vec3& b)
    {
        const Vec3 c[8] = {
            { a.x, a.y, a.z }, { b.x, a.y, a.z }, { b.x, b.y, a.z }, { a.x, b.y, a.z },
            { a.x, a.y, b.z }, { b.x, a.y, b.z }, { b.x, b.y, b.z }, { a.x, b.y, b.z },
        };
        const int faces[6][4] = { { 0, 3, 2, 1 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 },
                                  { 1, 2, 6, 5 }, { 2, 3, 7, 6 }, { 3, 0, 4, 7 } };
        std::vector<Vec3> mesh;
        for (const auto& f : faces)
        {
            for (int i : { f[0], f[1], f[2], f[0], f[2], f[3] })
                mesh.push_back(c[i]);
        }
        return mesh;
    }

    SequenceStep WaterlineStep(double stockToLeave = 0.5, double step = 2.0)
    {
        SequenceStep step_;
        step_.type = StepType::WaterlineRoughing;
        step_.clearance.mode = ClearanceMode::AbsoluteZ;
        step_.clearance.absoluteZ = 20;
        step_.feeds.plunge = 150;
        step_.feeds.cut = 1200;
        step_.waterlineRoughing.stockToLeave = stockToLeave;
        step_.waterlineRoughing.stepDown = step;
        return step_;
    }

    Tool Mill(double diameter)
    {
        Tool tool;
        tool.diameter = diameter;
        tool.overallLength = 30;
        return tool;
    }

    // A 40 x 30 x 10 stock, its footprint as the area, and a block in the middle.
    WaterlineRoughToolpathInput Input(const SequenceStep& step, const Tool& tool)
    {
        WaterlineRoughToolpathInput input;
        input.operation = &step;
        input.tool = &tool;
        ProfileChain footprint;
        footprint.ok = true;
        footprint.closed = true;
        footprint.path = StockFootprint(Frame(), { { 0, 0, 0 }, { 40, 30, 10 } });
        input.outlines = { footprint };
        input.partTriangles = Box({ 10, 8, 0 }, { 30, 22, 6 });
        input.haveStockMaxZ = true;
        input.stockMaxZ = 10;
        input.haveStockMinZ = true;
        input.stockMinZ = 0;
        input.havePartMaxZ = true;
        input.partMaxZ = 6;
        return input;
    }

    double DistanceToRectangle(double x, double y, double x0, double y0, double x1, double y1)
    {
        const double dx = (std::max)({ x0 - x, 0.0, x - x1 });
        const double dy = (std::max)({ y0 - y, 0.0, y - y1 });
        return std::hypot(dx, dy);
    }
}

TEST_CASE("flat faces facing up give the levels' marks")
{
    std::vector<Vec3> mesh = Box({ 0, 0, 0 }, { 10, 10, 4 });
    const std::vector<Vec3> step = Box({ 0, 0, 4 }, { 5, 10, 7 });
    mesh.insert(mesh.end(), step.begin(), step.end());
    const std::vector<double> heights = FlatHeights(mesh);
    REQUIRE(heights.size() == 2);       // 7 and 4; the bottoms face down
    CHECK(heights[0] == doctest::Approx(7));
    CHECK(heights[1] == doctest::Approx(4));
}

TEST_CASE("flat faces are found whichever way the triangles go round")
{
    // Like Frank's example: a 55 x 55 x 5 block (his has a half sphere sunk into
    // its top, which doesn't change the flat top's height).
    std::vector<Vec3> mesh = Box({ 0, 0, 0 }, { 55, 55, 5 });
    const std::vector<double> forward = FlatHeights(mesh);
    REQUIRE(forward.size() == 1);
    CHECK(forward[0] == doctest::Approx(5));

    // The same mesh with every triangle going the other way round.
    for (size_t t = 0; t + 2 < mesh.size(); t += 3)
        std::swap(mesh[t + 1], mesh[t + 2]);
    const std::vector<double> backward = FlatHeights(mesh);
    REQUIRE(backward.size() == 1);
    CHECK(backward[0] == doctest::Approx(5));

    // And its levels with 0.5 to leave and a step of 3, under a 6 mm stock: one at 5.5.
    const std::vector<double> levels = WaterlineLevels(6, 0, backward, 0.5, 3);
    REQUIRE(levels.size() >= 2);
    CHECK(levels[0] == doctest::Approx(5.5));
}

TEST_CASE("levels land on each flat face plus the stock to leave, no step deeper than asked")
{
    const std::vector<double> levels = WaterlineLevels(10, 0, { 6, 3 }, 0.5, 2);
    // 10 to 6.5 in two, 6.5 to 3.5 in two, 3.5 to 0 in two.
    const std::vector<double> expected = { 8.25, 6.5, 5.0, 3.5, 1.75, 0.0 };
    REQUIRE(levels.size() == expected.size());
    for (size_t i = 0; i < levels.size(); ++i)
        CHECK(levels[i] == doctest::Approx(expected[i]));

    // A flat face closer than the step still gets its level.
    const std::vector<double> close = WaterlineLevels(10, 0, { 9 }, 0, 5);
    CHECK(close.front() == doctest::Approx(9));
    CHECK(close.back() == doctest::Approx(0));

    CHECK(WaterlineLevels(0, 10, {}, 0, 1).empty());
}

TEST_CASE("contours go round with the higher values on their left")
{
    // A cone: values falling away from the middle.
    const int n = 41;
    std::vector<float> values(static_cast<size_t>(n) * n);
    for (int j = 0; j < n; ++j)
    {
        for (int i = 0; i < n; ++i)
            values[static_cast<size_t>(j) * n + i] = static_cast<float>(10.0 - std::hypot(i - 20.0, j - 20.0) * 0.5);
    }
    const std::vector<Polyline> loops = IsoContours(values, n, n, -20.5, -20.5, 1.0, 5.0, 3.0);
    REQUIRE(loops.size() == 1);
    double area = 0.0;
    for (size_t i = 0; i < loops[0].size(); ++i)
    {
        const Vec3& a = loops[0][i];
        const Vec3& b = loops[0][(i + 1) % loops[0].size()];
        area += a.x * b.y - b.x * a.y;
        CHECK(std::hypot(a.x, a.y) == doctest::Approx(10.0).epsilon(0.02));   // where 10 - r/2 = 5
        CHECK(a.z == 3.0);
    }
    CHECK(area > 0.0);      // anticlockwise: the higher middle on the left
}

TEST_CASE("each level clears round the part, leaving the stock to leave, and reaches the stock's open edges")
{
    const SequenceStep step = WaterlineStep();
    const Tool tool = Mill(6);
    const ToolpathResult result = GenerateWaterlineRoughToolpath(Input(step, tool));
    REQUIRE(result.ok);

    bool level65 = false, levelBottom = false, nearEdge = false;
    for (const ToolpathMove& move : result.toolpath.moves)
    {
        if (move.kind != MoveKind::Feed || move.feed != 1200)
            continue;
        const Vec3& p = move.to;
        level65 = level65 || std::fabs(p.z - 6.5) < 1e-9;
        levelBottom = levelBottom || std::fabs(p.z) < 1e-9;
        nearEdge = nearEdge || p.x < 0.5;

        // Inside the stock's footprint, the tool's centre at most at its edge.
        CHECK(p.x >= -0.2);
        CHECK(p.x <= 40.2);
        CHECK(p.y >= -0.2);
        CHECK(p.y <= 30.2);

        // Below the block's top plus the stock to leave, the tool keeps clear of its sides.
        if (p.z < 6.5 - 1e-9)
            CHECK(DistanceToRectangle(p.x, p.y, 10, 8, 30, 22) >= 3.0 + 0.5 - 0.2);
    }
    CHECK(level65);
    CHECK(levelBottom);
    CHECK(nearEdge);

    // Above the block it clears right across it.
    bool overBlock = false;
    for (const ToolpathMove& move : result.toolpath.moves)
    {
        if (move.kind == MoveKind::Feed && move.feed == 1200 && move.to.z > 6.5 + 1e-9)
            overBlock = overBlock || DistanceToRectangle(move.to.x, move.to.y, 12, 10, 28, 20) == 0.0;
    }
    CHECK(overBlock);

    // It starts and ends at the clearance plane.
    CHECK(result.toolpath.moves.front().to.z == doctest::Approx(20));
    CHECK(result.toolpath.moves.back().to.z == doctest::Approx(20));
}

TEST_CASE("a level on a flat face is cut even when the face's height rounds up as a float")
{
    // Frank's job: the area is a part's 55 x 55 shelf, with nothing beyond it, and
    // the shelf's height rounds up a little when stored as a float.
    const double shelf = 3.00000015;
    SequenceStep step = WaterlineStep(0.5, 3.0);
    step.clearance.absoluteZ = 40;
    const Tool tool = Mill(6);
    WaterlineRoughToolpathInput input;
    input.operation = &step;
    input.tool = &tool;
    input.faces = { { { 0, 0, shelf }, { 55, 0, shelf }, { 55, 55, shelf }, { 0, 0, shelf }, { 55, 55, shelf },
                      { 0, 55, shelf } } };
    input.partTriangles = Box({ 0, 0, 0 }, { 55, 55, shelf });
    input.haveStockMaxZ = true;
    input.stockMaxZ = 32.5;
    input.haveStockMinZ = true;
    input.stockMinZ = 0;
    input.havePartMaxZ = true;
    input.partMaxZ = shelf;
    const ToolpathResult result = GenerateWaterlineRoughToolpath(input);
    REQUIRE(result.ok);
    double lowest = 1e9;
    for (const ToolpathMove& move : result.toolpath.moves)
    {
        if (move.kind == MoveKind::Feed && move.feed == 1200)
            lowest = (std::min)(lowest, move.to.z);
    }
    CHECK(lowest == doctest::Approx(shelf + 0.5));      // the shelf's level is the lowest cut
}

namespace
{
    // The cutting moves' ends.
    std::vector<Vec3> Cuts(const Toolpath& toolpath)
    {
        std::vector<Vec3> points;
        for (const ToolpathMove& move : toolpath.moves)
        {
            if (move.kind == MoveKind::Feed && move.feed == 1200)
                points.push_back(move.to);
        }
        return points;
    }
}

TEST_CASE("the stepover by percent, distance or scallop, and the stepdown by percent, distance or levels")
{
    WaterlineRoughingSettings settings;
    CHECK(WaterlineStepover(settings, 6, 0) == doctest::Approx(3));
    settings.stepoverMode = WaterlineStepoverMode::Distance;
    settings.stepoverDistance = 2.5;
    CHECK(WaterlineStepover(settings, 6, 0) == doctest::Approx(2.5));
    settings.stepoverMode = WaterlineStepoverMode::Scallop;
    settings.scallop = 0.05;
    CHECK(WaterlineStepover(settings, 6, 3) == doctest::Approx(2.0 * std::sqrt(0.05 * 5.95)));   // a ball mill
    CHECK(WaterlineStepover(settings, 6, 1) == doctest::Approx(2.0 * std::sqrt(0.05 * 1.95)));   // a corner radius
    CHECK(WaterlineStepover(settings, 6, 0) == doctest::Approx(5.4));                            // flat: 90%

    CHECK(WaterlineStepdown(settings, 6, 10, 0) == doctest::Approx(2));
    settings.stepdownMode = WaterlineStepdownMode::Percent;
    settings.stepdownPercent = 25;
    CHECK(WaterlineStepdown(settings, 6, 10, 0) == doctest::Approx(1.5));
    settings.stepdownMode = WaterlineStepdownMode::Levels;
    settings.levels = 4;
    CHECK(WaterlineStepdown(settings, 6, 10, 0) == doctest::Approx(2.5));
}

TEST_CASE("every pattern and direction keeps clear of the part and inside the area")
{
    for (WaterlinePattern pattern : { WaterlinePattern::PartOffset, WaterlinePattern::StockOffset, WaterlinePattern::Linear })
    {
        for (CutDirection direction : { CutDirection::Climb, CutDirection::Conventional, CutDirection::Mixed })
        {
            for (WaterlineStart start : { WaterlineStart::Inside, WaterlineStart::Outside })
            {
                SequenceStep step = WaterlineStep();
                step.waterlineRoughing.pattern = pattern;
                step.waterlineRoughing.direction = direction;
                step.waterlineRoughing.start = start;
                step.waterlineRoughing.cutAngle = 30;
                const Tool tool = Mill(6);
                const ToolpathResult result = GenerateWaterlineRoughToolpath(Input(step, tool));
                INFO("pattern " << static_cast<int>(pattern) << ", direction " << static_cast<int>(direction)
                                << ", start " << static_cast<int>(start));
                REQUIRE(result.ok);
                const std::vector<Vec3> cuts = Cuts(result.toolpath);
                REQUIRE(cuts.size() > 20);
                bool atBottom = false;
                for (const Vec3& p : cuts)
                {
                    CHECK(p.x >= -0.2);
                    CHECK(p.x <= 40.2);
                    CHECK(p.y >= -0.2);
                    CHECK(p.y <= 30.2);
                    if (p.z < 6.5 - 1e-9)
                        CHECK(DistanceToRectangle(p.x, p.y, 10, 8, 30, 22) >= 3.5 - 0.2);
                    atBottom = atBottom || std::fabs(p.z) < 1e-9;
                }
                CHECK(atBottom);
            }
        }
    }
}

TEST_CASE("Clear Flats off sticks to the stepdown; Top and Bottom force the cut's range")
{
    SequenceStep step = WaterlineStep(0.5, 2.0);
    step.waterlineRoughing.clearFlats = false;
    const Tool tool = Mill(6);
    ToolpathResult result = GenerateWaterlineRoughToolpath(Input(step, tool));
    REQUIRE(result.ok);
    for (const Vec3& p : Cuts(result.toolpath))
    {
        CHECK(std::fabs(p.z - 6.5) > 1e-6);         // no level at the block's top
        const double steps = p.z / 2.0;
        CHECK(std::fabs(steps - std::round(steps)) < 1e-6);     // 8, 6, 4, 2, 0
    }

    step = WaterlineStep(0.5, 2.0);
    step.waterlineRoughing.useTop = true;
    step.waterlineRoughing.top = 8;
    step.waterlineRoughing.useBottom = true;
    step.waterlineRoughing.bottom = 4;
    result = GenerateWaterlineRoughToolpath(Input(step, tool));
    REQUIRE(result.ok);
    double lowest = 1e9, highest = -1e9;
    for (const Vec3& p : Cuts(result.toolpath))
    {
        lowest = (std::min)(lowest, p.z);
        highest = (std::max)(highest, p.z);
    }
    CHECK(lowest == doctest::Approx(4));
    CHECK(highest <= 8.0 + 1e-9);
    CHECK(highest > 4.0);
}

TEST_CASE("the linear extension takes the tool past the area's open edges")
{
    SequenceStep step = WaterlineStep();
    step.waterlineRoughing.extension = 2.0;
    const Tool tool = Mill(6);
    const ToolpathResult result = GenerateWaterlineRoughToolpath(Input(step, tool));
    REQUIRE(result.ok);
    double xLow = 1e9;
    for (const Vec3& p : Cuts(result.toolpath))
    {
        xLow = (std::min)(xLow, p.x);
        CHECK(p.x >= -2.3);
        if (p.z < 6.5 - 1e-9)
            CHECK(DistanceToRectangle(p.x, p.y, 10, 8, 30, 22) >= 3.5 - 0.2);
    }
    CHECK(xLow < -1.5);
}

TEST_CASE("depth first cuts each separate area to the bottom; level first goes level by level")
{
    // A full-height wall across the middle splits every level into two.
    const Tool tool = Mill(6);
    const auto rises = [&](CutLevelOrder order) {
        SequenceStep step = WaterlineStep(0.5, 2.0);
        step.waterlineRoughing.order = order;
        WaterlineRoughToolpathInput input = Input(step, tool);
        input.partTriangles = Box({ 18, 0, 0 }, { 22, 30, 10 });
        input.partMaxZ = 10;
        const ToolpathResult result = GenerateWaterlineRoughToolpath(input);
        REQUIRE(result.ok);
        int count = 0;
        double last = 1e9;
        for (const Vec3& p : Cuts(result.toolpath))
        {
            if (p.z > last + 1e-6)
                ++count;
            last = p.z;
        }
        return count;
    };
    CHECK(rises(CutLevelOrder::LevelFirst) == 0);
    CHECK(rises(CutLevelOrder::DepthFirst) == 1);   // once, from the first side's bottom to the second's top
}

TEST_CASE("every waterline setting saves, and older jobs keep their stepdown")
{
    SequenceStep waterline = WaterlineStep(0.75, 1.25);
    WaterlineRoughingSettings& w = waterline.waterlineRoughing;
    w.pattern = WaterlinePattern::Linear;
    w.direction = CutDirection::Mixed;
    w.start = WaterlineStart::Inside;
    w.linearStart = PocketLinearStart::Top;
    w.cutAngle = 45;
    w.stepoverMode = WaterlineStepoverMode::Scallop;
    w.stepoverPercent = 40;
    w.stepoverDistance = 2.5;
    w.scallop = 0.02;
    w.extension = 1.5;
    w.stepdownMode = WaterlineStepdownMode::Levels;
    w.stepdownPercent = 30;
    w.levels = 7;
    w.order = CutLevelOrder::LevelFirst;
    w.useTop = true;
    w.top = 9;
    w.useBottom = true;
    w.bottom = 1;
    w.clearFlats = false;

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "CAMForAlibre-waterline-all-test.cam";
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
    waterline.id = NewStableId();
    waterline.name = "Waterline Roughing 1";
    group.children.push_back(waterline);
    job.sequence.push_back(group);
    REQUIRE(SaveJob(job, path).ok);
    const LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    const SequenceStep& back = loaded.job.sequence[1].children[0];
    CHECK(SameSettings(waterline, back));
    const WaterlineRoughingSettings& b = back.waterlineRoughing;
    CHECK(b.pattern == WaterlinePattern::Linear);
    CHECK(b.direction == CutDirection::Mixed);
    CHECK(b.start == WaterlineStart::Inside);
    CHECK(b.linearStart == PocketLinearStart::Top);
    CHECK(b.stepoverMode == WaterlineStepoverMode::Scallop);
    CHECK(b.scallop == 0.02);
    CHECK(b.extension == 1.5);
    CHECK(b.stepdownMode == WaterlineStepdownMode::Levels);
    CHECK(b.levels == 7);
    CHECK(b.order == CutLevelOrder::LevelFirst);
    CHECK(b.useTop);
    CHECK(b.top == 9);
    CHECK(b.useBottom);
    CHECK(b.bottom == 1);
    CHECK_FALSE(b.clearFlats);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    // A job saved before the Cut Levels tab: just the stock to leave and stepDown.
    const std::filesystem::path oldPath = std::filesystem::temp_directory_path() / "CAMForAlibre-waterline-old-test.cam";
    {
        std::string text;
        {
            SequenceStep plain = WaterlineStep(0.5, 3.0);
            plain.id = NewStableId();
            plain.name = "Waterline Roughing 1";
            Job oldJob = job;
            oldJob.sequence[1].children = { plain };
            REQUIRE(SaveJob(oldJob, oldPath).ok);
        }
        LoadResult older = LoadJob(oldPath);
        REQUIRE(older.status == LoadStatus::Ok);
        CHECK(older.job.sequence[1].children[0].waterlineRoughing.stepDown == 3.0);
        CHECK(older.job.sequence[1].children[0].waterlineRoughing.stepdownMode == WaterlineStepdownMode::Distance);
    }
    std::filesystem::remove(oldPath, ignored);
}

TEST_CASE("waterline roughing refuses what it can't do, and saves its settings")
{
    SequenceStep step = WaterlineStep();
    const Tool tool = Mill(6);
    WaterlineRoughToolpathInput input = Input(step, tool);
    input.tool = nullptr;
    CHECK_FALSE(GenerateWaterlineRoughToolpath(input).ok);
    input = Input(step, tool);
    input.outlines.clear();
    CHECK_FALSE(GenerateWaterlineRoughToolpath(input).ok);
    step.waterlineRoughing.stepDown = 0;
    CHECK_FALSE(GenerateWaterlineRoughToolpath(Input(step, tool)).ok);
    WaterlineRoughingSettings bad;
    bad.stockToLeave = -1.0;
    CHECK_FALSE(ValidateWaterlineRoughing(bad).empty());

    CHECK(IsOperation(StepType::WaterlineRoughing));
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "CAMForAlibre-waterline-test.cam";
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
    SequenceStep waterline = WaterlineStep(0.75, 1.25);
    waterline.id = NewStableId();
    waterline.name = "Waterline Roughing 1";
    waterline.stockFootprint = true;
    group.children.push_back(waterline);
    job.sequence.push_back(group);
    REQUIRE(SaveJob(job, path).ok);
    const LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    const SequenceStep& back = loaded.job.sequence[1].children[0];
    CHECK(back.type == StepType::WaterlineRoughing);
    CHECK(back.waterlineRoughing.stockToLeave == 0.75);
    CHECK(back.waterlineRoughing.stepDown == 1.25);
    CHECK(back.stockFootprint);
    CHECK(SameSettings(waterline, back));
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

// Run by name (-tc="waterline timing*" --no-skip) to see how long a realistic job takes.
TEST_CASE("waterline timing on a 150 x 150 x 30 stock" * doctest::skip())
{
    SequenceStep step = WaterlineStep(0.5, 2.0);
    step.clearance.absoluteZ = 40;
    const Tool tool = Mill(6);
    WaterlineRoughToolpathInput input = Input(step, tool);
    ProfileChain footprint;
    footprint.ok = true;
    footprint.closed = true;
    footprint.path = StockFootprint(Frame(), { { 0, 0, 0 }, { 150, 150, 30 } });
    input.outlines = { footprint };
    input.partTriangles = Box({ 30, 30, 0 }, { 120, 120, 12 });
    const std::vector<Vec3> boss = Box({ 50, 50, 12 }, { 100, 90, 24 });
    input.partTriangles.insert(input.partTriangles.end(), boss.begin(), boss.end());
    input.stockMaxZ = 30;
    input.partMaxZ = 24;
    const auto t0 = std::chrono::steady_clock::now();
    const ToolpathResult result = GenerateWaterlineRoughToolpath(input);
    const auto t1 = std::chrono::steady_clock::now();
    REQUIRE(result.ok);
    MESSAGE("waterline: " << result.toolpath.moves.size() << " moves in "
            << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() << " ms");
}


TEST_CASE("waterline roughing comes down at rapid to the approach distance above the level above")
{
    for (double approach : { 2.0, 3.5 })
    {
        SequenceStep step = WaterlineStep(0.5, 2.0);
        step.waterlineRoughing.approachDistance = approach;
        const Tool tool = Mill(6);
        const ToolpathResult result = GenerateWaterlineRoughToolpath(Input(step, tool));
        REQUIRE(result.ok);
        const std::vector<ToolpathMove>& moves = result.toolpath.moves;
        REQUIRE(moves.size() > 3);

        // The first cut: clearance, then at rapid down to the stock's top plus the
        // approach distance, then the plunge feed to the first level.
        size_t plunge = 0;
        while (plunge < moves.size() && moves[plunge].kind != MoveKind::Feed)
            ++plunge;
        REQUIRE(plunge >= 1);
        REQUIRE(plunge < moves.size());
        CHECK(moves[plunge - 1].kind == MoveKind::Rapid);
        CHECK(moves[plunge - 1].to.z == doctest::Approx(10.0 + approach));
        CHECK(moves[plunge].feed == doctest::Approx(150.0));
        CHECK(moves[plunge].to.z < 10.0);

        // No rapid ever ends below the level above the one it enters: every rapid
        // straight down that's followed by a plunge stops at least the approach
        // distance over the plunge's level less one step.
        for (size_t m = 1; m < moves.size(); ++m)
        {
            if (moves[m].kind == MoveKind::Feed && moves[m].feed == doctest::Approx(150.0) &&
                moves[m - 1].kind == MoveKind::Rapid && moves[m - 1].to.z < 20.0 - 1e-9)
                CHECK(moves[m - 1].to.z >= moves[m].to.z + approach - 1e-6);
        }
    }
}
