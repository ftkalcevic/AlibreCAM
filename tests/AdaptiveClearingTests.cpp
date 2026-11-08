// AdaptiveClearingTests.cpp : tests for adaptive clearing, checked by cutting the toolpath in the simulation.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <atomic>
#include <filesystem>

#include "AdaptiveClearing.h"
#include "CamJob.h"
#include "FacingToolpath.h"
#include "PocketToolpath.h"
#include "Simulation.h"
#include "Stock.h"
#include "WaterlineRoughToolpath.h"

using namespace camcore;

namespace
{
    // A grid over [x0, x1] x [y0, y1] with a margin, in cells of `cell`.
    AdaptiveGrid Grid(double x0, double y0, double x1, double y1, double margin, double cell)
    {
        AdaptiveGrid grid;
        grid.cell = cell;
        grid.xLow = x0 - margin;
        grid.yLow = y0 - margin;
        grid.nx = static_cast<int>(std::ceil((x1 - x0 + 2 * margin) / cell)) + 1;
        grid.ny = static_cast<int>(std::ceil((y1 - y0 + 2 * margin) / cell)) + 1;
        grid.free.assign(static_cast<size_t>(grid.nx) * grid.ny, 0);
        grid.material.assign(grid.free.size(), 0);
        return grid;
    }

    template <typename Free, typename Material>
    void Fill(AdaptiveGrid& grid, Free isFree, Material isMaterial)
    {
        for (int j = 0; j < grid.ny; ++j)
        {
            for (int i = 0; i < grid.nx; ++i)
            {
                const Vec3 c = grid.Centre(i, j);
                grid.free[grid.Index(i, j)] = isFree(c.x, c.y) ? 1 : 0;
                grid.material[grid.Index(i, j)] = isMaterial(c.x, c.y) ? 1 : 0;
            }
        }
    }

    Tool FlatTool(double diameter)
    {
        Tool tool;
        tool.diameter = diameter;
        tool.fluteLength = 20;
        tool.overallLength = 40;
        return tool;
    }

    AdaptiveWriting Writing(double z, double top)
    {
        AdaptiveWriting writing;
        writing.z = z;
        writing.top = top;
        writing.enter = top + 2.0;
        writing.clearance = top + 5.0;
        return writing;
    }

    // For looking at: ALIBRECAM_ADAPTIVE_DUMP set to a folder writes the moves there as text.
    void DumpToolpath(const Toolpath& toolpath, const char* name)
    {
        char* folder = nullptr;
        size_t length = 0;
        if (_dupenv_s(&folder, &length, "ALIBRECAM_ADAPTIVE_DUMP") != 0 || folder == nullptr)
            return;
        const std::string path = std::string(folder) + "/" + name + ".txt";
        std::free(folder);
        FILE* f = nullptr;
        if (fopen_s(&f, path.c_str(), "w") != 0 || f == nullptr)
            return;
        for (const ToolpathMove& move : toolpath.moves)
            std::fprintf(f, "%d %.4f %.4f %.4f %d\n", move.kind == MoveKind::Rapid ? 0 : 1, move.to.x, move.to.y, move.to.z, move.arc ? 1 : 0);
        std::fclose(f);
    }

    bool Dumping()
    {
        char* folder = nullptr;
        size_t length = 0;
        const bool set = _dupenv_s(&folder, &length, "ALIBRECAM_ADAPTIVE_DUMP") == 0 && folder != nullptr;
        std::free(folder);
        return set;
    }

    struct Engagement
    {
        double worst = 0.0;     // the widest cut over any 2 mm, mm
        double mean = 0.0;
    };

    // Cuts the toolpath in a box stock, returning the material left (mm^3) and
    // how wide the side cut was over each 2 mm of straight cutting.
    double CutAndMeasure(const Toolpath& toolpath, const StockBox& box, double toolDiameter, double depth,
                         Engagement& engagement)
    {
        SimOperation operation;
        operation.name = "Adaptive";
        operation.profile = ProfileOf(FlatTool(toolDiameter));
        operation.toolpath = toolpath;
        Simulation simulation;
        const double cell = 0.1;
        simulation.Start(box, {}, { operation }, SimResolution{ false, cell });
        const double cellVolume = cell * cell * cell;

        double windowVolume = 0.0, windowLength = 0.0, sumWidth = 0.0;
        int windows = 0;
        while (!simulation.Finished())
        {
            const size_t at = simulation.Position();
            const SimMove move = simulation.Moves()[at];
            const long long before = simulation.Stock().Count();
            simulation.ClearWarning();
            simulation.Step(1);
            const long long removed = before - simulation.Stock().Count();
            const ToolpathMove& source = toolpath.moves[move.toolpathMove];
            const double length = std::hypot(move.to.x - move.from.x, move.to.y - move.from.y);
            const bool level = std::fabs(move.to.z - move.from.z) < 1e-9;
            if (move.rapid || source.arc || !level || length < 1e-9)
            {
                windowVolume = windowLength = 0.0;
                continue;
            }
            windowVolume += removed * cellVolume;
            windowLength += length;
            if (windowLength >= 2.0)
            {
                const double width = windowVolume / (windowLength * depth);
                if (Dumping() && width > 1.3)
                    std::printf("  wide %.2f ending at (%.2f, %.2f), toolpath move %zu\n", width, move.to.x, move.to.y,
                                move.toolpathMove);
                engagement.worst = (std::max)(engagement.worst, width);
                sumWidth += width;
                ++windows;
                windowVolume = windowLength = 0.0;
            }
        }
        engagement.mean = windows > 0 ? sumWidth / windows : 0.0;
        return simulation.Stock().Count() * cellVolume;
    }
}

TEST_CASE("adaptive clearing empties a closed pocket at a steady load, helixing in")
{
    // A 40 x 30 pocket 5 deep, a 6 mm tool, 0.9 mm optimal load.
    const double r = 3.0;
    AdaptiveGrid grid = Grid(0, 0, 40, 30, 1.0, 0.15);
    Fill(
        grid, [&](double x, double y) { return x >= r && x <= 40 - r && y >= r && y <= 30 - r; },
        [&](double x, double y) { return x >= 0 && x <= 40 && y >= 0 && y <= 30; });
    AdaptiveSettings settings;
    settings.toolRadius = r;
    settings.optimalLoad = 0.9;
    const AdaptiveResult result = AdaptiveClear(grid, settings);
    REQUIRE_FALSE(result.cuts.empty());
    CHECK(result.helixes == 1);
    CHECK(result.plunges == 0);
    CHECK(result.cuts.front().entry == AdaptiveCut::Entry::Helix);

    Toolpath toolpath;
    WriteAdaptiveCuts(result.cuts, Writing(0.0, 5.0), toolpath);
    DumpToolpath(toolpath, "pocket");

    // The tool's centre stays where it may go.
    for (const ToolpathMove& move : toolpath.moves)
    {
        if (move.to.z < 5.0 - 1e-9)
        {
            CHECK(move.to.x >= r - 0.2);
            CHECK(move.to.x <= 40 - r + 0.2);
            CHECK(move.to.y >= r - 0.2);
            CHECK(move.to.y <= 30 - r + 0.2);
        }
    }

    Engagement engagement;
    const double left = CutAndMeasure(toolpath, { { 0, 0, 0 }, { 40, 30, 5 } }, 6.0, 5.0, engagement);
    // Only the corners the tool can't reach: 4 x (r^2 - pi r^2 / 4) x 5, about 39 mm^3.
    const double corners = 4.0 * (r * r - 3.14159265 * r * r / 4.0) * 5.0;
    std::printf("adaptive pocket: %zu cuts, %zu moves, left %.1f mm3 (corners %.1f), cut %.2f mm wide on average, at most %.2f\n",
                result.cuts.size(), toolpath.moves.size(), left, corners, engagement.mean, engagement.worst);
    CHECK(left < corners * 1.6 + 20.0);
    CHECK(engagement.worst < 0.9 * 1.8);
    CHECK(engagement.mean > 0.9 * 0.5);
}

TEST_CASE("adaptive clearing goes round an island, and starts from air when there is some")
{
    // A 60 x 40 area with a round island of radius 8 in the middle. Outside the
    // area is air: the tool can start there, like facing a stock from its edge.
    const double r = 3.0;
    const double ix = 30, iy = 20, ir = 8;
    AdaptiveGrid grid = Grid(0, 0, 60, 40, r + 2.0, 0.15);
    Fill(
        grid, [&](double x, double y) { return std::hypot(x - ix, y - iy) >= ir + r; },
        [&](double x, double y) {
            return x >= 0 && x <= 60 && y >= 0 && y <= 40 && std::hypot(x - ix, y - iy) >= ir;
        });
    AdaptiveSettings settings;
    settings.toolRadius = r;
    settings.optimalLoad = 1.2;
    const AdaptiveResult result = AdaptiveClear(grid, settings);
    REQUIRE_FALSE(result.cuts.empty());
    CHECK(result.helixes == 0);
    CHECK(result.plunges == 0);
    const Vec3 first = result.cuts.front().points.front();
    CHECK((first.x < 0 || first.x > 60 || first.y < 0 || first.y > 40));
    CHECK(result.leftArea < 5.0);

    Toolpath toolpath;
    WriteAdaptiveCuts(result.cuts, Writing(0.0, 5.0), toolpath);
    for (const ToolpathMove& move : toolpath.moves)
    {
        if (move.to.z < 5.0 - 1e-9)
            CHECK(std::hypot(move.to.x - ix, move.to.y - iy) >= ir + r - 0.25);
    }
    Engagement engagement;
    CutAndMeasure(toolpath, { { 0, 0, 0 }, { 60, 40, 5 } }, 6.0, 5.0, engagement);
    std::printf("adaptive island: %zu cuts, %zu moves, cut %.2f mm wide on average, at most %.2f\n", result.cuts.size(),
                toolpath.moves.size(), engagement.mean, engagement.worst);
    CHECK(engagement.worst < 1.2 * 1.8);
}

TEST_CASE("adaptive clearing plunges where there's no room for a helix")
{
    // A slot only just wider than the tool.
    const double r = 3.0;
    AdaptiveGrid grid = Grid(0, 0, 30, 6.4, 1.0, 0.1);
    Fill(
        grid, [&](double x, double y) { return x >= r && x <= 30 - r && y >= r && y <= 6.4 - r; },
        [&](double x, double y) { return x >= 0 && x <= 30 && y >= 0 && y <= 6.4; });
    AdaptiveSettings settings;
    settings.toolRadius = r;
    settings.optimalLoad = 0.6;
    const AdaptiveResult result = AdaptiveClear(grid, settings);
    REQUIRE_FALSE(result.cuts.empty());
    CHECK(result.helixes == 0);
    CHECK(result.plunges >= 1);
    CHECK(result.leftArea < 40.0);
}

TEST_CASE("ramp entry settings are checked")
{
    RampEntry ramp;
    CHECK(ValidateRampEntry(ramp).empty());
    ramp.rampAngle = 0.0;
    CHECK_FALSE(ValidateRampEntry(ramp).empty());
    ramp = RampEntry();
    ramp.helixPercent = -1.0;
    CHECK_FALSE(ValidateRampEntry(ramp).empty());
}

TEST_CASE("adaptive timing" * doctest::skip())
{
    const double r = 3.0;
    AdaptiveGrid grid = Grid(0, 0, 150, 150, 1.0, 0.15);
    Fill(
        grid, [&](double x, double y) { return x >= r && x <= 150 - r && y >= r && y <= 150 - r; },
        [&](double x, double y) { return x >= 0 && x <= 150 && y >= 0 && y <= 150; });
    AdaptiveSettings settings;
    settings.toolRadius = r;
    settings.optimalLoad = 0.9;
    const auto start = std::chrono::steady_clock::now();
    const AdaptiveResult result = AdaptiveClear(grid, settings);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    size_t points = 0;
    for (const AdaptiveCut& cut : result.cuts)
        points += cut.points.size();
    std::printf("adaptive 150 x 150: %zu cuts, %zu points, %.2f s\n", result.cuts.size(), points, seconds);
}

TEST_CASE("pocketing with adaptive clearing helixes in, clears the pocket, and laps the wall exactly")
{
    // A 40 x 30 pocket, 5 deep in two levels, a 6 mm tool at 15%.
    SequenceStep step;
    step.type = StepType::Pocketing;
    step.clearance.mode = ClearanceMode::AbsoluteZ;
    step.clearance.absoluteZ = 10;
    step.cutLevels.location = CutGeometryLocation::AtTop;  // the outline at the top, cut 5 down
    step.cutLevels.totalDepth = 5.0;
    step.cutLevels.roughDepth = 5.0;
    step.cutLevels.finishDepth = 0.0;
    step.cutLevels.roughStep = 2.5;
    step.pocketing.adaptive = true;
    step.pocketing.stepover = { true, 15.0, 1.0 };
    Tool tool = FlatTool(6.0);
    ProfileChain outline;
    outline.ok = true;
    outline.closed = true;
    outline.path = PathFromPoints({ { 0, 0, 5 }, { 40, 0, 5 }, { 40, 30, 5 }, { 0, 30, 5 } });
    PocketToolpathInput input;
    input.operation = &step;
    input.tool = &tool;
    input.outlines = { outline };
    const ToolpathResult result = GeneratePocketToolpath(input);
    REQUIRE(result.ok);

    size_t arcs = 0;
    double lowest = 1e9;
    for (const ToolpathMove& move : result.toolpath.moves)
    {
        arcs += move.arc ? 1 : 0;
        lowest = (std::min)(lowest, move.to.z);
        if (move.to.z < 5.0 - 1e-9)
        {
            // The tool's centre stays the radius inside the wall.
            CHECK(move.to.x >= 3.0 - 1e-6);
            CHECK(move.to.x <= 37.0 + 1e-6);
            CHECK(move.to.y >= 3.0 - 1e-6);
            CHECK(move.to.y <= 27.0 + 1e-6);
        }
    }
    CHECK(arcs > 0);                        // the helix
    CHECK(lowest == doctest::Approx(0.0));  // down to the outline

    Engagement engagement;
    const double left = CutAndMeasure(result.toolpath, { { 0, 0, 0 }, { 40, 30, 5 } }, 6.0, 2.5, engagement);
    const double corners = 4.0 * (9.0 - 3.14159265 * 9.0 / 4.0) * 5.0;
    std::printf("adaptive pocketing: %zu moves, left %.1f mm3 (corners %.1f), cut %.2f mm wide on average, at most %.2f\n",
                result.toolpath.moves.size(), left, corners, engagement.mean, engagement.worst);
    CHECK(left < corners * 1.5 + 10.0);
}

TEST_CASE("facing with adaptive clearing starts from outside the boundary when the overtravel allows")
{
    SequenceStep step;
    step.type = StepType::Facing;
    step.clearance.mode = ClearanceMode::AbsoluteZ;
    step.clearance.absoluteZ = 10;
    step.cutLevels.totalDepth = 0;
    step.cutLevels.roughDepth = 0;
    step.cutLevels.finishDepth = 0;
    step.facing.adaptive = true;
    step.facing.stepDistance = { true, 20.0, 1.0 };
    step.facing.overtravel = { true, 100.0, 6.0 };
    step.boundaries = { PathFromPoints({ { 0, 0, 0 }, { 50, 0, 0 }, { 50, 30, 0 }, { 0, 30, 0 } }) };
    Tool tool = FlatTool(6.0);
    FacingToolpathInput input;
    input.operation = &step;
    input.tool = &tool;
    const ToolpathResult result = GenerateFacingToolpath(input);
    REQUIRE(result.ok);
    size_t arcs = 0;
    bool firstDown = true;
    for (const ToolpathMove& move : result.toolpath.moves)
    {
        arcs += move.arc ? 1 : 0;
        if (firstDown && move.to.z < 1e-9)
        {
            firstDown = false;
            // The first time down is outside the boundary, clear of it.
            const bool outside = move.to.x < -3.0 || move.to.x > 53.0 || move.to.y < -3.0 || move.to.y > 33.0;
            CHECK(outside);
        }
    }
    CHECK(arcs == 0);
    Engagement engagement;
    const double left = CutAndMeasure(result.toolpath, { { 0, 0, 0 }, { 50, 30, 1 } }, 6.0, 1.0, engagement);
    std::printf("adaptive facing: %zu moves, left %.1f mm3, cut %.2f mm wide on average, at most %.2f\n",
                result.toolpath.moves.size(), left, engagement.mean, engagement.worst);
    CHECK(left < 1.0);
}

TEST_CASE("waterline roughing with adaptive clearing leaves the part alone")
{
    // A 40 x 30 x 10 stock, its footprint as the area, and a 20 x 14 x 6 block in the middle.
    SequenceStep step;
    step.type = StepType::WaterlineRoughing;
    step.clearance.mode = ClearanceMode::AbsoluteZ;
    step.clearance.absoluteZ = 20;
    step.waterlineRoughing.stockToLeave = 0.5;
    step.waterlineRoughing.stepDown = 2.0;
    step.waterlineRoughing.adaptive = true;
    step.waterlineRoughing.stepoverPercent = 15.0;
    step.waterlineRoughing.extension = 4.0;
    Tool tool = FlatTool(6.0);
    WaterlineRoughToolpathInput input;
    input.operation = &step;
    input.tool = &tool;
    ProfileChain footprint;
    footprint.ok = true;
    footprint.closed = true;
    footprint.path = StockFootprint(Frame(), { { 0, 0, 0 }, { 40, 30, 10 } });
    input.outlines = { footprint };
    // The block, as triangles.
    const Vec3 lo{ 10, 8, 0 }, hi{ 30, 22, 6 };
    const Vec3 c[8] = { { lo.x, lo.y, lo.z }, { hi.x, lo.y, lo.z }, { hi.x, hi.y, lo.z }, { lo.x, hi.y, lo.z },
                        { lo.x, lo.y, hi.z }, { hi.x, lo.y, hi.z }, { hi.x, hi.y, hi.z }, { lo.x, hi.y, hi.z } };
    const int faces[12][3] = { { 0, 2, 1 }, { 0, 3, 2 }, { 4, 5, 6 }, { 4, 6, 7 }, { 0, 1, 5 }, { 0, 5, 4 },
                               { 1, 2, 6 }, { 1, 6, 5 }, { 2, 3, 7 }, { 2, 7, 6 }, { 3, 0, 4 }, { 3, 4, 7 } };
    for (const auto& f : faces)
        for (int k : f)
            input.partTriangles.push_back(c[k]);
    input.haveStockMaxZ = true;
    input.stockMaxZ = 10;
    input.haveStockMinZ = true;
    input.stockMinZ = 0;
    input.havePartMaxZ = true;
    input.partMaxZ = 6;
    const ToolpathResult result = GenerateWaterlineRoughToolpath(input);
    REQUIRE(result.ok);
    for (const ToolpathMove& move : result.toolpath.moves)
    {
        if (move.kind != MoveKind::Feed || move.to.z >= 6.0)
            continue;
        const double dx = (std::max)({ lo.x - move.to.x, 0.0, move.to.x - hi.x });
        const double dy = (std::max)({ lo.y - move.to.y, 0.0, move.to.y - hi.y });
        CHECK(std::hypot(dx, dy) >= 3.0 + 0.5 - 0.3);   // the radius and the stock to leave, less a cell or two
    }
    std::printf("adaptive waterline: %zu moves\n", result.toolpath.moves.size());
}

TEST_CASE("adaptive clearing's settings save and load, and are off in older files")
{
    static std::atomic<int> counter{ 0 };
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                       ("camcore_adaptive_" + std::to_string(stamp) + "_" + std::to_string(counter++) + ".cam");
    Job job;
    SequenceStep group;
    group.type = StepType::Group;
    group.id = NewStableId();
    group.name = "F1";
    SequenceStep pocket;
    pocket.type = StepType::Pocketing;
    pocket.id = NewStableId();
    pocket.name = "Pocket";
    pocket.pocketing.adaptive = true;
    pocket.pocketing.rampEntry.helixPercent = 65.0;
    pocket.pocketing.rampEntry.rampAngle = 3.5;
    SequenceStep facing = pocket;
    facing.type = StepType::Facing;
    facing.id = NewStableId();
    facing.facing.adaptive = true;
    facing.facing.rampEntry.rampAngle = 4.0;
    SequenceStep rough = pocket;
    rough.type = StepType::WaterlineRoughing;
    rough.id = NewStableId();
    rough.waterlineRoughing.adaptive = true;
    rough.waterlineRoughing.rampEntry.helixPercent = 50.0;
    group.children = { pocket, facing, rough };
    job.sequence.push_back(group);
    REQUIRE(SaveJob(job, path).ok);
    const LoadResult loaded = LoadJob(path);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    REQUIRE(loaded.status == LoadStatus::Ok);
    const std::vector<SequenceStep>& back = loaded.job.sequence[0].children;
    REQUIRE(back.size() == 3);
    CHECK(back[0].pocketing.adaptive);
    CHECK(back[0].pocketing.rampEntry.helixPercent == 65.0);
    CHECK(back[0].pocketing.rampEntry.rampAngle == 3.5);
    CHECK(back[1].facing.adaptive);
    CHECK(back[1].facing.rampEntry.rampAngle == 4.0);
    CHECK(back[2].waterlineRoughing.adaptive);
    CHECK(back[2].waterlineRoughing.rampEntry.helixPercent == 50.0);

    // Settings without the fields: off, with the defaults.
    CHECK_FALSE(PocketingSettings().adaptive);
    CHECK(PocketingSettings().rampEntry.helixPercent == 80.0);
    CHECK(PocketingSettings().rampEntry.rampAngle == 2.0);
}

TEST_CASE("adaptive fronts round a helix are round")
{
    // A big square pocket: the rings near the helix shouldn't be polygons.
    const double r = 3.0;
    AdaptiveGrid grid = Grid(0, 0, 80, 80, 1.0, 0.15);
    Fill(
        grid, [&](double x, double y) { return x >= r && x <= 80 - r && y >= r && y <= 80 - r; },
        [&](double x, double y) { return x >= 0 && x <= 80 && y >= 0 && y <= 80; });
    AdaptiveSettings settings;
    settings.toolRadius = r;
    settings.optimalLoad = 1.0;
    settings.simplify = 0.001;
    const AdaptiveResult result = AdaptiveClear(grid, settings);
    REQUIRE(result.helixes == 1);
    const Vec3 centre = result.cuts.front().helixCentre;

    // Each closed ring's points: how far they wander from a circle. Within a cell (the
    // helix's bore, where they start, is in cells), however big the ring: 16-step
    // distances were about 2.6% of the radius out.
    double worst = 0.0;
    int rings = 0;
    for (const AdaptiveCut& cut : result.cuts)
    {
        if (cut.points.size() < 20)
            continue;
        const Vec3& a = cut.points.front();
        const Vec3& b = cut.points.back();
        if (std::hypot(a.x - b.x, a.y - b.y) > 0.5)
            continue;   // not a closed ring
        double low = 1e9, high = 0.0;
        for (const Vec3& p : cut.points)
        {
            const double d = std::hypot(p.x - centre.x, p.y - centre.y);
            low = (std::min)(low, d);
            high = (std::max)(high, d);
        }
        if (high > 30.0)
            continue;   // past where the walls bend it
        worst = (std::max)(worst, high - low);
        ++rings;
    }
    std::printf("adaptive rings: %d checked, out of round by at most %.3f mm\n", rings, worst);
    CHECK(rings >= 5);
    CHECK(worst < 0.15);
}
