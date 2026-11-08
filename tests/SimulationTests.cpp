// SimulationTests.cpp : tests for the voxel stock, tool shapes, cutting, meshing and stepping.

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstring>

#include "Simulation.h"

using namespace camcore;

namespace
{
    // A 20 x 20 x 10 box from the origin, in 0.5 mm cells.
    VoxelStock Block(double cell = 0.5, bool divide = true)
    {
        VoxelStock stock;
        stock.Reset({ { 0, 0, 0 }, { 20, 20, 10 } }, cell, divide);
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

TEST_CASE("the resolution stops keep the grid within the budget, and auto divide goes finer")
{
    const StockBox box{ { 0, 0, 0 }, { 300, 200, 50 } };
    const std::vector<double> stops = ResolutionStops(box, 2.0e8);
    REQUIRE(stops.size() == 7);
    CHECK(stops.front() == doctest::Approx(0.25));      // 1200 x 800 x 200
    CHECK(stops.back() == doctest::Approx(10));
    CHECK(ResolutionStops(box, 1.0e6).front() == doctest::Approx(2));    // 150 x 100 x 25
    CHECK(AutoDivideCell(box) == doctest::Approx(0.1));             // 3000 x 2000 columns
    CHECK(AutoDivideCell({ { 0, 0, 0 }, { 136, 150, 6 } }) == doctest::Approx(0.05));
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
    simulation.Start({ { 0, 0, 0 }, { 20, 20, 10 } }, {}, { Operation(Flat(4), moves) }, SimResolution{ true, 0.5 });
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
    simulation.Start({ { 0, 0, 0 }, { 20, 20, 10 } }, {}, { Operation(Flat(4), moves) }, SimResolution{ true, 0.5 });
    CHECK(simulation.RunToEnd() == 2);
    CHECK(simulation.HasWarning());
    CHECK(simulation.WarningMove() == 1);
}

TEST_CASE("the shank and holder touching stock stop with a warning")
{
    // Feeding 12 deep into stock 10 tall: the 4 mm flutes are under the top, the
    // shank above them is in the stock.
    const std::vector<ToolpathMove> moves = {
        Move(MoveKind::Rapid, { 5, 10, 15 }), Move(MoveKind::Feed, { 5, 10, -2 }),
        Move(MoveKind::Feed, { 15, 10, -2 }),
    };
    Tool tool;
    tool.diameter = 4.0;
    tool.fluteLength = 4.0;
    tool.overallLength = 30.0;
    SimOperation operation = Operation(Flat(4), moves);
    operation.profile = ProfileOf(tool);
    operation.shank = ShankOf(tool);
    operation.holder = HolderOf(tool, 20.0, 30.0);
    CHECK(operation.shank.start == 4.0);
    CHECK(operation.shank.length == 26.0);
    CHECK(operation.holder.start == 30.0);
    CHECK(operation.holder.radius == 10.0);

    Simulation simulation;
    simulation.Start({ { 0, 0, 0 }, { 20, 20, 10 } }, {}, { operation }, SimResolution{ true, 0.5 });
    CHECK(simulation.RunToEnd() == 2);
    CHECK(simulation.Warning() == SimWarning::ShankHitsStock);
    CHECK(simulation.WarningMove() == 1);
    // It carries on from there when asked.
    simulation.ClearWarning();
    CHECK(simulation.RunToEnd() == 1);
    CHECK(simulation.Finished());

    // A short tool: the holder reaches the stock first.
    tool.overallLength = 8.0;
    operation.profile = ProfileOf(tool);
    operation.shank = ShankOf(tool);
    operation.holder = HolderOf(tool, 20.0, 30.0);
    simulation.Start({ { 0, 0, 0 }, { 20, 20, 10 } }, {}, { operation }, SimResolution{ true, 0.5 });
    simulation.RunToEnd();
    CHECK(simulation.Warning() == SimWarning::HolderHitsStock);

    // Cutting within the flutes is fine.
    const std::vector<ToolpathMove> shallow = {
        Move(MoveKind::Rapid, { 5, 10, 15 }), Move(MoveKind::Feed, { 5, 10, 7 }),
        Move(MoveKind::Feed, { 15, 10, 7 }), Move(MoveKind::Rapid, { 15, 10, 50 }),
    };
    tool.overallLength = 30.0;
    SimOperation fine = Operation(Flat(4), shallow);
    fine.profile = ProfileOf(tool);
    fine.shank = ShankOf(tool);
    fine.holder = HolderOf(tool, 20.0, 30.0);
    simulation.Start({ { 0, 0, 0 }, { 20, 20, 10 } }, {}, { fine }, SimResolution{ true, 0.5 });
    simulation.RunToEnd();
    CHECK(simulation.Finished());
    CHECK_FALSE(simulation.HasWarning());
}

TEST_CASE("a reduced neck tool's neck and wide shank")
{
    // A 2 mm end mill with 3 mm flutes, its 2 mm neck 12 long, then a 6 mm shank
    // up to 40.
    Tool tool;
    tool.id = "neck";
    tool.name = "2 mm long reach";
    tool.diameter = 2.0;
    tool.fluteLength = 3.0;
    tool.overallLength = 40.0;
    tool.reducedNeck = true;
    tool.neckLength = 12.0;
    tool.shankDiameter = 6.0;
    CHECK(ValidateTool(tool).empty());
    const SimCylinder neck = NeckOf(tool);
    const SimCylinder shank = ShankOf(tool);
    CHECK(neck.start == 3.0);
    CHECK(neck.radius == 1.0);
    CHECK(neck.length == 9.0);
    CHECK(shank.start == 12.0);
    CHECK(shank.radius == 3.0);
    CHECK(shank.length == 28.0);

    // Without the reduced neck: no neck, and the shank is the tool's diameter.
    Tool plain = tool;
    plain.reducedNeck = false;
    CHECK_FALSE(NeckOf(plain).Present());
    CHECK(ShankOf(plain).start == 3.0);
    CHECK(ShankOf(plain).radius == 1.0);

    // A neck as long as the flutes: the wide shank starts at the flutes.
    Tool shortNeck = tool;
    shortNeck.neckLength = 3.0;
    CHECK(ValidateTool(shortNeck).empty());
    CHECK_FALSE(NeckOf(shortNeck).Present());
    CHECK(ShankOf(shortNeck).start == 3.0);
    CHECK(ShankOf(shortNeck).radius == 3.0);

    // The checks.
    Tool bad = tool;
    bad.neckLength = 2.0;
    CHECK(ValidateTool(bad).size() == 1);
    bad.neckLength = 41.0;
    CHECK(ValidateTool(bad).size() == 1);
    bad = tool;
    bad.shankDiameter = 1.5;
    CHECK(ValidateTool(bad).size() == 1);

    // In inches, both are converted.
    Tool inches = tool;
    inches.unit = ToolUnit::Inches;
    const Tool mm = ToolInMillimetres(inches);
    CHECK(mm.neckLength == doctest::Approx(12.0 * 25.4));
    CHECK(mm.shankDiameter == doctest::Approx(6.0 * 25.4));

    // Slots into stock 10 tall: 2 deep stays within the flutes, 8 deep puts the
    // neck (3 to 12 above the tip) in the stock, 14 deep the wide shank too.
    const auto run = [&](double depth) {
        const std::vector<ToolpathMove> moves = {
            Move(MoveKind::Rapid, { 5, 10, 15 }), Move(MoveKind::Feed, { 5, 10, 10 - depth }),
            Move(MoveKind::Feed, { 15, 10, 10 - depth }),
        };
        SimOperation operation = Operation(Flat(2), moves);
        operation.profile = ProfileOf(tool);
        operation.neck = NeckOf(tool);
        operation.shank = ShankOf(tool);
        Simulation simulation;
        simulation.Start({ { 0, 0, 0 }, { 20, 20, 10 } }, {}, { operation }, SimResolution{ true, 0.25 });
        simulation.RunToEnd();
        return simulation.HasWarning() ? simulation.Warning() : SimWarning::None;
    };
    CHECK(run(2.0) == SimWarning::None);            // within the flutes
    CHECK(run(8.0) == SimWarning::NeckHitsStock);   // the neck in the stock
    CHECK(run(14.0) == SimWarning::ShankHitsStock); // the shank too, which is checked first
}

TEST_CASE("counting a move leaves the stock as it was")
{
    Simulation simulation;
    simulation.Start({ { 0, 0, 0 }, { 20, 20, 10 } }, {}, {}, SimResolution{ true, 0.5 });
    const long long whole = simulation.Stock().Count();
    ToolProfile profile = Flat(4);
    CHECK(CountMove(simulation.Stock(), profile, { 5, 10, 5 }, { 15, 10, 5 }, { 0, 0, 1 }) == 1);
    const long long all = CountMove(simulation.Stock(), profile, { 5, 10, 5 }, { 15, 10, 5 }, { 0, 0, 1 }, false);
    CHECK(simulation.Stock().Count() == whole);
    CHECK(CutMove(simulation.Stock(), profile, { 5, 10, 5 }, { 15, 10, 5 }, { 0, 0, 1 }) == all);
    CHECK(CountMove(simulation.Stock(), profile, { 5, 10, 20 }, { 15, 10, 20 }, { 0, 0, 1 }) == 0);
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
    simulation.Start({ { 0, 0, 0 }, { 20, 20, 10 } }, {}, { operation }, SimResolution{ true, 0.5 });
    CHECK(simulation.Moves().size() > 5);       // the half circle in many chords
    simulation.RunToEnd();
    CHECK_FALSE(simulation.HasWarning());
    // Anticlockwise from (5, 10) about (10, 10) goes through (10, 5).
    CHECK_FALSE(FilledAt(simulation.Stock(), 10, 5, 9));
    CHECK(FilledAt(simulation.Stock(), 10, 15, 9));

    // The same in a frame moved 2 up: the cut is 2 higher.
    operation.frame.origin = { 0, 0, 2 };
    simulation.Start({ { 0, 0, 0 }, { 20, 20, 10 } }, {}, { operation }, SimResolution{ true, 0.5 });
    simulation.RunToEnd();
    CHECK(FilledAt(simulation.Stock(), 10, 5, 9));
}

namespace
{
    bool SameCells(const VoxelStock& a, const VoxelStock& b)
    {
        if (a.SizeX() != b.SizeX() || a.SizeY() != b.SizeY() || a.SizeZ() != b.SizeZ())
            return false;
        for (int w = 0; w < a.WordsZ(); ++w)
        {
            for (int j = 0; j < a.SizeY(); ++j)
            {
                for (int i = 0; i < a.SizeX(); ++i)
                {
                    if (a.Word(i, j, w) != b.Word(i, j, w))
                        return false;
                }
            }
        }
        return true;
    }

    std::vector<ToolProfile> EveryToolType()
    {
        std::vector<ToolProfile> profiles;
        for (ToolType type : { ToolType::FlatEndMill, ToolType::BallMill, ToolType::CornerRadiusMill, ToolType::VMill,
                               ToolType::ChamferMill, ToolType::TaperMill, ToolType::ThreadMill, ToolType::Drill })
        {
            Tool tool;
            tool.type = type;
            tool.diameter = 6;
            tool.overallLength = 30;
            tool.cornerRadius = 1;
            tool.includedAngle = 90;
            tool.tipDiameter = 1;
            tool.taperAngle = 10;
            tool.pointAngle = 118;
            profiles.push_back(ProfileOf(tool));
        }
        return profiles;
    }

    // A few cuts: a plunge, a slot, a ramp, and a diagonal.
    void SomeCuts(VoxelStock& stock, const ToolProfile& profile, const Vec3& axis, bool generalOnly)
    {
        const double flip = axis.z;     // with the tool pointing down, the tip is at the bottom of the cut
        const double top = (flip > 0) ? 0.0 : 10.0;
        const auto z = [&](double depth) { return (flip > 0) ? 10.0 - depth : depth; };
        (void)top;
        CutMove(stock, profile, { 5, 5, z(-5) }, { 5, 5, z(3) }, axis, generalOnly);
        CutMove(stock, profile, { 3, 12, z(2) }, { 17, 12, z(2) }, axis, generalOnly);
        CutMove(stock, profile, { 4, 17, z(0) }, { 16, 17, z(4.3) }, axis, generalOnly);
        CutMove(stock, profile, { 12.3, 2.1, z(1.7) }, { 18.9, 9.4, z(1.7) }, axis, generalOnly);
    }
}

TEST_CASE("the height where a tool reaches a radius is the inverse of its radius")
{
    for (const ToolProfile& profile : EveryToolType())
    {
        for (double h : { 0.0, 0.3, 0.9, 1.7, 2.5, 4.0, 8.0 })
        {
            const double r = profile.RadiusAt(h);
            if (r <= 0.0)
                continue;
            const double back = profile.HeightForRadius(r);
            REQUIRE(back >= 0.0);
            CHECK(profile.RadiusAt(back) == doctest::Approx(r).epsilon(1e-6));
            CHECK(back <= h + 1e-9);
        }
        CHECK(profile.HeightForRadius(profile.radius + 0.1) < 0.0);
    }
}

TEST_CASE("the column at a time cut matches the cell by cell one, both ways up")
{
    for (const ToolProfile& profile : EveryToolType())
    {
        for (const Vec3& axis : { Vec3{ 0, 0, 1 }, Vec3{ 0, 0, -1 } })
        {
            VoxelStock fast = Block(0.25);
            VoxelStock general = Block(0.25);
            SomeCuts(fast, profile, axis, false);
            SomeCuts(general, profile, axis, true);
            CHECK(fast.Count() < Block(0.25).Count());
            CHECK(SameCells(fast, general));
        }
    }
}

TEST_CASE("divided and undivided stock cut the same")
{
    for (const ToolProfile& profile : EveryToolType())
    {
        VoxelStock divided = Block(0.25, true);
        VoxelStock dense = Block(0.25, false);
        SomeCuts(divided, profile, kUp, false);
        SomeCuts(dense, profile, kUp, false);
        CHECK(SameCells(divided, dense));
        CHECK(divided.Count() == dense.Count());

        // Sideways too.
        VoxelStock dividedX = Block(0.25, true);
        VoxelStock denseX = Block(0.25, false);
        CutMove(dividedX, profile, { 25, 10, 5 }, { 15, 12, 4 }, { 1, 0, 0 });
        CutMove(denseX, profile, { 25, 10, 5 }, { 15, 12, 4 }, { 1, 0, 0 });
        CHECK(SameCells(dividedX, denseX));
    }

    // And an imported shape.
    VoxelStock divided;
    divided.Reset({ { 0, 0, 0 }, { 20, 20, 10 } }, 0.25, true);
    divided.FillMesh(BoxMesh({ 2, 3, 1 }, { 17, 15, 9 }));
    VoxelStock dense;
    dense.Reset({ { 0, 0, 0 }, { 20, 20, 10 } }, 0.25, false);
    dense.FillMesh(BoxMesh({ 2, 3, 1 }, { 17, 15, 9 }));
    CHECK(SameCells(divided, dense));
    CHECK(divided.Count() == 60LL * 48 * 32);
}

TEST_CASE("bricks are split only where cut, and merge again when they're cut away")
{
    VoxelStock stock;
    stock.Reset({ { 0, 0, 0 }, { 40, 40, 10 } }, 0.25, true);     // 160 x 160 x 40 cells: 20 x 20 x 1 bricks
    stock.FillAll();
    CHECK(stock.MixedBricks() == 0);
    CHECK(stock.FullBricks() == 400);

    // A 2 mm plunge at a brick's middle touches just that brick.
    CutMove(stock, Flat(1), { 1, 1, 20 }, { 1, 1, 8 }, kUp);
    CHECK(stock.MixedBricks() == 1);

    // Facing everything down to below the bottom empties every brick.
    for (double y = 0; y <= 40; y += 4)
        CutMove(stock, Flat(6), { -5, y, -1 }, { 45, y, -1 }, kUp);
    CHECK(stock.Count() == 0);
    CHECK(stock.MixedBricks() == 0);
    CHECK(stock.FullBricks() == 0);

    // Undivided stock keeps its words.
    VoxelStock dense;
    dense.Reset({ { 0, 0, 0 }, { 40, 40, 10 } }, 0.25, false);
    dense.FillAll();
    CHECK(dense.MixedBricks() == 400);
}

TEST_CASE("meshing the changed chunks on all cores gives the same meshes as one at a time")
{
    VoxelStock stock;
    stock.Reset({ { 0, 0, 0 }, { 30, 30, 10 } }, 0.1, true);
    stock.FillAll();
    for (int c = 0; c < stock.ChunkCount(); ++c)
        stock.SetChunkClean(c);     // as if it had been meshed
    CutMove(stock, Flat(6), { 3, 3, 8 }, { 27, 21, 8 }, kUp);
    CutMove(stock, ProfileOf([] { Tool t; t.type = ToolType::BallMill; t.diameter = 4; t.overallLength = 30; return t; }()),
            { 25, 5, 15 }, { 25, 5, 6 }, kUp);
    const std::vector<int> dirty = DirtyChunks(stock);
    CHECK_FALSE(dirty.empty());
    CHECK(dirty.size() < static_cast<size_t>(stock.ChunkCount()));

    std::vector<StockMesh> meshes;
    MeshChunks(stock, dirty, meshes);
    REQUIRE(meshes.size() == static_cast<size_t>(stock.ChunkCount()));
    for (int c : dirty)
    {
        const StockMesh one = MeshChunk(stock, c);
        CHECK(meshes[static_cast<size_t>(c)].vertices == one.vertices);
        CHECK(meshes[static_cast<size_t>(c)].faces == one.faces);
    }
}

TEST_CASE("an empty chunk has no faces, and a tall stock meshes word by word")
{
    VoxelStock stock;
    stock.Reset({ { 0, 0, 0 }, { 10, 10, 20 } }, 0.1, true);     // 100 x 100 x 200 cells: 4 x 4 x 4 chunks
    stock.FillAll();
    size_t triangles = 0;
    for (int c = 0; c < stock.ChunkCount(); ++c)
        triangles += MeshChunk(stock, c).TriangleCount();
    // Every chunk on the outside shows a rectangle on each outside face.
    // Sides: 4 sides x 4 across x 4 up; top and bottom: 2 x 16.
    CHECK(triangles == (4 * 4 * 4 + 2 * 16) * 2);

    CutMove(stock, Flat(30), { 5, 5, -1 }, { 5, 5, -1 }, kUp);
    CHECK(stock.Count() == 0);
    for (int c = 0; c < stock.ChunkCount(); ++c)
        CHECK(MeshChunk(stock, c).TriangleCount() == 0);
}

TEST_CASE("a simulation can be started undivided at a chosen cell, and rewinds either way")
{
    const std::vector<ToolpathMove> moves = {
        Move(MoveKind::Rapid, { 5, 10, 15 }), Move(MoveKind::Feed, { 5, 10, 8 }), Move(MoveKind::Feed, { 15, 10, 8 }),
    };
    for (bool divide : { true, false })
    {
        Simulation simulation;
        simulation.Start({ { 0, 0, 0 }, { 20, 20, 10 } }, {}, { Operation(Flat(4), moves) },
                         SimResolution{ divide, 0.2 });
        CHECK(simulation.Stock().Cell() == doctest::Approx(0.2));
        CHECK(simulation.Stock().Divided() == divide);
        const long long whole = simulation.Stock().Count();
        simulation.RunToEnd();
        CHECK(simulation.Stock().Count() < whole);
        simulation.Rewind();
        CHECK(simulation.Stock().Count() == whole);
    }
}

// Run by name (-tc="simulation timing*") to see how long a realistic job takes.
TEST_CASE("simulation timing on a 136 x 150 x 6 stock" * doctest::skip())
{
    std::vector<ToolpathMove> moves = { Move(MoveKind::Rapid, { -5, 0, 10 }) };
    for (double depth : { 5.5, 5.0 })
    {
        for (double y = 0; y <= 150; y += 4)
        {
            moves.push_back(Move(MoveKind::Rapid, { -5, y, 10 }));
            moves.push_back(Move(MoveKind::Feed, { -5, y, depth }));
            moves.push_back(Move(MoveKind::Feed, { 141, y, depth }));
            moves.push_back(Move(MoveKind::Rapid, { 141, y, 10 }));
        }
    }
    for (bool divide : { true, false })
    {
        const auto t0 = std::chrono::steady_clock::now();
        Simulation simulation;
        simulation.Start({ { 0, 0, 0 }, { 136, 150, 6 } }, {}, { Operation(Flat(6), moves) },
                         SimResolution{ divide, 0.0 });
        const auto ms = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count(); };
        const auto t1 = std::chrono::steady_clock::now();
        std::vector<StockMesh> meshes;
        const auto meshAll = [&]() {
            std::vector<int> all(static_cast<size_t>(simulation.Stock().ChunkCount()));
            for (size_t c = 0; c < all.size(); ++c)
                all[c] = static_cast<int>(c);
            MeshChunks(simulation.Stock(), all, meshes);
            size_t count = 0;
            for (const StockMesh& mesh : meshes)
                count += mesh.TriangleCount();
            return count;
        };
        const size_t triangles = meshAll();
        const auto t2 = std::chrono::steady_clock::now();
        simulation.RunToEnd();
        const auto t3 = std::chrono::steady_clock::now();
        const size_t after = meshAll();
        const auto t4 = std::chrono::steady_clock::now();
        VoxelStock part;
        part.ResetLike(simulation.Stock());
        part.FillMesh(BoxMesh({ 2, 2, 0 }, { 134, 148, 5 }));
        const auto t5 = std::chrono::steady_clock::now();
        const StockDifference difference = CompareStock(simulation.Stock(), part);
        const auto t6 = std::chrono::steady_clock::now();
        MESSAGE("part filled in " << ms(t4, t5) << " ms, compared in " << ms(t5, t6) << " ms: not enough "
                << difference.notEnoughVolume << " mm3, too much " << difference.tooMuchVolume << " mm3");
        MESSAGE(std::string(divide ? "divided" : "undivided") << " cell " << simulation.Stock().Cell() << ": start "
                << ms(t0, t1) << " ms, mesh " << triangles << " in " << ms(t1, t2) << " ms, cut "
                << simulation.Moves().size() << " moves in " << ms(t2, t3) << " ms, mesh " << after << " in "
                << ms(t3, t4) << " ms, mixed bricks " << simulation.Stock().MixedBricks());
    }
}

namespace
{
    std::uint32_t ReadUint32(const std::string& bytes, size_t at)
    {
        std::uint32_t value = 0;
        for (int b = 0; b < 4; ++b)
            value |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + b])) << (8 * b);
        return value;
    }

    float ReadFloat(const std::string& bytes, size_t at)
    {
        const std::uint32_t raw = ReadUint32(bytes, at);
        float value;
        std::memcpy(&value, &raw, sizeof(value));
        return value;
    }
}

TEST_CASE("the whole stock meshes as the chunks joined, and saves as a binary STL")
{
    VoxelStock stock = Block(0.5);      // 2 x 2 x 1 chunks
    const StockMesh whole = MeshStock(stock);
    size_t separately = 0;
    for (int c = 0; c < stock.ChunkCount(); ++c)
        separately += MeshChunk(stock, c).TriangleCount();
    CHECK(whole.TriangleCount() == separately);
    CHECK(whole.vertices.size() == whole.normals.size());
    for (size_t f = 0; f + 3 < whole.faces.size(); f += 4)
    {
        CHECK(whole.faces[f] == 3);
        for (int c = 1; c <= 3; ++c)
            CHECK(static_cast<size_t>(whole.faces[f + c]) < whole.vertices.size() / 3);
    }

    const std::string stl = StockMeshToStl(whole, "CAMForAlibre test");
    REQUIRE(stl.size() == 84 + 50 * whole.TriangleCount());
    CHECK(stl.compare(0, 17, "CAMForAlibre test") == 0);
    CHECK(ReadUint32(stl, 80) == whole.TriangleCount());
    for (size_t t = 0; t < whole.TriangleCount(); ++t)
    {
        const size_t at = 84 + 50 * t;
        const Vec3 normal{ ReadFloat(stl, at), ReadFloat(stl, at + 4), ReadFloat(stl, at + 8) };
        CHECK(Length(normal) == doctest::Approx(1.0));
        CHECK(std::fabs(normal.x) + std::fabs(normal.y) + std::fabs(normal.z) == doctest::Approx(1.0));
        for (int c = 0; c < 3; ++c)
        {
            // Each corner is on the box's surface, on the face the normal points out of.
            const Vec3 p{ ReadFloat(stl, at + 12 + 12 * c), ReadFloat(stl, at + 16 + 12 * c),
                          ReadFloat(stl, at + 20 + 12 * c) };
            const double faceAt = (normal.x > 0.5) ? p.x - 20 : (normal.x < -0.5) ? p.x
                                : (normal.y > 0.5) ? p.y - 20 : (normal.y < -0.5) ? p.y
                                : (normal.z > 0.5) ? p.z - 10 : p.z;
            CHECK(faceAt == doctest::Approx(0.0));
        }
        CHECK(stl[at + 48] == 0);
        CHECK(stl[at + 49] == 0);
    }
}

namespace
{
    // A stock over 20 x 20 x 10 at 0.25 mm, filled with a box.
    VoxelStock BoxCells(const Vec3& a, const Vec3& b, bool divide = true)
    {
        VoxelStock stock;
        stock.Reset({ { 0, 0, 0 }, { 20, 20, 10 } }, 0.25, divide);
        stock.FillMesh(BoxMesh(a, b));
        return stock;
    }

    const Vec3 kPartLow{ 4, 4, 2 };
    const Vec3 kPartHigh{ 16, 14, 8 };
}

TEST_CASE("stock cut exactly to the part shows no differences")
{
    const VoxelStock part = BoxCells(kPartLow, kPartHigh);
    const StockDifference difference = CompareStock(part, part);
    CHECK(difference.notEnough.Count() == 0);
    CHECK(difference.tooMuch.Count() == 0);
    CHECK(difference.notEnoughVolume == 0.0);
}

TEST_CASE("stock left proud of the part is not enough removed, within a cell's tolerance")
{
    const VoxelStock part = BoxCells(kPartLow, kPartHigh);

    // 2 mm left on the +X side: a 2 x 10 x 6 slab, less the cell next to the part.
    const VoxelStock proud = BoxCells(kPartLow, { kPartHigh.x + 2, kPartHigh.y, kPartHigh.z });
    const StockDifference difference = CompareStock(proud, part);
    CHECK(difference.tooMuch.Count() == 0);
    CHECK(difference.notEnoughVolume == doctest::Approx(1.75 * 10 * 6));

    // One cell proud is within the tolerance.
    const VoxelStock oneCell = BoxCells(kPartLow, { kPartHigh.x + 0.25, kPartHigh.y, kPartHigh.z });
    CHECK(CompareStock(oneCell, part).notEnough.Count() == 0);
}

TEST_CASE("a cut into the part is too much removed, and divided or not gives the same")
{
    for (bool divide : { true, false })
    {
        const VoxelStock part = BoxCells(kPartLow, kPartHigh, divide);
        VoxelStock stock = BoxCells(kPartLow, kPartHigh, divide);
        // A 4 mm slot 2 mm deep across the top, along X.
        CutMove(stock, Flat(4), { 0, 9, 6 }, { 20, 9, 6 }, kUp);
        const StockDifference difference = CompareStock(stock, part);
        CHECK(difference.notEnough.Count() == 0);
        // The slot is 12 x 4 x 2 inside the part. The part shrunk by a cell is
        // trimmed only at its own faces: the slot's ends at the part's sides and
        // its top, so 11.5 long x 4 wide x 1.75 deep.
        CHECK(difference.tooMuchVolume == doctest::Approx(11.5 * 4 * 1.75));
        CHECK(difference.tooMuch.Divided() == divide);
    }
}

TEST_CASE("each move takes its time: feeds, the rapid rate and dwells")
{
    std::vector<ToolpathMove> moves = {
        Move(MoveKind::Rapid, { 0, 10, 15 }), Move(MoveKind::Rapid, { 0, 10, 5 }),     // 10 at the rapid rate
        Move(MoveKind::Feed, { 0, 10, 4 }), Move(MoveKind::Feed, { 100, 10, 4 }),
    };
    moves[2].feed = 300.0;          // 1 mm at 300 mm/min: 0.2 s
    moves[3].feed = 600.0;          // 100 mm at 600 mm/min: 10 s
    ToolpathMove dwell;
    dwell.kind = MoveKind::Dwell;
    dwell.to = { 100, 10, 4 };
    dwell.seconds = 1.5;
    moves.push_back(dwell);
    Simulation simulation;
    simulation.Start({ { 0, 0, 0 }, { 120, 20, 10 } }, {}, { Operation(Flat(4), moves) }, SimResolution{ true, 0.5 },
                     3000.0);      // 10 mm at 3000 mm/min: 0.2 s
    REQUIRE(simulation.Moves().size() == 5);     // the first only says where it starts
    CHECK(simulation.Moves()[0].seconds == 0.0);
    CHECK(simulation.Moves()[1].seconds == doctest::Approx(0.2));
    CHECK(simulation.Moves()[2].seconds == doctest::Approx(0.2));
    CHECK(simulation.Moves()[3].seconds == doctest::Approx(10.0));
    CHECK(simulation.Moves()[4].seconds == doctest::Approx(1.5));
    CHECK(simulation.TotalSeconds() == doctest::Approx(11.9));
    CHECK(simulation.ElapsedSeconds() == 0.0);
}

TEST_CASE("advancing by time cuts part way along a move, and the rest matches cutting it whole")
{
    std::vector<ToolpathMove> moves = {
        Move(MoveKind::Rapid, { 10, 10, 15 }), Move(MoveKind::Feed, { 10, 10, 6 }),
        Move(MoveKind::Feed, { 110, 10, 6 }), Move(MoveKind::Rapid, { 110, 10, 15 }),
    };
    moves[1].feed = 600.0;      // 0.9 s
    moves[2].feed = 600.0;      // 10 s
    const auto start = [&](Simulation& simulation) {
        simulation.Start({ { 0, 0, 0 }, { 120, 20, 10 } }, {}, { Operation(Flat(4), moves) }, SimResolution{ true, 0.5 });
    };

    Simulation whole;
    start(whole);
    whole.RunToEnd();
    const long long cut = whole.Stock().Count();

    // Half way along the long move: the tool is in the middle, with that much cut.
    Simulation timed;
    start(timed);
    timed.Advance(0.9 + 5.0);
    CHECK(timed.Position() == 2);
    CHECK(timed.ToolTip().x == doctest::Approx(60.0));
    CHECK(timed.ElapsedSeconds() == doctest::Approx(5.9));
    CHECK_FALSE(FilledAt(timed.Stock(), 40, 10, 7));    // behind the tool
    CHECK(FilledAt(timed.Stock(), 80, 10, 7));          // ahead of it
    timed.Advance(100.0);
    CHECK(timed.Finished());
    CHECK(timed.Stock().Count() == cut);

    // Many small steps: the same.
    Simulation small;
    start(small);
    while (!small.Finished())
        small.Advance(0.37);
    CHECK(small.Stock().Count() == cut);

    // Step after stopping part way finishes the move; Rewind clears it.
    Simulation mixed;
    start(mixed);
    mixed.Advance(3.0);
    mixed.Step(1);
    CHECK(mixed.Position() == 3);
    CHECK(mixed.ToolTip().x == doctest::Approx(110.0));
    mixed.Rewind();
    CHECK(mixed.ElapsedSeconds() == 0.0);
    CHECK(mixed.ToolTip().x == doctest::Approx(10.0));
}

TEST_CASE("a rapid into stock found part way along stops there")
{
    const std::vector<ToolpathMove> moves = {
        Move(MoveKind::Rapid, { 5, 10, 15 }), Move(MoveKind::Rapid, { 5, 10, 5 }),
        Move(MoveKind::Feed, { 15, 10, 5 }),
    };
    Simulation simulation;
    simulation.Start({ { 0, 0, 0 }, { 20, 20, 10 } }, {}, { Operation(Flat(4), moves) }, SimResolution{ true, 0.5 },
                     600.0);   // the 10 mm rapid takes 1 s
    simulation.Advance(0.8);    // 8 mm down: into the stock at Z 7
    CHECK(simulation.HasWarning());
    CHECK(simulation.Position() == 1);
    CHECK(simulation.WarningMove() == 1);
}

namespace
{
    ToolProfile VBit(double diameter, double angle)
    {
        Tool tool;
        tool.type = ToolType::VMill;
        tool.diameter = diameter;
        tool.includedAngle = angle;
        tool.fluteLength = 10;
        tool.overallLength = 40;
        return ProfileOf(tool);
    }
}

TEST_CASE("a V bit skimming the top cuts exactly the cells inside its cone")
{
    // A 6 mm 90 degree V bit 0.3 deep along X across a 30 x 10 x 5 block.
    const double cell = 0.1;
    VoxelStock stock;
    stock.Reset({ { 0, 0, 0 }, { 30, 10, 5 } }, cell, true);
    stock.FillAll();
    const long long before = stock.Count();
    const ToolProfile profile = VBit(6.0, 90.0);
    const Vec3 from{ 5, 5, 4.7 }, to{ 25, 5, 4.7 };
    const long long removed = CutMove(stock, profile, from, to, { 0, 0, 1 });

    // Every cell, by brute force: inside if its centre is above the tip and within
    // the cone's radius at its height of the move's line.
    long long expected = 0;
    for (int k = 0; k < stock.SizeZ(); ++k)
    {
        for (int j = 0; j < stock.SizeY(); ++j)
        {
            for (int i = 0; i < stock.SizeX(); ++i)
            {
                const Vec3 c = stock.Centre(i, j, k);
                const double h = c.z - from.z;
                if (h < 0.0)
                    continue;
                const double x = (std::max)(from.x, (std::min)(to.x, c.x));
                const double d = std::hypot(c.x - x, c.y - from.y);
                if (d <= profile.RadiusAt(h) + 1e-9)
                    ++expected;
            }
        }
    }
    CHECK(removed == expected);
    CHECK(before - stock.Count() == expected);
    CHECK(expected > 0);
}

TEST_CASE("V bit engraving timing" * doctest::skip())
{
    // A 6 mm V bit 0.1 deep, zigzagging over a 136 x 150 x 6 stock at 0.05 mm.
    VoxelStock stock;
    stock.Reset({ { 0, 0, 0 }, { 136, 150, 6 } }, 0.05, true);
    stock.FillAll();
    const ToolProfile profile = VBit(6.0, 90.0);
    const auto start = std::chrono::steady_clock::now();
    long long removed = 0;
    for (int row = 0; row < 20; ++row)
    {
        const double y = 10.0 + row * 6.0;
        removed += CutMove(stock, profile, { 5, y, 5.9 }, { 130, y, 5.9 }, { 0, 0, 1 });
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("V bit engraving: 20 rows of 125 mm, %lld cells, %.3f s\n", removed, seconds);
}
