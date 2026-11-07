// NurbsTests.cpp : tests for NURBS surfaces and finishing on them.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>

#include "CamJob.h"
#include "Nurbs.h"
#include "Stock.h"
#include "SurfaceGeometry.h"
#include "WaterlineRoughToolpath.h"

using namespace camcore;

namespace
{
    // A clamped, evenly spaced knot vector for count control points of this order.
    std::vector<double> ClampedKnots(int count, int order)
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

    // A bicubic patch over 60 x 60 with a smooth bump of height h in its middle.
    NurbsSurface Bump(double h)
    {
        NurbsSurface s;
        s.orderU = s.orderV = 4;
        s.countU = s.countV = 8;
        s.knotsU = ClampedKnots(8, 4);
        s.knotsV = ClampedKnots(8, 4);
        for (int j = 0; j < 8; ++j)
        {
            for (int i = 0; i < 8; ++i)
            {
                const double x = 60.0 * i / 7.0, y = 60.0 * j / 7.0;
                const double z = h * std::exp(-((x - 30) * (x - 30) + (y - 30) * (y - 30)) / 250.0);
                s.points.push_back({ x, y, z });
            }
        }
        return s;
    }

    // A wavy bicubic patch, 6 x 5 control points.
    NurbsSurface Wavy()
    {
        NurbsSurface s;
        s.orderU = s.orderV = 4;
        s.countU = 6;
        s.countV = 5;
        s.knotsU = ClampedKnots(6, 4);
        s.knotsV = ClampedKnots(5, 4);
        for (int j = 0; j < 5; ++j)
        {
            for (int i = 0; i < 6; ++i)
                s.points.push_back({ 2.0 * i, 2.5 * j, std::sin(1.3 * i) * std::cos(0.9 * j) });
        }
        return s;
    }

    // A quarter of a cylinder of radius r and height h, exactly (rational quadratic in u).
    NurbsSurface QuarterCylinder(double r, double h)
    {
        NurbsSurface s;
        s.orderU = 3;
        s.orderV = 2;
        s.countU = 3;
        s.countV = 2;
        s.knotsU = { 0, 0, 0, 1, 1, 1 };
        s.knotsV = { 0, 0, 1, 1 };
        const double c = std::sqrt(0.5);
        for (double z : { 0.0, h })
        {
            s.points.push_back({ r, 0, z });
            s.points.push_back({ r, r, z });
            s.points.push_back({ 0, r, z });
            s.weights.push_back(1.0);
            s.weights.push_back(c);
            s.weights.push_back(1.0);
        }
        return s;
    }

    // The surface sampled into triangles, n x n cells.
    std::vector<Vec3> Mesh(const NurbsSurface& s, int n)
    {
        std::vector<Vec3> mesh;
        const auto at = [&](int i, int j) {
            Vec3 p;
            s.Evaluate(s.LowU() + (s.HighU() - s.LowU()) * i / n, s.LowV() + (s.HighV() - s.LowV()) * j / n, p);
            return p;
        };
        for (int j = 0; j < n; ++j)
        {
            for (int i = 0; i < n; ++i)
            {
                const Vec3 a = at(i, j), b = at(i + 1, j), c = at(i + 1, j + 1), d = at(i, j + 1);
                mesh.insert(mesh.end(), { a, b, c, a, c, d });
            }
        }
        return mesh;
    }
}

TEST_CASE("a bilinear NURBS patch evaluates exactly with its derivatives")
{
    NurbsSurface s;
    s.orderU = s.orderV = 2;
    s.countU = s.countV = 2;
    s.knotsU = { 0, 0, 1, 1 };
    s.knotsV = { 0, 0, 1, 1 };
    s.points = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 1, 1, 1 } };     // z = u v
    REQUIRE(s.Validate());
    Vec3 p, du, dv, duu, duv, dvv;
    s.Evaluate(0.3, 0.6, p, &du, &dv, &duu, &duv, &dvv);
    CHECK(Length(p - Vec3{ 0.3, 0.6, 0.18 }) < 1e-12);
    CHECK(Length(du - Vec3{ 1, 0, 0.6 }) < 1e-12);
    CHECK(Length(dv - Vec3{ 0, 1, 0.3 }) < 1e-12);
    CHECK(Length(duu) < 1e-12);
    CHECK(Length(duv - Vec3{ 0, 0, 1 }) < 1e-12);
    CHECK(Length(dvv) < 1e-12);
}

TEST_CASE("a bicubic NURBS patch's derivatives match finite differences")
{
    const NurbsSurface s = Wavy();
    REQUIRE(s.Validate());
    const double h = 1e-5;
    for (double u : { 0.1, 0.37, 0.5, 0.82 })
    {
        for (double v : { 0.15, 0.44, 0.9 })
        {
            Vec3 p, du, dv, duu, duv, dvv;
            s.Evaluate(u, v, p, &du, &dv, &duu, &duv, &dvv);
            Vec3 pu1, pu0, pv1, pv0, du1, du0, dv1;
            s.Evaluate(u + h, v, pu1, &du1);
            s.Evaluate(u - h, v, pu0, &du0);
            s.Evaluate(u, v + h, pv1, nullptr, &dv1);
            s.Evaluate(u, v - h, pv0);
            Vec3 dvBack;
            s.Evaluate(u, v - h, pv0, nullptr, &dvBack);
            Vec3 duAbove;
            s.Evaluate(u, v + h, pv1, &duAbove);
            Vec3 duBelow;
            s.Evaluate(u, v - h, pv0, &duBelow);
            CHECK(Length(du - (pu1 - pu0) * (0.5 / h)) < 1e-5);
            CHECK(Length(dv - (pv1 - pv0) * (0.5 / h)) < 1e-5);
            CHECK(Length(duu - (du1 - du0) * (0.5 / h)) < 1e-3);
            CHECK(Length(dvv - (dv1 - dvBack) * (0.5 / h)) < 1e-3);
            CHECK(Length(duv - (duAbove - duBelow) * (0.5 / h)) < 1e-3);
        }
    }
}

TEST_CASE("a rational quarter cylinder is exact, and its closest points are too")
{
    NurbsSurface s = QuarterCylinder(10, 5);
    REQUIRE(s.Validate());
    for (double u : { 0.0, 0.2, 0.5, 0.77, 1.0 })
    {
        Vec3 p;
        s.Evaluate(u, 0.4, p);
        CHECK(std::hypot(p.x, p.y) == doctest::Approx(10.0).epsilon(1e-12));
        CHECK(p.z == doctest::Approx(2.0));
    }
    s.Prepare();
    for (const Vec3& q : { Vec3{ 12, 3, 1 }, Vec3{ 4, 5, 4.5 }, Vec3{ 1, 14, 2 }, Vec3{ 7.07, 7.07, 3 } })
    {
        Vec3 closest;
        REQUIRE(s.ClosestPoint(q, closest));
        const double r = std::hypot(q.x, q.y);
        const Vec3 exact{ q.x * 10 / r, q.y * 10 / r, q.z };
        CHECK(Length(closest - exact) < 1e-9);
    }
}

TEST_CASE("closest points on a wavy NURBS patch match a dense search")
{
    NurbsSurface s = Wavy();
    s.Prepare();
    for (const Vec3& q : { Vec3{ 3.1, 4.2, 2.0 }, Vec3{ 7.5, 1.0, -1.5 }, Vec3{ 5.0, 6.0, 0.3 }, Vec3{ 9.0, 9.0, 3.0 } })
    {
        Vec3 closest;
        REQUIRE(s.ClosestPoint(q, closest));
        double best = 1e9;
        const int n = 400;
        for (int j = 0; j <= n; ++j)
        {
            for (int i = 0; i <= n; ++i)
            {
                Vec3 p;
                s.Evaluate(static_cast<double>(i) / n, static_cast<double>(j) / n, p);
                best = (std::min)(best, Length(p - q));
            }
        }
        CHECK(Length(closest - q) <= best + 1e-6);
    }
}

TEST_CASE("a NURBS surface with the wrong number of knots isn't valid")
{
    NurbsSurface s = Wavy();
    CHECK(s.Validate());
    s.knotsU.pop_back();
    CHECK_FALSE(s.Validate());
    s = Wavy();
    s.points.pop_back();
    CHECK_FALSE(s.Validate());
    s = QuarterCylinder(10, 5);
    s.weights[1] = 0.0;
    CHECK_FALSE(s.Validate());
}

TEST_CASE("a NURBS surface fits its own mesh and not a shifted one")
{
    auto shared = std::make_shared<NurbsSurface>(QuarterCylinder(10, 5));
    shared->Prepare();
    AnalyticSurface surface;
    surface.kind = SurfaceKind::Nurbs;
    surface.nurbs = shared;
    std::vector<Vec3> points = Mesh(*shared, 12);
    CHECK(SurfaceFitsPoints(surface, points, 0.1));
    for (Vec3& p : points)
        p.x += 0.5;
    CHECK_FALSE(SurfaceFitsPoints(surface, points, 0.1));
}

namespace
{
    SequenceStep NurbsStep(StepType type)
    {
        SequenceStep step;
        step.type = type;
        step.clearance.mode = ClearanceMode::AbsoluteZ;
        step.clearance.absoluteZ = 30;
        step.feeds.plunge = 150;
        step.feeds.cut = 1200;
        WaterlineSettingsOf(step).stepDown = 0.5;
        WaterlineSettingsOf(step).stepoverMode = WaterlineStepoverMode::Distance;
        WaterlineSettingsOf(step).stepoverDistance = (type == StepType::ParallelFinishing) ? 1.0 : 100.0;
        WaterlineSettingsOf(step).clearFlats = false;
        return step;
    }

    Tool BallTool()
    {
        Tool tool;
        tool.type = ToolType::BallMill;
        tool.diameter = 6;
        tool.overallLength = 40;
        return tool;
    }

    // The bump as one face: its mesh, and either its NURBS or a callback asking the same NURBS.
    WaterlineRoughToolpathInput BumpInput(const SequenceStep& step, const Tool& tool,
                                          const std::shared_ptr<NurbsSurface>& bump, bool worked, size_t& calls)
    {
        WaterlineRoughToolpathInput input;
        input.operation = &step;
        input.tool = &tool;
        ProfileChain footprint;
        footprint.ok = true;
        footprint.closed = true;
        footprint.path = StockFootprint(Frame(), { { 0, 0, 0 }, { 60, 60, 20 } });
        input.outlines = { footprint };
        input.partTriangles = Mesh(*bump, 30);
        input.partTriangleFaces.assign(input.partTriangles.size() / 3, 0);
        input.haveStockMaxZ = true;
        input.stockMaxZ = 20;
        input.haveStockMinZ = true;
        input.stockMinZ = 0;
        input.havePartMaxZ = true;
        input.partMaxZ = 12;
        if (worked)
        {
            AnalyticSurface surface;
            surface.kind = SurfaceKind::Nurbs;
            surface.nurbs = bump;
            surface.onFaceTolerance = 1.0;
            input.faceSurfaces = { surface };
            input.closestPoint = [&calls](int, const Vec3&, Vec3&, bool&) {
                ++calls;
                return false;
            };
        }
        else
        {
            input.closestPoint = [bump, &calls](int, const Vec3& p, Vec3& closest, bool& onFace) {
                ++calls;
                onFace = true;
                return bump->ClosestPoint(p, closest);
            };
        }
        return input;
    }
}

TEST_CASE("finishing on a NURBS face works it out here, as asking would, with no glitches")
{
    auto bump = std::make_shared<NurbsSurface>(Bump(12));
    REQUIRE(bump->Validate());
    bump->Prepare();
    const Tool tool = BallTool();
    for (StepType type : { StepType::WaterlineFinishing, StepType::ParallelFinishing })
    {
        const SequenceStep step = NurbsStep(type);
        size_t calls = 0;
        const auto generate = [&](bool worked) {
            const WaterlineRoughToolpathInput input = BumpInput(step, tool, bump, worked, calls);
            return (type == StepType::WaterlineFinishing) ? GenerateWaterlineFinishToolpath(input)
                                                          : GenerateParallelFinishToolpath(input);
        };
        const ToolpathResult here = generate(true);
        REQUIRE(here.ok);
        CHECK(calls == 0);
        CHECK(here.refineCalls == 0);
        CHECK(here.refinedPoints > 100);
        CHECK(here.lapTurnBacks == 0);
        CHECK(here.lapZigzags == 0);

        calls = 0;
        const ToolpathResult asked = generate(false);
        REQUIRE(asked.ok);
        CHECK(calls > 0);
        if (type == StepType::WaterlineFinishing)
        {
            // Its laps are simplified by height, checked on the true surface where
            // that's worked out here and on the facets for a face asked about, so
            // the two needn't match point for point; both are clean.
            CHECK(asked.lapTurnBacks == 0);
            CHECK(asked.lapZigzags == 0);
            CHECK(here.toolpath.moves.size() <= asked.toolpath.moves.size());
            continue;
        }
        REQUIRE(here.toolpath.moves.size() == asked.toolpath.moves.size());
        double worst = 0.0;
        for (size_t i = 0; i < here.toolpath.moves.size(); ++i)
            worst = (std::max)(worst, Length(here.toolpath.moves[i].to - asked.toolpath.moves[i].to));
        CHECK(worst < 0.001);
    }
}

// Run by name (-tc="nurbs finishing timing*" --no-skip) to see how long it takes.
TEST_CASE("nurbs finishing timing on a bump" * doctest::skip())
{
    auto bump = std::make_shared<NurbsSurface>(Bump(12));
    bump->Prepare();
    const Tool tool = BallTool();
    const SequenceStep step = NurbsStep(StepType::WaterlineFinishing);
    size_t calls = 0;
    const WaterlineRoughToolpathInput input = BumpInput(step, tool, bump, true, calls);
    const auto t0 = std::chrono::steady_clock::now();
    const ToolpathResult result = GenerateWaterlineFinishToolpath(input);
    const auto t1 = std::chrono::steady_clock::now();
    REQUIRE(result.ok);
    MESSAGE("nurbs waterline finishing: " << result.toolpath.moves.size() << " moves, " << result.refinedPoints
            << " refined, in " << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() << " ms");
}
