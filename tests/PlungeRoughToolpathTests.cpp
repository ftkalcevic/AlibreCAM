// PlungeRoughToolpathTests.cpp : tests for Plunge Roughing's grid of plunges, their depths and its toolpath.

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>

#include "Boundary.h"
#include "CamJob.h"
#include "Facing.h"
#include "Features.h"
#include "Stock.h"
#include "PlungeRoughToolpath.h"

using namespace camcore;

namespace
{
    ProfileChain Rectangle(double x0, double y0, double x1, double y1, double z = 0.0)
    {
        ProfileChain chain;
        chain.ok = true;
        chain.closed = true;
        chain.path = PathFromPoints({ { x0, y0, z }, { x1, y0, z }, { x1, y1, z }, { x0, y1, z } });
        return chain;
    }

    // The 12 triangles of a box.
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

    SequenceStep PlungeStep()
    {
        SequenceStep step;
        step.type = StepType::PlungeRoughing;
        step.clearance.mode = ClearanceMode::AbsoluteZ;
        step.clearance.absoluteZ = 20;
        step.feeds.plunge = 150;
        step.feeds.retract = 900;
        step.plungeRoughing.stockToLeave = 0.5;
        step.plungeRoughing.approach = 2;
        step.plungeRoughing.cutAngle = 90;      // rows along X
        step.plungeRoughing.stepover = { true, 75.0, 0.0 };
        return step;
    }

    Tool Mill(double diameter, ToolType type = ToolType::FlatEndMill)
    {
        Tool tool;
        tool.type = type;
        tool.diameter = diameter;
        tool.overallLength = 30;
        return tool;
    }

    PlungeRoughToolpathInput Input(const SequenceStep& step, const Tool& tool)
    {
        PlungeRoughToolpathInput input;
        input.operation = &step;
        input.tool = &tool;
        input.outlines = { Rectangle(0, 0, 40, 30) };
        input.haveStockMaxZ = true;
        input.stockMaxZ = 10;
        input.haveStockMinZ = true;
        input.stockMinZ = 0;
        return input;
    }

    // Each plunge's bottom: the feed down's end.
    std::vector<Vec3> Bottoms(const Toolpath& toolpath)
    {
        std::vector<Vec3> bottoms;
        for (size_t i = 0; i < toolpath.moves.size(); ++i)
        {
            const ToolpathMove& move = toolpath.moves[i];
            if (move.kind == MoveKind::Feed && move.feed == 150)
                bottoms.push_back(move.to);
        }
        return bottoms;
    }

    // The 0 to 40 by 0 to 30 rectangle as a plunge area, in 0.1 mm cells.
    PlungeArea RectangleArea()
    {
        PlungeArea area;
        area.Reset(0, 0, 40, 30, 0.1);
        area.AddLoop(Rectangle(0, 0, 40, 30).path);
        area.Finish();
        return area;
    }

    // A flat face's triangles: a rectangle, as two triangles.
    std::vector<Vec3> FlatFace(double x0, double y0, double x1, double y1, double z = 0.0)
    {
        return { { x0, y0, z }, { x1, y0, z }, { x1, y1, z }, { x0, y0, z }, { x1, y1, z }, { x0, y1, z } };
    }

    double DistanceToRectangle(const Vec3& p, double x0, double y0, double x1, double y1)
    {
        return (std::min)({ p.x - x0, x1 - p.x, p.y - y0, y1 - p.y });
    }
}

TEST_CASE("plunges pack inside the area, clear of its wall, and cover it")
{
    PlungeRoughingSettings settings;
    settings.cutAngle = 90;
    const double reach = 3.0 + 0.5;
    const double pitch = 4.5;
    const PlungeArea area = RectangleArea();
    const std::vector<std::vector<Vec3>> rows = PlungeRows(area, reach, pitch, 6.0, settings);
    REQUIRE(rows.size() >= 2);

    std::vector<Vec3> all;
    for (size_t r = 0; r < rows.size(); ++r)
    {
        for (size_t i = 0; i < rows[r].size(); ++i)
        {
            const Vec3& p = rows[r][i];
            CHECK(DistanceToRectangle(p, 0, 0, 40, 30) >= reach - 1e-6);
            if (i > 0)
            {
                const double apart = std::fabs(p.x - rows[r][i - 1].x);
                CHECK(apart <= pitch + 1e-6);
                CHECK(apart > 0.1);
            }
            all.push_back(p);
        }
        if (r > 0)
            CHECK(std::fabs(rows[r][0].y - rows[r - 1][0].y) <= pitch * std::sqrt(3.0) / 2.0 + 1e-6);
    }

    // Every point the tool's centre can reach is cut: within a tool radius of a
    // plunge, give or take the grid's cells at the edge.
    for (double x = reach; x <= 40 - reach + 1e-9; x += 0.25)
    {
        for (double y = reach; y <= 30 - reach + 1e-9; y += 0.25)
        {
            double nearest = 1e9;
            for (const Vec3& p : all)
                nearest = (std::min)(nearest, std::hypot(p.x - x, p.y - y));
            CHECK(nearest <= 3.0 + 2.5 * area.Cell());
        }
    }
}

TEST_CASE("rows run along the angle, from the start side, zig or zigzag")
{
    PlungeRoughingSettings settings;
    settings.cutAngle = 90;
    settings.start = PlungeStartSide::Bottom;
    settings.direction = PlungePathDirection::ZigZag;
    std::vector<std::vector<Vec3>> rows = PlungeRows(RectangleArea(), 3.5, 4.5, 6.0, settings);
    REQUIRE(rows.size() >= 3);
    for (const auto& row : rows)
    {
        for (const Vec3& p : row)
            CHECK(p.y == doctest::Approx(row.front().y));
    }
    CHECK(rows[0][0].y < rows[1][0].y);                         // from the bottom up
    CHECK(rows[0].front().x < rows[0].back().x);                // the first row one way
    CHECK(rows[1].front().x > rows[1].back().x);                // the next back

    settings.direction = PlungePathDirection::Zig;
    rows = PlungeRows(RectangleArea(), 3.5, 4.5, 6.0, settings);
    CHECK(rows[1].front().x < rows[1].back().x);

    settings.start = PlungeStartSide::Top;
    rows = PlungeRows(RectangleArea(), 3.5, 4.5, 6.0, settings);
    CHECK(rows[0][0].y > rows[1][0].y);                         // from the top down

    // An angle of 0 runs the rows along Y.
    settings.cutAngle = 0;
    rows = PlungeRows(RectangleArea(), 3.5, 4.5, 6.0, settings);
    for (const Vec3& p : rows[0])
        CHECK(p.x == doctest::Approx(rows[0].front().x));
}

TEST_CASE("each plunge stops the stock to leave above the parts under the whole tool")
{
    const SequenceStep step = PlungeStep();
    const Tool tool = Mill(6);
    PlungeRoughToolpathInput input = Input(step, tool);
    // A floor at 4, and a block rising to 7 over the right half.
    input.partTriangles = Box({ 0, 0, 0 }, { 40, 30, 4 });
    const std::vector<Vec3> block = Box({ 20, 0, 4 }, { 40, 30, 7 });
    input.partTriangles.insert(input.partTriangles.end(), block.begin(), block.end());

    const ToolpathResult result = GeneratePlungeRoughToolpath(input);
    REQUIRE(result.ok);
    const std::vector<Vec3> bottoms = Bottoms(result.toolpath);
    REQUIRE_FALSE(bottoms.empty());
    int low = 0, high = 0;
    for (const Vec3& p : bottoms)
    {
        if (p.x < 20 - 3.5 - 0.5)
        {
            CHECK(p.z == doctest::Approx(4.5));
            ++low;
        }
        else if (p.x > 20 - 3.5 + 0.05)
        {
            // The tool reaches over the block, so it stops above it.
            CHECK(p.z == doctest::Approx(7.5));
            ++high;
        }
    }
    CHECK(low > 0);
    CHECK(high > 0);
}

TEST_CASE("a ball mill stops with its tip the stock to leave above a flat floor")
{
    const SequenceStep step = PlungeStep();
    const Tool tool = Mill(6, ToolType::BallMill);
    PlungeRoughToolpathInput input = Input(step, tool);
    input.partTriangles = Box({ 0, 0, 0 }, { 40, 30, 4 });
    const ToolpathResult result = GeneratePlungeRoughToolpath(input);
    REQUIRE(result.ok);
    for (const Vec3& p : Bottoms(result.toolpath))
        CHECK(p.z == doctest::Approx(4.5).epsilon(0.002));
}

TEST_CASE("without a part under it, a plunge goes to the stock's bottom, and moves go by the approach height")
{
    const SequenceStep step = PlungeStep();
    const Tool tool = Mill(6);
    const ToolpathResult result = GeneratePlungeRoughToolpath(Input(step, tool));
    REQUIRE(result.ok);
    const std::vector<ToolpathMove>& moves = result.toolpath.moves;
    REQUIRE(moves.size() >= 5);

    // Down from the clearance plane to 2 above the stock, plunge, back up, ... and up at the end.
    CHECK(moves[0].kind == MoveKind::Rapid);
    CHECK(moves[0].to.z == doctest::Approx(20));
    CHECK(moves[1].kind == MoveKind::Rapid);
    CHECK(moves[1].to.z == doctest::Approx(12));
    CHECK(moves[2].kind == MoveKind::Feed);
    CHECK(moves[2].feed == 150);
    CHECK(moves[2].to.z == doctest::Approx(0));
    CHECK(moves[3].kind == MoveKind::Feed);
    CHECK(moves[3].feed == 900);
    CHECK(moves[3].to.z == doctest::Approx(12));
    CHECK(moves[4].kind == MoveKind::Rapid);
    CHECK(moves[4].to.z == doctest::Approx(12));
    CHECK(moves.back().kind == MoveKind::Rapid);
    CHECK(moves.back().to.z == doctest::Approx(20));
}

TEST_CASE("plunge roughing refuses what it can't do")
{
    SequenceStep step = PlungeStep();
    const Tool tool = Mill(6);

    PlungeRoughToolpathInput input = Input(step, tool);
    input.tool = nullptr;
    CHECK_FALSE(GeneratePlungeRoughToolpath(input).ok);

    input = Input(step, tool);
    input.outlines = { Rectangle(0, 0, 5, 5) };     // narrower than the tool and the stock to leave
    CHECK_FALSE(GeneratePlungeRoughToolpath(input).ok);

    input = Input(step, tool);
    input.outlines[0].closed = false;
    CHECK_FALSE(GeneratePlungeRoughToolpath(input).ok);

    input = Input(step, tool);
    input.partTriangles = Box({ 0, 0, 0 }, { 40, 30, 10 });     // the part fills the stock
    CHECK_FALSE(GeneratePlungeRoughToolpath(input).ok);

    step.plungeRoughing.stepover = { false, 0.0, 0.0 };
    input = Input(step, tool);
    CHECK_FALSE(GeneratePlungeRoughToolpath(input).ok);

    step = PlungeStep();
    step.plungeRoughing.approach = 15;                          // above the clearance plane
    input = Input(step, tool);
    CHECK_FALSE(GeneratePlungeRoughToolpath(input).ok);

    CHECK_FALSE(ValidatePlungeRoughing({ -1.0 }).empty());
}

TEST_CASE("plunges at least a tool apart never overlap, ends included")
{
    // Frank's example: a 55 mm square, a 3 mm tool, 125% stepover.
    PlungeArea area;
    area.Reset(0, 0, 55, 55, 3.0 / 40.0);
    area.AddLoop(Rectangle(0, 0, 55, 55).path);
    area.Finish();
    for (double percent : { 100.0, 110.0, 125.0, 150.0, 200.0 })
    {
        PlungeRoughingSettings settings;
        settings.stockToLeave = 0.5;
        for (double angle : { 0.0, 90.0, 30.0 })
        {
            settings.cutAngle = angle;
            const double pitch = 3.0 * percent / 100.0;
            const std::vector<std::vector<Vec3>> rows = PlungeRows(area, 1.5 + 0.5, pitch, 3.0, settings);
            std::vector<Vec3> all;
            for (const auto& row : rows)
                all.insert(all.end(), row.begin(), row.end());
            REQUIRE(all.size() > 10);
            double closest = 1e9;
            for (size_t i = 0; i < all.size(); ++i)
            {
                for (size_t j = i + 1; j < all.size(); ++j)
                    closest = (std::min)(closest, std::hypot(all[i].x - all[j].x, all[i].y - all[j].y));
            }
            INFO("stepover " << percent << "%, angle " << angle);
            CHECK(closest >= 3.0 - 1e-6);
            // Neighbours along a row are exactly the stepover apart, or at least a tool apart at the ends.
            for (const auto& row : rows)
            {
                for (size_t i = 1; i < row.size(); ++i)
                    CHECK(std::hypot(row[i].x - row[i - 1].x, row[i].y - row[i - 1].y) >= 3.0 - 1e-6);
            }
        }
    }
}

TEST_CASE("the stepover has no maximum: plunges wider apart than the tool leave plugs")
{
    SequenceStep step = PlungeStep();
    const Tool tool = Mill(6);
    step.plungeRoughing.stepover = { true, 250.0, 0.0 };       // 15 mm apart
    CHECK(ValidatePlungeRoughing(step.plungeRoughing).empty());
    const ToolpathResult result = GeneratePlungeRoughToolpath(Input(step, tool));
    REQUIRE(result.ok);
    const std::vector<Vec3> bottoms = Bottoms(result.toolpath);
    REQUIRE(bottoms.size() >= 2);
    CHECK(std::hypot(bottoms[1].x - bottoms[0].x, bottoms[1].y - bottoms[0].y) > 6.0);

    step.plungeRoughing.stepover = { false, 0.0, 20.0 };
    CHECK(ValidatePlungeRoughing(step.plungeRoughing).empty());
    CHECK(GeneratePlungeRoughToolpath(Input(step, tool)).ok);
}

TEST_CASE("faces are projected from above, and shapes that touch merge into one area")
{
    const SequenceStep step = PlungeStep();
    const Tool tool = Mill(6);

    // Two faces side by side, at different heights, make one 40 x 30 area.
    PlungeRoughToolpathInput input = Input(step, tool);
    input.outlines.clear();
    input.faces = { FlatFace(0, 0, 20, 30, 3), FlatFace(20, 0, 40, 30, 5) };
    ToolpathResult merged = GeneratePlungeRoughToolpath(input);
    REQUIRE(merged.ok);
    bool acrossTheJoin = false;
    for (const Vec3& p : Bottoms(merged.toolpath))
        acrossTheJoin = acrossTheJoin || std::fabs(p.x - 20) < 2.0;
    CHECK(acrossTheJoin);       // separately, nothing could plunge within 3.5 of x = 20

    // The same as one loop round the lot.
    const ToolpathResult loop = GeneratePlungeRoughToolpath(Input(step, tool));
    REQUIRE(loop.ok);
    CHECK(static_cast<double>(Bottoms(merged.toolpath).size()) ==
          doctest::Approx(static_cast<double>(Bottoms(loop.toolpath).size())).epsilon(0.1));

    // A face and a loop that overlap merge too.
    input.faces = { FlatFace(0, 0, 25, 30) };
    input.outlines = { Rectangle(15, 0, 40, 30) };
    merged = GeneratePlungeRoughToolpath(input);
    REQUIRE(merged.ok);
    acrossTheJoin = false;
    for (const Vec3& p : Bottoms(merged.toolpath))
        acrossTheJoin = acrossTheJoin || (p.x > 22 && p.x < 28);
    CHECK(acrossTheJoin);
}

TEST_CASE("a face's holes are filled, and a face seen edge on covers nothing")
{
    const SequenceStep step = PlungeStep();
    const Tool tool = Mill(6);

    // A 40 x 30 frame with a 20 x 14 hole in the middle, as four strips.
    PlungeRoughToolpathInput input = Input(step, tool);
    input.outlines.clear();
    std::vector<Vec3> ring;
    for (const auto& strip : { FlatFace(0, 0, 40, 8), FlatFace(0, 22, 40, 30), FlatFace(0, 8, 10, 22),
                               FlatFace(30, 8, 40, 22) })
        ring.insert(ring.end(), strip.begin(), strip.end());
    input.faces = { ring };
    const ToolpathResult result = GeneratePlungeRoughToolpath(input);
    REQUIRE(result.ok);
    bool inHole = false;
    for (const Vec3& p : Bottoms(result.toolpath))
        inHole = inHole || (p.x > 14 && p.x < 26 && p.y > 12 && p.y < 18);
    CHECK(inHole);

    // An upright face has no area from above.
    input.faces = { { { 0, 0, 0 }, { 40, 0, 0 }, { 40, 0, 10 }, { 0, 0, 0 }, { 40, 0, 10 }, { 0, 0, 10 } } };
    CHECK_FALSE(GeneratePlungeRoughToolpath(input).ok);
}

TEST_CASE("plunge roughing takes any face, and edges; facing still takes only flat faces")
{
    for (PickKind kind : { PickKind::PlanarFace, PickKind::CylindricalFace, PickKind::SurfaceFace, PickKind::LineEdge,
                           PickKind::CircleEdge, PickKind::CurveEdge })
        CHECK(FeatureAllowed(FeatureFilter::ProjectedArea, kind));
    CHECK_FALSE(FeatureAllowed(FeatureFilter::ProjectedArea, PickKind::DesignPoint));
    CHECK_FALSE(FeatureAllowed(FeatureFilter::FacingArea, PickKind::SurfaceFace));
    CHECK_FALSE(FeatureAllowed(FeatureFilter::FacingArea, PickKind::CylindricalFace));
}

TEST_CASE("at an open edge the tool's centre goes up to it; where parts carry on, the tool stays inside")
{
    const SequenceStep step = PlungeStep();
    const Tool tool = Mill(6);
    const double reach = 3.0 + 0.5;
    const auto extremes = [](const Toolpath& toolpath, double& xLow, double& xHigh, double& yLow, double& yHigh) {
        const std::vector<Vec3> bottoms = Bottoms(toolpath);
        REQUIRE_FALSE(bottoms.empty());
        xLow = xHigh = bottoms[0].x;
        yLow = yHigh = bottoms[0].y;
        for (const Vec3& p : bottoms)
        {
            xLow = (std::min)(xLow, p.x);
            xHigh = (std::max)(xHigh, p.x);
            yLow = (std::min)(yLow, p.y);
            yHigh = (std::max)(yHigh, p.y);
        }
    };
    double xLow = 0, xHigh = 0, yLow = 0, yHigh = 0;

    // The area is the part's whole footprint: nothing beyond its edges.
    PlungeRoughToolpathInput input = Input(step, tool);
    input.partTriangles = Box({ 0, 0, 0 }, { 40, 30, 4 });
    ToolpathResult result = GeneratePlungeRoughToolpath(input);
    REQUIRE(result.ok);
    extremes(result.toolpath, xLow, xHigh, yLow, yHigh);
    CHECK(xLow < 1.0);
    CHECK(xLow >= -1e-6);
    CHECK(xHigh > 39.0);
    CHECK(xHigh <= 40 + 1e-6);
    CHECK(yLow < 1.0);
    CHECK(yHigh > 29.0);

    // A pocket: walls all round, outside the area. The tool stays inside.
    input.partTriangles = Box({ 0, 0, 0 }, { 40, 30, 2 });
    for (const auto& wall : { Box({ -10, -10, 0 }, { 0, 40, 12 }), Box({ 40, -10, 0 }, { 50, 40, 12 }),
                              Box({ 0, -10, 0 }, { 40, 0, 12 }), Box({ 0, 30, 0 }, { 40, 40, 12 }) })
        input.partTriangles.insert(input.partTriangles.end(), wall.begin(), wall.end());
    result = GeneratePlungeRoughToolpath(input);
    REQUIRE(result.ok);
    extremes(result.toolpath, xLow, xHigh, yLow, yHigh);
    CHECK(xLow >= reach - 1e-6);
    CHECK(xHigh <= 40 - reach + 1e-6);
    CHECK(yLow >= reach - 1e-6);
    CHECK(yHigh <= 30 - reach + 1e-6);

    // One wall only, on the -X side: that edge is closed, the rest open.
    input.partTriangles = Box({ 0, 0, 0 }, { 40, 30, 2 });
    const std::vector<Vec3> wall = Box({ -10, -10, 0 }, { 0, 40, 12 });
    input.partTriangles.insert(input.partTriangles.end(), wall.begin(), wall.end());
    result = GeneratePlungeRoughToolpath(input);
    REQUIRE(result.ok);
    extremes(result.toolpath, xLow, xHigh, yLow, yHigh);
    CHECK(xLow >= reach - 1e-6);
    CHECK(xHigh > 39.0);

    // Without the parts every edge is closed.
    input.partTriangles.clear();
    result = GeneratePlungeRoughToolpath(input);
    REQUIRE(result.ok);
    extremes(result.toolpath, xLow, xHigh, yLow, yHigh);
    CHECK(xLow >= reach - 1e-6);
    CHECK(yHigh <= 30 - reach + 1e-6);
}

TEST_CASE("the stock's footprint as the area: plunges reach the stock's edges")
{
    const SequenceStep step = PlungeStep();
    const Tool tool = Mill(6);
    PlungeRoughToolpathInput input = Input(step, tool);
    ProfileChain footprint;
    footprint.ok = true;
    footprint.closed = true;
    footprint.path = StockFootprint(Frame(), { { 0, 0, 0 }, { 40, 30, 10 } });
    input.outlines = { footprint };
    input.partTriangles = Box({ 10, 8, 0 }, { 30, 22, 6 });     // a part inside the stock
    const ToolpathResult result = GeneratePlungeRoughToolpath(input);
    REQUIRE(result.ok);
    double xLow = 1e9, xHigh = -1e9;
    bool overPart = false;
    for (const Vec3& p : Bottoms(result.toolpath))
    {
        xLow = (std::min)(xLow, p.x);
        xHigh = (std::max)(xHigh, p.x);
        if (p.x > 14 && p.x < 26 && p.y > 12 && p.y < 18)
        {
            overPart = true;
            CHECK(p.z == doctest::Approx(6.5));
        }
    }
    CHECK(xLow < 1.0);
    CHECK(xHigh > 39.0);
    CHECK(overPart);
}

TEST_CASE("an empty features list is fine with the stock's footprint")
{
    CHECK_FALSE(CheckAreaFeatures({}, "Facing").ok);
    CHECK(CheckAreaFeatures({}, "Facing", true).ok);
}

TEST_CASE("a plunge roughing operation saves its settings")
{
    CHECK(IsOperation(StepType::PlungeRoughing));
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "CAMForAlibre-plunge-test.cam";

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
    SequenceStep plunge = PlungeStep();
    plunge.id = NewStableId();
    plunge.name = "Plunge Roughing 1";
    plunge.plungeRoughing.direction = PlungePathDirection::Zig;
    plunge.plungeRoughing.start = PlungeStartSide::Top;
    plunge.plungeRoughing.cutAngle = 30;
    plunge.plungeRoughing.stepover = { false, 60.0, 3.25 };
    plunge.plungeRoughing.approach = 1.5;
    plunge.stockFootprint = true;
    PickedGeometry surface;
    surface.kind = PickKind::SurfaceFace;
    surface.occurrenceName = "Mould<1>";
    surface.point = { 1, 2, 3 };
    plunge.features.push_back(surface);
    group.children.push_back(plunge);
    job.sequence.push_back(group);
    REQUIRE(SaveJob(job, path).ok);

    const LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    const SequenceStep& back = loaded.job.sequence[1].children[0];
    CHECK(back.type == StepType::PlungeRoughing);
    const PlungeRoughingSettings& settings = back.plungeRoughing;
    CHECK(settings.stockToLeave == 0.5);
    CHECK(settings.direction == PlungePathDirection::Zig);
    CHECK(settings.start == PlungeStartSide::Top);
    CHECK(settings.cutAngle == 30);
    CHECK_FALSE(settings.stepover.usePercent);
    CHECK(settings.stepover.distance == 3.25);
    CHECK(settings.stepover.percent == 60.0);
    CHECK(settings.approach == 1.5);
    CHECK(SameSettings(plunge, back));
    CHECK(back.stockFootprint);
    SequenceStep without = back;
    without.stockFootprint = false;
    CHECK_FALSE(SameSettings(without, back));
    REQUIRE(back.features.size() == 1);
    CHECK(back.features[0].kind == PickKind::SurfaceFace);
    CHECK(back.features[0].point.z == 3);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}
