// SimulationTests.cpp : tests for the voxel stock, tool shapes, cutting, meshing and stepping.

#include <doctest/doctest.h>

#include <cmath>

#include "Simulation.h"

using namespace camcore;

namespace
{
    // A 20 x 20 x 10 box from the origin, in 0.5 mm cells.
    VoxelStock Block(double cell = 0.5)
    {
        VoxelStock stock;
        stock.Reset({ { 0, 0, 0 }, { 20, 20, 10 } }, cell);
        stock.FillAll();
        return stock;
    }

    // The cell containing a point.
    bool FilledAt(const VoxelStock& stock, double x, double y, double z)
    {
        const int i = static_cast<int>(std::floor((x - stock.Lower().x) / stock.Cell()));
        const int j = static_cast<int>(std::floor((y - stock.Lower().y) / stock.Cell()));
        const int k = static_cast<int>(std::floor((z - stock.Lower().z) / stock.Cell()));
        return stock.Filled(i, j, k);
    }

    ToolProfile Flat(double diameter)
    {
        Tool tool;
        tool.diameter = diameter;
        tool.fluteLength = 20;
        tool.overallLength = 50;
        return ProfileOf(tool);
    }

    // The 12 triangles of a box.
    std::vector<Vec3> BoxMesh(const Vec3& a, const Vec3& b)
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

    const Vec3 kUp{ 0, 0, 1 };
}

TEST_CASE("the cell size keeps the grid within the budget")
{
    const StockBox box{ { 0, 0, 0 }, { 300, 200, 50 } };
    CHECK(ChooseCellSize(box, 2.0e8) == doctest::Approx(0.25));     // 1200 x 800 x 200
    CHECK(ChooseCellSize(box, 1.0e6) == doctest::Approx(2));        // 150 x 100 x 25
    CHECK(ChooseCellSize({ { 0, 0, 0 }, { 10, 10, 10 } }, 2.0e8) == doctest::Approx(0.1));
}

TEST_CASE("a box fills completely, and a box mesh fills the same cells")
{
    const VoxelStock full = Block();
    CHECK(full.Count() == 40LL * 40 * 20);

    VoxelStock meshed;
    meshed.Reset({ { 0, 0, 0 }, { 20, 20, 10 } }, 0.5);
    meshed.FillMesh(BoxMesh({ 0, 0, 0 }, { 20, 20, 10 }));
    CHECK(meshed.Count() == full.Count());

    // A smaller box inside the grid fills only itself.
    meshed.FillMesh(BoxMesh({ 5, 5, 0 }, { 10, 10, 5 }));
    CHECK(meshed.Count() == 10LL * 10 * 10);
}

TEST_CASE("each tool type's radius up from its tip")
{
    Tool tool;
    tool.diameter = 10;
    tool.overallLength = 40;

    tool.type = ToolType::FlatEndMill;
    CHECK(ProfileOf(tool).RadiusAt(0) == doctest::Approx(5));
    CHECK(ProfileOf(tool).RadiusAt(-0.1) < 0);
    CHECK(ProfileOf(tool).RadiusAt(41) < 0);

    tool.type = ToolType::BallMill;
    CHECK(ProfileOf(tool).RadiusAt(0) == doctest::Approx(0));
    CHECK(ProfileOf(tool).RadiusAt(5) == doctest::Approx(5));
    CHECK(ProfileOf(tool).RadiusAt(1) == doctest::Approx(3));       // sqrt(25 - 16)

    tool.type = ToolType::CornerRadiusMill;
    tool.cornerRadius = 1;
    CHECK(ProfileOf(tool).RadiusAt(0) == doctest::Approx(4));
    CHECK(ProfileOf(tool).RadiusAt(1) == doctest::Approx(5));

    tool.type = ToolType::VMill;
    tool.includedAngle = 90;
    tool.tipDiameter = 0;
    CHECK(ProfileOf(tool).RadiusAt(2) == doctest::Approx(2));
    CHECK(ProfileOf(tool).RadiusAt(8) == doctest::Approx(5));       // no wider than the tool

    tool.type = ToolType::Drill;
    tool.pointAngle = 90;
    CHECK(ProfileOf(tool).RadiusAt(3) == doctest::Approx(3));
}

TEST_CASE("a flat end mill plunges a round hole of its radius and depth")
{
    VoxelStock stock = Block();
    const long long removed = CutMove(stock, Flat(6), { 10, 10, 20 }, { 10, 10, 6 }, kUp);
    CHECK(removed > 0);
    CHECK_FALSE(FilledAt(stock, 10, 10, 9.9));
    CHECK_FALSE(FilledAt(stock, 12.6, 10, 8));      // inside the 3 mm radius
    CHECK(FilledAt(stock, 13.6, 10, 8));            // outside it
    CHECK(FilledAt(stock, 10, 10, 5.6));            // below the tip
    CHECK_FALSE(FilledAt(stock, 10, 10, 6.4));
}

TEST_CASE("a flat end mill slot is its width and depth")
{
    VoxelStock stock = Block();
    CutMove(stock, Flat(4), { 2, 10, 8 }, { 18, 10, 8 }, kUp);
    CHECK_FALSE(FilledAt(stock, 10, 11.6, 9));
    CHECK(FilledAt(stock, 10, 12.6, 9));
    CHECK(FilledAt(stock, 10, 10, 7.6));
    CHECK_FALSE(FilledAt(stock, 10, 10, 8.4));
}

TEST_CASE("a ball mill slot has a round bottom, and a V-mill a V")
{
    Tool ball;
    ball.type = ToolType::BallMill;
    ball.diameter = 6;
    ball.overallLength = 40;
    VoxelStock stock = Block();
    CutMove(stock, ProfileOf(ball), { 2, 10, 7 }, { 18, 10, 7 }, kUp);
    CHECK_FALSE(FilledAt(stock, 10, 10, 7.3));      // the bottom of the round
    CHECK(FilledAt(stock, 10, 12.4, 7.3));          // beside it, below the round
    CHECK_FALSE(FilledAt(stock, 10, 12.4, 9.8));    // higher up, it's 3 wide

    Tool v;
    v.type = ToolType::VMill;
    v.diameter = 10;
    v.includedAngle = 90;
    v.overallLength = 40;
    VoxelStock vee = Block();
    CutMove(vee, ProfileOf(v), { 2, 10, 6 }, { 18, 10, 6 }, kUp);
    // At 2 above the tip the V is 2 wide each side; at 1, 1.
    CHECK_FALSE(FilledAt(vee, 10, 11.6, 8.1));
    CHECK(FilledAt(vee, 10, 11.6, 6.9));
}

TEST_CASE("a tool pointing along X cuts sideways")
{
    VoxelStock stock = Block();
    // The tip goes from outside the +X face 5 mm in, the tool along +X.
    CutMove(stock, Flat(4), { 25, 10, 5 }, { 15, 10, 5 }, { 1, 0, 0 });
    CHECK_FALSE(FilledAt(stock, 17, 10, 5));
    CHECK(FilledAt(stock, 14.6, 10, 5));
    CHECK(FilledAt(stock, 17, 10, 7.6));
}

TEST_CASE("a full box meshes as 6 merged faces, and one notch changes only its chunk")
{
    VoxelStock stock = Block(0.5);      // 40 x 40 x 20 cells: 2 x 2 x 1 chunks
    size_t triangles = 0;
    for (int c = 0; c < stock.ChunkCount(); ++c)
    {
        triangles += MeshChunk(stock, c).TriangleCount();
        stock.SetChunkClean(c);
    }
    // Each chunk shows its outside faces: 4 chunks x (top, bottom, 2 sides) x 2 triangles.
    CHECK(triangles == 4 * 4 * 2);

    stock.Clear(1, 1, 19);
    int dirty = 0;
    for (int c = 0; c < stock.ChunkCount(); ++c)
        dirty += stock.ChunkDirty(c) ? 1 : 0;
    CHECK(dirty == 1);
    CHECK(MeshChunk(stock, 0).TriangleCount() > 8);
}

namespace
{
    SimOperation Operation(const ToolProfile& profile, std::vector<ToolpathMove> moves)
    {
        SimOperation operation;
        operation.name = "Op";
        operation.profile = profile;
        operation.toolpath.moves = std::move(moves);
        return operation;
    }

    ToolpathMove Move(MoveKind kind, Vec3 to)
    {
        ToolpathMove move;
        move.kind = kind;
        move.to = to;
        move.feed = 100;
        return move;
    }
}

TEST_CASE("stepping cuts move by move, and seeking matches stepping")
{
    const std::vector<ToolpathMove> moves = {
        Move(MoveKind::Rapid, { 5, 10, 15 }), Move(MoveKind::Feed, { 5, 10, 8 }),
        Move(MoveKind::Feed, { 15, 10, 8 }), Move(MoveKind::Rapid, { 15, 10, 15 }),
    };
    Simulation simulation;
    simulation.Start({ { 0, 0, 0 }, { 20, 20, 10 } }, {}, { Operation(Flat(4), moves) }, 1.0e5);
    REQUIRE(simulation.Moves().size() == 4);
    const long long whole = simulation.Stock().Count();

    CHECK(simulation.Step(2) == 2);
    const long long afterPlunge = simulation.Stock().Count();
    CHECK(afterPlunge < whole);
    simulation.Step(10);
    CHECK(simulation.Finished());
    CHECK_FALSE(simulation.HasWarning());
    const long long done = simulation.Stock().Count();
    CHECK(done < afterPlunge);

    simulation.SeekTo(2);
    CHECK(simulation.Position() == 2);
    CHECK(simulation.Stock().Count() == afterPlunge);
    simulation.Rewind();
    CHECK(simulation.Stock().Count() == whole);
    simulation.SeekTo(4);
    CHECK(simulation.Stock().Count() == done);
}

TEST_CASE("a rapid through stock stops with a warning")
{
    const std::vector<ToolpathMove> moves = {
        Move(MoveKind::Rapid, { 5, 10, 15 }), Move(MoveKind::Rapid, { 5, 10, 5 }),
        Move(MoveKind::Feed, { 15, 10, 5 }),
    };
    Simulation simulation;
    simulation.Start({ { 0, 0, 0 }, { 20, 20, 10 } }, {}, { Operation(Flat(4), moves) }, 1.0e5);
    CHECK(simulation.RunToEnd() == 2);
    CHECK(simulation.HasWarning());
    CHECK(simulation.WarningMove() == 1);
}

TEST_CASE("arcs become chords, and operations run in their own frames")
{
    ToolpathMove arc = Move(MoveKind::Feed, { 15, 10, 8 });
    arc.arc = true;
    arc.centre = { 10, 10, 8 };
    const std::vector<ToolpathMove> moves = { Move(MoveKind::Rapid, { 5, 10, 15 }), Move(MoveKind::Feed, { 5, 10, 8 }),
                                              arc };

    SimOperation operation = Operation(Flat(2), moves);
    Simulation simulation;
    simulation.Start({ { 0, 0, 0 }, { 20, 20, 10 } }, {}, { operation }, 1.0e5);
    CHECK(simulation.Moves().size() > 5);       // the half circle in many chords
    simulation.RunToEnd();
    CHECK_FALSE(simulation.HasWarning());
    // Anticlockwise from (5, 10) about (10, 10) goes through (10, 5).
    CHECK_FALSE(FilledAt(simulation.Stock(), 10, 5, 9));
    CHECK(FilledAt(simulation.Stock(), 10, 15, 9));

    // The same in a frame moved 2 up: the cut is 2 higher.
    operation.frame.origin = { 0, 0, 2 };
    simulation.Start({ { 0, 0, 0 }, { 20, 20, 10 } }, {}, { operation }, 1.0e5);
    simulation.RunToEnd();
    CHECK(FilledAt(simulation.Stock(), 10, 5, 9));
}
