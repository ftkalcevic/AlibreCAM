// WaterlineFinishToolpathTests.cpp : tests for Waterline Finishing's laps and levels.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

#include "CamJob.h"
#include "Stock.h"
#include "WaterlineRoughToolpath.h"

using namespace camcore;

namespace
{
    const double kPi = 3.14159265358979323846;

    // A half sphere of radius r on z = 0 at (cx, cy), facets outward.
    std::vector<Vec3> HalfSphere(double cx, double cy, double r, int around = 96, int up = 48)
    {
        std::vector<Vec3> mesh;
        const auto at = [&](int a, int u) {
            const double theta = 2.0 * kPi * a / around;
            const double phi = 0.5 * kPi * u / up;
            return Vec3{ cx + r * std::cos(phi) * std::cos(theta), cy + r * std::cos(phi) * std::sin(theta),
                         r * std::sin(phi) };
        };
        for (int u = 0; u < up; ++u)
        {
            for (int a = 0; a < around; ++a)
            {
                const Vec3 p00 = at(a, u), p10 = at(a + 1, u), p01 = at(a, u + 1), p11 = at(a + 1, u + 1);
                mesh.insert(mesh.end(), { p00, p10, p11 });
                if (u + 1 < up)
                    mesh.insert(mesh.end(), { p00, p11, p01 });
            }
        }
        return mesh;
    }

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

    SequenceStep FinishStep()
    {
        SequenceStep step;
        step.type = StepType::WaterlineFinishing;
        step.clearance.mode = ClearanceMode::AbsoluteZ;
        step.clearance.absoluteZ = 30;
        step.feeds.plunge = 150;
        step.feeds.cut = 1200;
        step.waterlineFinishing.stepDown = 1.0;
        step.waterlineFinishing.stepoverMode = WaterlineStepoverMode::Distance;
        step.waterlineFinishing.stepoverDistance = 100.0;   // no extra levels unless asked
        step.waterlineFinishing.clearFlats = false;
        return step;
    }

    Tool Ball(double diameter)
    {
        Tool tool;
        tool.type = ToolType::BallMill;
        tool.diameter = diameter;
        tool.overallLength = 40;
        return tool;
    }

    // A 60 x 60 x 20 stock, its footprint the area, and a half sphere of radius 15 in its middle.
    WaterlineRoughToolpathInput Input(const SequenceStep& step, const Tool& tool)
    {
        WaterlineRoughToolpathInput input;
        input.operation = &step;
        input.tool = &tool;
        ProfileChain footprint;
        footprint.ok = true;
        footprint.closed = true;
        footprint.path = StockFootprint(Frame(), { { 0, 0, 0 }, { 60, 60, 20 } });
        input.outlines = { footprint };
        input.partTriangles = HalfSphere(30, 30, 15);
        input.haveStockMaxZ = true;
        input.stockMaxZ = 20;
        input.haveStockMinZ = true;
        input.stockMinZ = 0;
        input.havePartMaxZ = true;
        input.partMaxZ = 15;
        return input;
    }

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

TEST_CASE("a ball nose's laps follow a half sphere at the ball's true distance")
{
    const SequenceStep step = FinishStep();
    const Tool tool = Ball(6);
    const ToolpathResult result = GenerateWaterlineFinishToolpath(Input(step, tool));
    REQUIRE(result.ok);
    const std::vector<Vec3> cuts = Cuts(result.toolpath);
    REQUIRE(cuts.size() > 50);
    std::set<long long> levels;
    for (const Vec3& p : cuts)
    {
        // The ball's centre is 3 above the tip, and 15 + 3 from the sphere's centre.
        const double expected = std::sqrt((std::max)(0.0, 18.0 * 18.0 - (p.z + 3.0) * (p.z + 3.0)));
        // Exact on the triangles: within the facets' sag (0.008) and the 0.01
        // simplifying, both across the surface, so more sideways where it's flatter
        // (the normal's sideways part is expected / 18).
        if (expected > 1.0)
            CHECK(std::fabs(std::hypot(p.x - 30, p.y - 30) - expected) < 0.025 * 18.0 / expected);
        levels.insert(std::llround(p.z * 1000));
    }
    CHECK(levels.size() >= 10);     // a lap at each level the sphere reaches
}

TEST_CASE("with the sphere known exactly, a ball's laps are on the true surface despite coarse facets")
{
    const SequenceStep step = FinishStep();
    const Tool tool = Ball(6);
    WaterlineRoughToolpathInput input = Input(step, tool);
    input.partTriangles = HalfSphere(30, 30, 15, 24, 12);   // facets up to 0.13 inside
    const auto worst = [&](const ToolpathResult& result) {
        double error = 0.0;
        for (const Vec3& p : Cuts(result.toolpath))
        {
            const double expected = std::sqrt((std::max)(0.0, 18.0 * 18.0 - (p.z + 3.0) * (p.z + 3.0)));
            // Measured across the surface (the normal's sideways part is expected / 18).
            if (expected > 1.0)
                error = (std::max)(error, std::fabs(std::hypot(p.x - 30, p.y - 30) - expected) * expected / 18.0);
        }
        return error;
    };
    const ToolpathResult facets = GenerateWaterlineFinishToolpath(input);
    REQUIRE(facets.ok);
    CHECK(worst(facets) > 0.05);

    input.partTriangleFaces.assign(input.partTriangles.size() / 3, 0);
    AnalyticSurface sphere;
    sphere.kind = SurfaceKind::Sphere;
    sphere.centre = { 30, 30, 0 };
    sphere.radius = 15;
    sphere.onFaceTolerance = 0.2;   // these facets are up to 0.13 from the sphere
    input.faceSurfaces = { sphere };
    const ToolpathResult exact = GenerateWaterlineFinishToolpath(input);
    REQUIRE(exact.ok);
    CHECK(exact.refinedPoints > 100);
    CHECK(exact.refineCalls == 0);
    CHECK(worst(exact) < 0.005);
    MESSAGE("laps: facets off by " << worst(facets) << ", exact off by " << worst(exact));
}

TEST_CASE("climb goes round the part clockwise, conventional anticlockwise")
{
    for (CutDirection direction : { CutDirection::Climb, CutDirection::Conventional })
    {
        SequenceStep step = FinishStep();
        step.waterlineFinishing.direction = direction;
        const Tool tool = Ball(6);
        const ToolpathResult result = GenerateWaterlineFinishToolpath(Input(step, tool));
        REQUIRE(result.ok);
        // The turn of the first level's lap.
        const std::vector<Vec3> cuts = Cuts(result.toolpath);
        REQUIRE(cuts.size() > 3);
        double area = 0.0;
        const double z = cuts.front().z;
        for (size_t i = 0; i + 1 < cuts.size() && std::fabs(cuts[i + 1].z - z) < 1e-9; ++i)
            area += (cuts[i].x - 30) * (cuts[i + 1].y - 30) - (cuts[i + 1].x - 30) * (cuts[i].y - 30);
        if (direction == CutDirection::Climb)
            CHECK(area < 0.0);
        else
            CHECK(area > 0.0);
    }
}

TEST_CASE("a small stepover adds levels where the slope is shallow")
{
    SequenceStep step = FinishStep();
    step.waterlineFinishing.optimiseStepover = true;
    const Tool tool = Ball(6);
    const auto levelCount = [&](double stepover) {
        step.waterlineFinishing.stepoverDistance = stepover;
        const ToolpathResult result = GenerateWaterlineFinishToolpath(Input(step, tool));
        REQUIRE(result.ok);
        std::set<long long> levels;
        for (const Vec3& p : Cuts(result.toolpath))
            levels.insert(std::llround(p.z * 1000));
        return levels.size();
    };
    const size_t plain = levelCount(100.0);
    const size_t fine = levelCount(1.0);
    CHECK(fine > plain);
}

TEST_CASE("a scallop stepdown needs a ball nose, and sets the steps from the ball's radius")
{
    SequenceStep step = FinishStep();
    step.waterlineFinishing.stepdownMode = WaterlineStepdownMode::Scallop;
    step.waterlineFinishing.stepdownScallop = 0.1;
    const Tool ball = Ball(6);
    CHECK(WaterlineStepdown(step.waterlineFinishing, 6, 20, 0, 3) == doctest::Approx(2.0 * std::sqrt(0.1 * 5.9)));
    CHECK(GenerateWaterlineFinishToolpath(Input(step, ball)).ok);

    Tool flat;
    flat.diameter = 6;
    flat.overallLength = 40;
    CHECK_FALSE(GenerateWaterlineFinishToolpath(Input(step, flat)).ok);
}

TEST_CASE("depth first finishes each part down to the bottom before the next")
{
    const Tool tool = Ball(6);
    const auto rises = [&](CutLevelOrder order) {
        SequenceStep step = FinishStep();
        step.waterlineFinishing.order = order;
        WaterlineRoughToolpathInput input = Input(step, tool);
        input.partTriangles = Box({ 8, 20, 0 }, { 20, 40, 12 });
        const std::vector<Vec3> second = Box({ 40, 20, 0 }, { 52, 40, 12 });
        input.partTriangles.insert(input.partTriangles.end(), second.begin(), second.end());
        input.partMaxZ = 12;
        const ToolpathResult result = GenerateWaterlineFinishToolpath(input);
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
    CHECK(rises(CutLevelOrder::DepthFirst) == 1);
}

TEST_CASE("a waterline finishing operation saves its settings, with its own defaults")
{
    CHECK(IsOperation(StepType::WaterlineFinishing));
    SequenceStep fresh;
    CHECK(fresh.waterlineFinishing.stockToLeave == 0.0);

    SequenceStep finishing = FinishStep();
    finishing.waterlineFinishing.stockToLeave = 0.1;
    finishing.waterlineFinishing.direction = CutDirection::Mixed;
    finishing.waterlineFinishing.stepdownMode = WaterlineStepdownMode::Scallop;
    finishing.waterlineFinishing.stepdownScallop = 0.02;
    finishing.waterlineFinishing.finishFlats = true;

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "CAMForAlibre-finish-test.cam";
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
    finishing.id = NewStableId();
    finishing.name = "Waterline Finishing 1";
    group.children.push_back(finishing);
    job.sequence.push_back(group);
    REQUIRE(SaveJob(job, path).ok);
    const LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    const SequenceStep& back = loaded.job.sequence[1].children[0];
    CHECK(back.type == StepType::WaterlineFinishing);
    CHECK(SameSettings(finishing, back));
    CHECK(back.waterlineFinishing.stockToLeave == 0.1);
    CHECK(back.waterlineFinishing.direction == CutDirection::Mixed);
    CHECK(back.waterlineFinishing.stepdownMode == WaterlineStepdownMode::Scallop);
    CHECK(back.waterlineFinishing.stepdownScallop == 0.02);
    CHECK(back.waterlineFinishing.finishFlats);
    SequenceStep unticked = back;
    unticked.waterlineFinishing.finishFlats = false;
    CHECK_FALSE(SameSettings(back, unticked));
    CHECK(&WaterlineSettingsOf(back) == &back.waterlineFinishing);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

namespace
{
    // A 40 x 40 x 5 block with a half sphere of radius 10 on its top, the block's
    // footprint the area, and a 20 high stock.
    WaterlineRoughToolpathInput ShelfInput(const SequenceStep& step, const Tool& tool)
    {
        WaterlineRoughToolpathInput input;
        input.operation = &step;
        input.tool = &tool;
        ProfileChain footprint;
        footprint.ok = true;
        footprint.closed = true;
        footprint.path = StockFootprint(Frame(), { { 0, 0, 0 }, { 40, 40, 20 } });
        input.outlines = { footprint };
        input.partTriangles = Box({ 0, 0, 0 }, { 40, 40, 5 });
        std::vector<Vec3> dome = HalfSphere(20, 20, 10);
        for (Vec3& p : dome)
            p.z += 5;
        input.partTriangles.insert(input.partTriangles.end(), dome.begin(), dome.end());
        input.haveStockMaxZ = true;
        input.stockMaxZ = 20;
        input.haveStockMinZ = true;
        input.stockMinZ = 0;
        input.havePartMaxZ = true;
        input.partMaxZ = 15;
        return input;
    }

    // The cutting moves at height z, as segments.
    std::vector<std::pair<Vec3, Vec3>> CutsAt(const Toolpath& toolpath, double z)
    {
        std::vector<std::pair<Vec3, Vec3>> segments;
        for (size_t i = 1; i < toolpath.moves.size(); ++i)
        {
            const ToolpathMove& a = toolpath.moves[i - 1];
            const ToolpathMove& b = toolpath.moves[i];
            if (b.kind == MoveKind::Feed && b.feed == 1200 && std::fabs(a.to.z - z) < 1e-6 &&
                std::fabs(b.to.z - z) < 1e-6)
                segments.push_back({ a.to, b.to });
        }
        return segments;
    }

    double DistanceTo(const std::vector<std::pair<Vec3, Vec3>>& segments, double x, double y)
    {
        double best = 1e9;
        for (const auto& [a, b] : segments)
        {
            const double dx = b.x - a.x, dy = b.y - a.y;
            const double length2 = dx * dx + dy * dy;
            double f = length2 > 0.0 ? ((x - a.x) * dx + (y - a.y) * dy) / length2 : 0.0;
            f = (std::max)(0.0, (std::min)(1.0, f));
            best = (std::min)(best, std::hypot(a.x + dx * f - x, a.y + dy * f - y));
        }
        return best;
    }
}

TEST_CASE("finish flats clears a shelf round a dome, and without it only the dome is lapped")
{
    Tool flat;
    flat.type = ToolType::FlatEndMill;
    flat.diameter = 6;
    flat.overallLength = 40;
    SequenceStep step = FinishStep();
    step.waterlineFinishing.stepoverDistance = 2.0;

    step.waterlineFinishing.finishFlats = false;
    const ToolpathResult plain = GenerateWaterlineFinishToolpath(ShelfInput(step, flat));
    REQUIRE(plain.ok);
    for (const auto& [a, b] : CutsAt(plain.toolpath, 5.0))
        CHECK(std::hypot(b.x - 20, b.y - 20) < 14.5);   // the lap round the dome, 10 + 3 out

    step.waterlineFinishing.finishFlats = true;
    const ToolpathResult flats = GenerateWaterlineFinishToolpath(ShelfInput(step, flat));
    REQUIRE(flats.ok);
    const auto shelf = CutsAt(flats.toolpath, 5.0);
    REQUIRE(!shelf.empty());
    // Every place on the shelf the tool's centre can reach is within half a stepover of a pass.
    int missed = 0;
    for (double x = 1; x <= 39; x += 1)
    {
        for (double y = 1; y <= 39; y += 1)
        {
            if (std::hypot(x - 20, y - 20) < 13.5)
                continue;
            if (DistanceTo(shelf, x, y) > 1.5)
                ++missed;
        }
    }
    CHECK(missed == 0);
    // Nothing on the shelf comes closer to the dome than the tool's radius.
    for (const auto& [a, b] : shelf)
        CHECK(std::hypot(b.x - 20, b.y - 20) > 12.8);
}

TEST_CASE("finish flats leaves the stock to leave on the flat")
{
    Tool flat;
    flat.type = ToolType::FlatEndMill;
    flat.diameter = 6;
    flat.overallLength = 40;
    SequenceStep step = FinishStep();
    step.waterlineFinishing.stepoverDistance = 2.0;
    step.waterlineFinishing.stockToLeave = 0.5;
    step.waterlineFinishing.finishFlats = true;
    const ToolpathResult result = GenerateWaterlineFinishToolpath(ShelfInput(step, flat));
    REQUIRE(result.ok);
    CHECK(CutsAt(result.toolpath, 5.0).empty());
    const auto shelf = CutsAt(result.toolpath, 5.5);
    REQUIRE(!shelf.empty());
    CHECK(DistanceTo(shelf, 2, 2) < 1.5);
}

// Run by name (-tc="waterline finishing timing*" --no-skip) to see how long a realistic job takes.
TEST_CASE("waterline finishing timing on a half sphere" * doctest::skip())
{
    SequenceStep step = FinishStep();
    step.waterlineFinishing.stepDown = 0.25;
    step.waterlineFinishing.stepoverDistance = 0.5;
    const Tool tool = Ball(6);
    const auto t0 = std::chrono::steady_clock::now();
    const ToolpathResult result = GenerateWaterlineFinishToolpath(Input(step, tool));
    const auto t1 = std::chrono::steady_clock::now();
    REQUIRE(result.ok);
    MESSAGE("waterline finishing: " << result.toolpath.moves.size() << " moves in "
            << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() << " ms");
}


namespace
{
    // How many times a cut turns back on itself (more than 120 degrees) along a
    // level, with both moves at least 0.02 long: spikes and little loops.
    int Reversals(const Toolpath& toolpath)
    {
        int count = 0;
        for (size_t i = 2; i < toolpath.moves.size(); ++i)
        {
            const ToolpathMove& a = toolpath.moves[i - 2];
            const ToolpathMove& b = toolpath.moves[i - 1];
            const ToolpathMove& c = toolpath.moves[i];
            if (b.kind != MoveKind::Feed || c.kind != MoveKind::Feed || b.feed != 1200 || c.feed != 1200)
                continue;
            if (std::fabs(a.to.z - b.to.z) > 1e-9 || std::fabs(b.to.z - c.to.z) > 1e-9)
                continue;
            const Vec3 u = b.to - a.to, v = c.to - b.to;
            const double lu = Length(u), lv = Length(v);
            if (lu < 0.02 || lv < 0.02)
                continue;
            if (Dot(u, v) / (lu * lv) < -0.5)
                ++count;
        }
        return count;
    }
}

TEST_CASE("laps over a shallow saddle between two domes have no spikes")
{
    SequenceStep step = FinishStep();
    step.waterlineFinishing.stepDown = 0.5;
    const Tool tool = Ball(6);
    WaterlineRoughToolpathInput input = Input(step, tool);
    // Two low domes overlapping, so the ground between them is a shallow saddle.
    input.partTriangles = HalfSphere(22, 30, 14, 48, 24);
    for (Vec3& p : input.partTriangles)
        p.z *= 0.4;
    std::vector<Vec3> second = HalfSphere(38, 30, 14, 48, 24);
    for (Vec3& p : second)
        p.z *= 0.4;
    input.partTriangles.insert(input.partTriangles.end(), second.begin(), second.end());
    input.partMaxZ = 5.6;
    const ToolpathResult result = GenerateWaterlineFinishToolpath(input);
    REQUIRE(result.ok);
    const int reversals = Reversals(result.toolpath);
    MESSAGE("saddle: " << result.toolpath.moves.size() << " moves, " << reversals << " reversals");
    CHECK(reversals == 0);
}

TEST_CASE("laps moved onto two overlapping spheres' true surfaces have no spikes")
{
    SequenceStep step = FinishStep();
    step.waterlineFinishing.stepDown = 0.5;
    const Tool tool = Ball(6);
    WaterlineRoughToolpathInput input = Input(step, tool);
    input.partTriangles = HalfSphere(22, 30, 12, 24, 12);
    input.partTriangleFaces.assign(input.partTriangles.size() / 3, 0);
    const std::vector<Vec3> second = HalfSphere(38, 30, 12, 24, 12);
    input.partTriangles.insert(input.partTriangles.end(), second.begin(), second.end());
    input.partTriangleFaces.resize(input.partTriangles.size() / 3, 1);
    input.partMaxZ = 12;
    for (double cx : { 22.0, 38.0 })
    {
        AnalyticSurface sphere;
        sphere.kind = SurfaceKind::Sphere;
        sphere.centre = { cx, 30, 0 };
        sphere.radius = 12;
        sphere.onFaceTolerance = 0.2;
        input.faceSurfaces.push_back(sphere);
    }
    const ToolpathResult result = GenerateWaterlineFinishToolpath(input);
    REQUIRE(result.ok);
    const int reversals = Reversals(result.toolpath);
    MESSAGE("spheres: " << result.toolpath.moves.size() << " moves, " << reversals << " reversals, "
                        << result.refinedPoints << " refined");
    CHECK(reversals == 0);
}


namespace
{
    // The top of a sphere of radius r centred at (cx, cy, cz), above z = 0, facets outward.
    std::vector<Vec3> Cap(double cx, double cy, double cz, double r, int around = 96, int up = 24)
    {
        std::vector<Vec3> mesh;
        const double low = std::asin((std::max)(-1.0, (std::min)(1.0, -cz / r)));
        const auto at = [&](int a, int u) {
            const double theta = 2.0 * kPi * a / around;
            const double phi = low + (0.5 * kPi - low) * u / up;
            return Vec3{ cx + r * std::cos(phi) * std::cos(theta), cy + r * std::cos(phi) * std::sin(theta),
                         cz + r * std::sin(phi) };
        };
        for (int u = 0; u < up; ++u)
        {
            for (int a = 0; a < around; ++a)
            {
                const Vec3 p00 = at(a, u), p10 = at(a + 1, u), p01 = at(a, u + 1), p11 = at(a + 1, u + 1);
                mesh.insert(mesh.end(), { p00, p10, p11 });
                if (u + 1 < up)
                    mesh.insert(mesh.end(), { p00, p11, p01 });
            }
        }
        return mesh;
    }
}

TEST_CASE("laps on a shallow surface whose true answers wobble a little have no hooks")
{
    // A shallow cap (radius 60, 8 high), its closest points wobbling by up to 0.002
    // along the normal, as Alibre's answers on a spline might against its coarse facets.
    SequenceStep step = FinishStep();
    step.waterlineFinishing.stepDown = 0.25;
    const Tool tool = Ball(6);
    WaterlineRoughToolpathInput input = Input(step, tool);
    const Vec3 centre{ 30, 30, -52 };
    input.partTriangles = Cap(30, 30, -52, 60, 48, 12);
    input.partTriangleFaces.assign(input.partTriangles.size() / 3, 0);
    input.partMaxZ = 8;
    input.closestPoint = [&](int, const Vec3& p, Vec3& closest, bool& onFace) {
        const Vec3 d = p - centre;
        const double length = Length(d);
        const double wobble = 0.002 * std::sin(p.x * 37.0) * std::cos(p.y * 41.0);
        closest = centre + d * ((60.0 + wobble) / length);
        onFace = closest.z >= 0.0;
        return true;
    };
    const ToolpathResult result = GenerateWaterlineFinishToolpath(input);
    REQUIRE(result.ok);
    CHECK(result.refinedPoints > 100);
    const int reversals = Reversals(result.toolpath);
    // Hooks: a turn of more than 90 degrees between two short moves.
    int hooks = 0;
    for (size_t i = 2; i < result.toolpath.moves.size(); ++i)
    {
        const ToolpathMove& a = result.toolpath.moves[i - 2];
        const ToolpathMove& b = result.toolpath.moves[i - 1];
        const ToolpathMove& c = result.toolpath.moves[i];
        if (b.kind != MoveKind::Feed || c.kind != MoveKind::Feed || b.feed != 1200 || c.feed != 1200)
            continue;
        if (std::fabs(a.to.z - b.to.z) > 1e-9 || std::fabs(b.to.z - c.to.z) > 1e-9)
            continue;
        const Vec3 u = b.to - a.to, v = c.to - b.to;
        if (Length(u) < 0.5 && Length(v) < 0.5 && Dot(u, v) < 0.0)
        {
            ++hooks;
        }
    }
    CHECK(result.lapTurnBacks == 0);
    CHECK(result.lapZigzags == 0);
    MESSAGE("wobbly cap: " << result.toolpath.moves.size() << " moves, " << reversals << " reversals, " << hooks
                           << " hooks, " << result.lapPointsSmoothed << " smoothed");
    CHECK(reversals == 0);
    CHECK(hooks == 0);
}

namespace
{
    // Short zigzags: an S-bend within 1 mm at one level with a turn of more than 90 degrees.
    int Zigzags(const Toolpath& toolpath)
    {
        int count = 0;
        const auto& m = toolpath.moves;
        for (size_t i = 3; i < m.size(); ++i)
        {
            bool ok = true;
            for (size_t k = i - 2; k <= i; ++k)
                ok = ok && m[k].kind == MoveKind::Feed && m[k].feed == 1200;
            for (size_t k = i - 2; k <= i; ++k)
                ok = ok && std::fabs(m[k].to.z - m[i - 3].to.z) < 1e-9;
            if (!ok)
                continue;
            Vec3 d[3];
            double total = 0.0;
            for (int k = 0; k < 3; ++k)
            {
                d[k] = m[i - 2 + k].to - m[i - 3 + k].to;
                total += Length(d[k]);
            }
            const double c0 = d[0].x * d[1].y - d[0].y * d[1].x, c1 = d[1].x * d[2].y - d[1].y * d[2].x;
            if (total < 1.0 && c0 * c1 < 0.0 && std::fabs(c0) > 1e-4 && std::fabs(c1) > 1e-4 &&
                (std::min)(Dot(d[0], d[1]), Dot(d[1], d[2])) < 0.0)
                ++count;
        }
        return count;
    }
}

TEST_CASE("laps on a nearly flat surface with coarse facets and wobbling answers have no glitches")
{
    // A cap of radius 120 only 6 high, on 24 x 6 facets (well inside the sphere),
    // its true-surface answers wobbling by up to 0.01: points move far sideways.
    SequenceStep step = FinishStep();
    step.waterlineFinishing.stepDown = 0.25;
    const Tool tool = Ball(6);
    WaterlineRoughToolpathInput input = Input(step, tool);
    const Vec3 centre{ 30, 30, -114 };
    input.partTriangles = Cap(30, 30, -114, 120, 24, 6);
    input.partTriangleFaces.assign(input.partTriangles.size() / 3, 0);
    input.partMaxZ = 6;
    input.closestPoint = [&](int, const Vec3& p, Vec3& closest, bool& onFace) {
        const Vec3 d = p - centre;
        const double wobble = 0.01 * std::sin(p.x * 37.0) * std::cos(p.y * 41.0);
        closest = centre + d * ((120.0 + wobble) / Length(d));
        onFace = closest.z >= 0.0;
        return true;
    };
    const ToolpathResult result = GenerateWaterlineFinishToolpath(input);
    REQUIRE(result.ok);
    CHECK(result.refinedPoints > 100);
    const int reversals = Reversals(result.toolpath);
    const int zigzags = Zigzags(result.toolpath);
    MESSAGE("flat cap: " << result.toolpath.moves.size() << " moves, in the moves " << reversals << " turn-backs and "
                         << zigzags << " zigzags (links between pieces included), in the laps " << result.lapTurnBacks
                         << " and " << result.lapZigzags << ", " << result.lapPointsSmoothed << " put back");
    // The laps themselves; the moves also turn round at the tip of a thin sliver
    // of a lap where the area's corner cuts one off, which isn't counted.
    CHECK(result.lapTurnBacks == 0);
    CHECK(result.lapZigzags == 0);
}

namespace
{
    // A straight lap along x from 0 to 10 at y = 0, with a finger at x = 5 going up
    // to y = length and back, width apart.
    Polyline FingerLap(double width, double length)
    {
        Polyline lap;
        for (double x = 0.0; x < 5.0; x += 0.1)
            lap.push_back({ x, 0, 0 });
        for (double y = 0.0; y <= length + 1e-9; y += 0.05)
            lap.push_back({ 5.0, y, 0 });
        for (double y = length; y >= -1e-9; y -= 0.05)
            lap.push_back({ 5.0 + width, y, 0 });
        for (double x = 5.0 + width + 0.1; x <= 10.0 + 1e-9; x += 0.1)
            lap.push_back({ x, 0, 0 });
        return lap;
    }

    // How far the lap reaches up from y = 0: the finger's tip, or 0 once it's gone.
    double Reach(const Polyline& p)
    {
        double reach = 0.0;
        for (const Vec3& q : p)
            reach = (std::max)(reach, q.y);
        return reach;
    }
}

TEST_CASE("a thin finger in a lap is cut across where the tool keeps clear of the part")
{
    const auto clear = [](const Vec3&, const Vec3&) { return true; };
    Polyline lap = FingerLap(0.1, 0.6);
    REQUIRE(Reach(lap) == doctest::Approx(0.6));
    CHECK(RemoveThinFingers(lap, false, clear) == 1);
    CHECK(Reach(lap) < 1e-9);
    CHECK(lap.front().x == 0.0);
    CHECK(lap.back().x == doctest::Approx(10.0));

    // Where the shortcut would dip into the part, the finger stays.
    Polyline blocked = FingerLap(0.1, 0.6);
    const auto never = [](const Vec3&, const Vec3&) { return false; };
    CHECK(RemoveThinFingers(blocked, false, never) == 0);
    CHECK(blocked.size() == FingerLap(0.1, 0.6).size());

    // A wide lobe and a long finger are real, and stay.
    Polyline wide = FingerLap(1.0, 0.6);
    CHECK(RemoveThinFingers(wide, false, clear) == 0);
    Polyline longer = FingerLap(0.1, 5.0);
    CHECK(RemoveThinFingers(longer, false, clear) == 0);
}

TEST_CASE("a small loop in a closed lap is cut off, and a small real lap stays")
{
    const auto clear = [](const Vec3&, const Vec3&) { return true; };
    // A square lap 10 across with a little loop (0.15 across) on one side.
    Polyline lap;
    for (double x = 0.0; x < 5.0; x += 0.1)
        lap.push_back({ x, 0, 0 });
    for (int k = 0; k < 12; ++k)
    {
        const double a = 2.0 * 3.14159265358979 * k / 12.0;
        lap.push_back({ 5.0 + 0.075 * std::sin(a), 0.075 - 0.075 * std::cos(a), 0 });
    }
    for (double x = 5.0; x < 10.0; x += 0.1)
        lap.push_back({ x, 0, 0 });
    for (double y = 0.0; y < 10.0; y += 0.1)
        lap.push_back({ 10, y, 0 });
    for (double x = 10.0; x > 0.0; x -= 0.1)
        lap.push_back({ x, 10, 0 });
    for (double y = 10.0; y > 0.0; y -= 0.1)
        lap.push_back({ 0, y, 0 });
    const size_t before = lap.size();
    CHECK(RemoveThinFingers(lap, true, clear) == 1);
    CHECK(lap.size() < before);

    // A small round lap of radius 0.5 is a real lap round a peak.
    Polyline ring;
    for (int k = 0; k < 40; ++k)
    {
        const double a = 2.0 * 3.14159265358979 * k / 40.0;
        ring.push_back({ 0.5 * std::cos(a), 0.5 * std::sin(a), 0 });
    }
    CHECK(RemoveThinFingers(ring, true, clear) == 0);
    CHECK(ring.size() == 40);
}

namespace
{
    // The heights the cuts are at, lowest first.
    std::vector<double> CutLevels(const Toolpath& toolpath)
    {
        std::set<long long> found;
        for (const ToolpathMove& move : toolpath.moves)
        {
            if (move.kind == MoveKind::Feed && move.feed == 1200)
                found.insert(std::llround(move.to.z * 1e6));
        }
        std::vector<double> levels;
        for (long long z : found)
            levels.push_back(z * 1e-6);
        return levels;
    }
}

TEST_CASE("without Optimise Stepover the levels are the stepdown's; with it, more where the slope is shallow")
{
    SequenceStep step = FinishStep();
    step.waterlineFinishing.stepoverDistance = 1.0;
    const Tool tool = Ball(6);

    step.waterlineFinishing.optimiseStepover = false;
    const ToolpathResult plain = GenerateWaterlineFinishToolpath(Input(step, tool));
    REQUIRE(plain.ok);
    const std::vector<double> even = CutLevels(plain.toolpath);
    REQUIRE(even.size() >= 10);
    for (size_t k = 1; k < even.size(); ++k)
        CHECK(even[k] - even[k - 1] == doctest::Approx(1.0).epsilon(1e-6));

    step.waterlineFinishing.optimiseStepover = true;
    const ToolpathResult optimised = GenerateWaterlineFinishToolpath(Input(step, tool));
    REQUIRE(optimised.ok);
    const std::vector<double> slope = CutLevels(optimised.toolpath);
    CHECK(slope.size() > even.size());
    // Low down, where the dome is steep, the full stepdown is kept.
    for (size_t k = 1; k < slope.size() && slope[k] < 6.0; ++k)
        CHECK(slope[k] - slope[k - 1] == doctest::Approx(1.0).epsilon(1e-6));
    // Near the top, where it's shallow, the levels are closer.
    CHECK(slope.back() - slope[slope.size() - 2] < 1.0);
}

TEST_CASE("Optimise Stepover is saved, and a file without it reads as off")
{
    SequenceStep finishing = FinishStep();
    finishing.id = NewStableId();
    finishing.name = "Waterline Finishing 1";
    finishing.waterlineFinishing.optimiseStepover = true;
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
    group.children.push_back(finishing);
    job.sequence.push_back(group);
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "CAMForAlibre-optimise-test.cam";
    REQUIRE(SaveJob(job, path).ok);
    LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    CHECK(loaded.job.sequence[1].children[0].waterlineFinishing.optimiseStepover);

    // The same file without the field: off.
    std::ifstream in(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    // Renamed to something nothing reads, as if it had never been written.
    const std::string field = "\"optimiseStepover\"";
    const size_t at = text.find(field);
    REQUIRE(at != std::string::npos);
    text.replace(at, field.size(), "\"notRead\"");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << text;
    }
    loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    CHECK_FALSE(loaded.job.sequence[1].children[0].waterlineFinishing.optimiseStepover);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST_CASE("waterline finishing's laps reach past the area's edge by the overtravel")
{
    const Tool tool = Ball(6);
    const auto furthest = [&](double overtravel) {
        SequenceStep step = FinishStep();
        step.waterlineFinishing.extension = overtravel;
        WaterlineRoughToolpathInput input = Input(step, tool);
        // A 20 x 20 area in the middle, smaller than the dome's laps.
        input.outlines[0].path = StockFootprint(Frame(), { { 20, 20, 0 }, { 40, 40, 20 } });
        const ToolpathResult result = GenerateWaterlineFinishToolpath(input);
        REQUIRE(result.ok);
        double reach = 0.0;
        for (const Vec3& p : Cuts(result.toolpath))
            reach = (std::max)(reach, (std::max)(std::fabs(p.x - 30), std::fabs(p.y - 30)));
        return reach;
    };
    CHECK(furthest(0.0) < 10.3);
    CHECK(furthest(3.0) > 12.5);
    CHECK(furthest(3.0) < 13.3);
}

TEST_CASE("waterline finishing enters each lap at rapid to the approach distance above it, then the plunge feed")
{
    SequenceStep step = FinishStep();
    step.waterlineFinishing.approachDistance = 2.0;
    const Tool tool = Ball(6);
    const ToolpathResult result = GenerateWaterlineFinishToolpath(Input(step, tool));
    REQUIRE(result.ok);
    int entries = 0;
    const auto& m = result.toolpath.moves;
    for (size_t i = 1; i < m.size(); ++i)
    {
        if (m[i].kind != MoveKind::Feed || m[i].feed != 150)
            continue;
        if (m[i].to.z + 2.0 > 30.0)
            continue;   // capped at the clearance plane
        CHECK(m[i - 1].kind == MoveKind::Rapid);
        CHECK(m[i - 1].to.z == doctest::Approx(m[i].to.z + 2.0));
        ++entries;
    }
    CHECK(entries > 0);
}

TEST_CASE("a slope range keeps waterline's laps to the steep part of the dome")
{
    // The ball's tip on the dome: its slope where its centre is d out is asin(d / 18).
    SequenceStep step = FinishStep();
    step.waterlineFinishing.slopeFrom = 45.0;
    const Tool tool = Ball(6);
    const ToolpathResult all = GenerateWaterlineFinishToolpath(Input(FinishStep(), tool));
    const ToolpathResult steep = GenerateWaterlineFinishToolpath(Input(step, tool));
    REQUIRE(all.ok);
    REQUIRE(steep.ok);
    const std::vector<Vec3> cuts = Cuts(steep.toolpath);
    REQUIRE(cuts.size() > 20);
    CHECK(cuts.size() < Cuts(all.toolpath).size());
    double flattest = 90.0;
    for (const Vec3& p : cuts)
    {
        const double d = std::hypot(p.x - 30, p.y - 30);
        if (d < 18.0)
            flattest = (std::min)(flattest, std::asin(d / 18.0) * 180.0 / 3.14159265358979323846);
    }
    MESSAGE("flattest lap point " << flattest << " degrees");
    CHECK(flattest > 42.0);     // the grid's slope is within a few degrees
}

TEST_CASE("slope limits must run from 0 to 90 degrees, From under To")
{
    WaterlineRoughingSettings w;
    CHECK(ValidateWaterlineRoughing(w).empty());
    w.slopeFrom = 50.0;
    w.slopeTo = 40.0;
    CHECK_FALSE(ValidateWaterlineRoughing(w).empty());
    w.slopeFrom = -1.0;
    w.slopeTo = 40.0;
    CHECK_FALSE(ValidateWaterlineRoughing(w).empty());
}
