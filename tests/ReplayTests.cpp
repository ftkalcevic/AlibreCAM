// ReplayTests.cpp : saving and loading a finishing operation's input, and
// replaying one saved by Generate in Alibre to find problems.

#include <doctest/doctest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>

#include "CamJob.h"
#include "DropCutter.h"
#include "FinishingDump.h"
#include "Stock.h"
#include "WaterlineRoughToolpath.h"

using namespace camcore;

namespace
{
    std::vector<double> Clamped(int count, int order)
    {
        std::vector<double> knots;
        const int spans = count - order + 1;
        for (int k = 0; k < order; ++k)
            knots.push_back(0.0);
        for (int k = 1; k < spans; ++k)
            knots.push_back(static_cast<double>(k) / spans);
        for (int k = 0; k < order; ++k)
            knots.push_back(1.0);
        return knots;
    }
}

TEST_CASE("a finishing operation's input saves and loads, giving the same toolpath")
{
    SequenceStep step;
    step.type = StepType::WaterlineFinishing;
    step.name = "Waterline Finishing 1";
    step.id = NewStableId();
    step.clearance.mode = ClearanceMode::AbsoluteZ;
    step.clearance.absoluteZ = 30;
    step.feeds.plunge = 150;
    step.feeds.cut = 1200;
    step.waterlineFinishing.stepDown = 1.0;
    Tool tool;
    tool.id = "ball";
    tool.name = "Ball";
    tool.type = ToolType::BallMill;
    tool.diameter = 6;
    tool.overallLength = 40;

    // A bicubic bump as one NURBS face.
    auto bump = std::make_shared<NurbsSurface>();
    bump->orderU = bump->orderV = 4;
    bump->countU = bump->countV = 6;
    bump->knotsU = Clamped(6, 4);
    bump->knotsV = Clamped(6, 4);
    for (int j = 0; j < 6; ++j)
    {
        for (int i = 0; i < 6; ++i)
        {
            const double x = 12.0 * i, y = 12.0 * j;
            bump->points.push_back({ x, y, 10.0 * std::exp(-((x - 30) * (x - 30) + (y - 30) * (y - 30)) / 300.0) });
        }
    }
    bump->Prepare();
    WaterlineRoughToolpathInput input;
    input.operation = &step;
    input.tool = &tool;
    ProfileChain footprint;
    footprint.ok = true;
    footprint.closed = true;
    footprint.path = StockFootprint(Frame(), { { 0, 0, 0 }, { 60, 60, 20 } });
    input.outlines = { footprint };
    for (int j = 0; j < 20; ++j)
    {
        for (int i = 0; i < 20; ++i)
        {
            Vec3 a, b, c, d;
            bump->Evaluate(i / 20.0, j / 20.0, a);
            bump->Evaluate((i + 1) / 20.0, j / 20.0, b);
            bump->Evaluate((i + 1) / 20.0, (j + 1) / 20.0, c);
            bump->Evaluate(i / 20.0, (j + 1) / 20.0, d);
            input.partTriangles.insert(input.partTriangles.end(), { a, b, c, a, c, d });
        }
    }
    input.partTriangleFaces.assign(input.partTriangles.size() / 3, 0);
    AnalyticSurface surface;
    surface.kind = SurfaceKind::Nurbs;
    surface.nurbs = bump;
    surface.onFaceTolerance = 1.0;
    input.faceSurfaces = { surface };
    input.haveStockMaxZ = true;
    input.stockMaxZ = 20;
    input.haveStockMinZ = true;
    input.stockMinZ = 0;
    input.havePartMaxZ = true;
    input.partMaxZ = 10;
    const ToolpathResult original = GenerateWaterlineFinishToolpath(input);
    REQUIRE(original.ok);

    const std::filesystem::path path = std::filesystem::temp_directory_path() / "CAMForAlibre-dump-test.json";
    std::string problem;
    REQUIRE(SaveFinishingInput(input, path, problem));
    FinishingDump dump;
    REQUIRE_MESSAGE(LoadFinishingInput(path, dump, problem), problem);
    CHECK(dump.operation.type == StepType::WaterlineFinishing);
    CHECK(dump.tool.type == ToolType::BallMill);
    CHECK(dump.input.partTriangles.size() == input.partTriangles.size());
    REQUIRE(dump.input.faceSurfaces.size() == 1);
    REQUIRE(dump.input.faceSurfaces[0].nurbs != nullptr);
    const ToolpathResult replayed = GenerateWaterlineFinishToolpath(dump.input);
    REQUIRE(replayed.ok);
    REQUIRE(replayed.toolpath.moves.size() == original.toolpath.moves.size());
    for (size_t i = 0; i < original.toolpath.moves.size(); ++i)
        CHECK(Length(replayed.toolpath.moves[i].to - original.toolpath.moves[i].to) < 1e-9);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::path job = path;
    job += ".cam";
    std::filesystem::remove(job, ignored);
}

namespace
{
    // Glitches in one lap, as CountLapGlitches finds them: (index of the point, what).
    std::vector<std::pair<size_t, const char*>> Glitches(const Polyline& p, bool closed)
    {
        std::vector<std::pair<size_t, const char*>> found;
        const size_t n = p.size();
        if (n < 3)
            return found;
        const size_t segments = closed ? n : n - 1;
        const auto segment = [&](size_t k) { return p[(k + 1) % n] - p[k]; };
        for (size_t k = 0; k + 1 < segments || (closed && k < segments); ++k)
        {
            const Vec3 u = segment(k), v = segment((k + 1) % segments);
            const double lu = Length(u), lv = Length(v);
            if (lu >= 0.02 && lv >= 0.02 && Dot(u, v) < -0.5 * lu * lv)
                found.push_back({ (k + 1) % n, "turn-back" });
            if (k + 2 >= segments && !closed)
                continue;
            const Vec3 x = segment((k + 2) % segments);
            const double c0 = u.x * v.y - u.y * v.x, c1 = v.x * x.y - v.y * x.x;
            if (lu + lv + Length(x) < 1.0 && c0 * c1 < 0.0 && std::fabs(c0) > 1e-4 && std::fabs(c1) > 1e-4 &&
                (std::min)(Dot(u, v), Dot(v, x)) < 0.0)
                found.push_back({ (k + 1) % n, "zigzag" });
            const double ten = std::sin(10.0 * 3.14159265358979 / 180.0), lx = Length(x);
            if (lu > 1e-9 && lv > 1e-9 && lx > 1e-9 && lv < 0.5 && c0 * c1 < 0.0 && std::fabs(c0) > ten * lu * lv &&
                std::fabs(c1) > ten * lv * lx)
                found.push_back({ (k + 1) % n, "wobble" });
        }
        return found;
    }

    void PrintNear(const char* stage, const std::vector<Polyline>& laps, const Vec3& at)
    {
        for (size_t lap = 0; lap < laps.size(); ++lap)
        {
            std::string text;
            for (size_t k = 0; k < laps[lap].size(); ++k)
            {
                const Vec3& q = laps[lap][k];
                if (std::hypot(q.x - at.x, q.y - at.y) > 0.6)
                    continue;
                char buffer[96];
                std::snprintf(buffer, sizeof buffer, " [%zu](%.4f,%.4f)", k, q.x, q.y);
                text += buffer;
            }
            if (!text.empty())
                MESSAGE(stage << " lap " << lap << ":" << text);
        }
    }
}

// Set CAMFORALIBRE_REPLAY to a dump Generate wrote (last-finishing.json) and run by
// name (-tc="replay*" --no-skip): it says what's wrong in the laps, stage by stage.
TEST_CASE("replay a saved finishing operation" * doctest::skip())
{
    char* pPath = nullptr;
    size_t length = 0;
    _dupenv_s(&pPath, &length, "CAMFORALIBRE_REPLAY");
    const std::string path = (pPath != nullptr) ? pPath : "";
    free(pPath);
    REQUIRE_MESSAGE(!path.empty(), "Set CAMFORALIBRE_REPLAY to the dump's path.");
    FinishingDump dump;
    std::string problem;
    REQUIRE_MESSAGE(LoadFinishingInput(path, dump, problem), problem);
    MESSAGE("replaying " << dump.operation.name << ": " << dump.input.partTriangles.size() / 3 << " triangles, "
                         << dump.input.faceSurfaces.size() << " faces' surfaces, tool " << dump.tool.diameter);
    // CAMFORALIBRE_REPLAY_OPTIMISE=1 (or 0) turns Waterline Finishing's Optimise Stepover on (or off).
    char* pOptimise = nullptr;
    size_t optimiseLength = 0;
    _dupenv_s(&pOptimise, &optimiseLength, "CAMFORALIBRE_REPLAY_OPTIMISE");
    if (pOptimise != nullptr)
        WaterlineSettingsOf(dump.operation).optimiseStepover = pOptimise[0] == '1';
    free(pOptimise);
    LapStages stages;
    dump.input.lapStages = &stages;
    const bool waterline = dump.operation.type == StepType::WaterlineFinishing;
    const ToolpathResult result = waterline ? GenerateWaterlineFinishToolpath(dump.input)
                                            : GenerateParallelFinishToolpath(dump.input);
    REQUIRE(result.ok);
    MESSAGE("moves " << result.toolpath.moves.size() << ", refined " << result.refinedPoints << ", asked "
                     << result.refineCalls << ", put back " << result.lapPointsSmoothed << ", turn-backs "
                     << result.lapTurnBacks << ", zigzags " << result.lapZigzags << ", wobbles " << result.lapWobbles
                     << ", fingers cut " << result.lapFingersRemoved << ", lap points " << result.lapPointsBefore
                     << " to " << result.lapPointsAfter);
    if (!waterline)
        return;
    double closest = 1e9;
    for (size_t k = 1; k < stages.levels.size(); ++k)
        closest = (std::min)(closest, std::fabs(stages.levels[k - 1] - stages.levels[k]));
    MESSAGE("levels " << stages.levels.size() << ", the closest " << closest << " apart, optimise stepover "
                      << std::string(WaterlineSettingsOf(dump.operation).optimiseStepover ? "on" : "off"));
    int shown = 0;
    for (size_t li = 0; li < stages.simplified.size(); ++li)
    {
        for (size_t lap = 0; lap < stages.simplified[li].size(); ++lap)
        {
            for (const auto& [k, what] : Glitches(stages.simplified[li][lap], stages.closed[li][lap]))
            {
                if (shown++ >= 12)
                    break;
                const Vec3 at = stages.simplified[li][lap][k];
                MESSAGE("== " << what << " at (" << at.x << ", " << at.y << ") level " << stages.levels[li]);
                PrintNear("  grid      ", stages.grid[li], at);
                PrintNear("  moved     ", stages.moved[li], at);
                PrintNear("  cleaned   ", stages.cleaned[li], at);
                PrintNear("  simplified", stages.simplified[li], at);
            }
        }
    }
}

// As the replay, but run as Waterline Roughing (stock to leave 0.5, stepdown 2, the
// default Stock Offset pattern), to check roughing on the same geometry.
TEST_CASE("replay as waterline roughing" * doctest::skip())
{
    char* pPath = nullptr;
    size_t length = 0;
    _dupenv_s(&pPath, &length, "CAMFORALIBRE_REPLAY");
    const std::string path = (pPath != nullptr) ? pPath : "";
    free(pPath);
    REQUIRE_MESSAGE(!path.empty(), "Set CAMFORALIBRE_REPLAY to the dump's path.");
    FinishingDump dump;
    std::string problem;
    REQUIRE_MESSAGE(LoadFinishingInput(path, dump, problem), problem);
    dump.operation.type = StepType::WaterlineRoughing;
    dump.operation.waterlineRoughing = WaterlineRoughingSettings();
    dump.operation.waterlineRoughing.stockToLeave = 0.5;
    dump.operation.waterlineRoughing.stepDown = 2.0;
    const ToolpathResult result = GenerateWaterlineRoughToolpath(dump.input);
    REQUIRE_MESSAGE(result.ok, result.problem);
    // How much cutting there is at each level.
    std::map<long long, double> cut;
    for (size_t i = 1; i < result.toolpath.moves.size(); ++i)
    {
        const ToolpathMove& a = result.toolpath.moves[i - 1];
        const ToolpathMove& b = result.toolpath.moves[i];
        if (b.kind == MoveKind::Feed && std::fabs(a.to.z - b.to.z) < 1e-9)
            cut[std::llround(b.to.z * 1000)] += std::hypot(b.to.x - a.to.x, b.to.y - a.to.y);
    }
    MESSAGE("roughing: " << result.toolpath.moves.size() << " moves over " << cut.size() << " levels");
    for (const auto& [z, length] : cut)
        MESSAGE("  level " << z / 1000.0 << ": " << length << " mm of cutting");
}
