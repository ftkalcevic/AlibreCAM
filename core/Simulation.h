// Simulation.h : cutting the stock with the toolpaths, as a grid of voxels. No Alibre or MFC code belongs here.
//
// Everything is in the assembly's coordinate system, in millimetres. The stock is
// a grid of cells over its box, in the assembly's axes; a cell is cut away when
// its centre falls inside the tool.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Stock.h"
#include "ToolLibrary.h"
#include "Toolpath.h"

namespace camcore
{
    // The cells' size: the smallest of 0.1, 0.2, 0.25, 0.5, 1, 2, 5 mm... that keeps
    // the grid to at most maxCells.
    double ChooseCellSize(const StockBox& box, double maxCells = 2.0e8);

    class VoxelStock
    {
    public:
        static constexpr int kChunk = 32;   // cells along each side of a chunk, which is meshed on its own

        // An empty grid over the box. Its size is rounded up to whole cells.
        void Reset(const StockBox& box, double cell);
        void FillAll();

        // Fills the cells inside a closed triangle mesh (three points to a
        // triangle), by counting its crossings up each column.
        void FillMesh(const std::vector<Vec3>& triangles);

        int SizeX() const { return m_nx; }
        int SizeY() const { return m_ny; }
        int SizeZ() const { return m_nz; }
        double Cell() const { return m_cell; }
        const Vec3& Lower() const { return m_lower; }

        bool Filled(int i, int j, int k) const;

        // A cell by its index in the grid: (k * SizeY() + j) * SizeX() + i.
        bool FilledAt(size_t index) const { return ((m_bits[index / 64] >> (index % 64)) & 1) != 0; }
        void Clear(int i, int j, int k);
        Vec3 Centre(int i, int j, int k) const;
        long long Count() const;

        // The chunks, and which have changed since they were last meshed.
        int ChunksX() const { return (m_nx + kChunk - 1) / kChunk; }
        int ChunksY() const { return (m_ny + kChunk - 1) / kChunk; }
        int ChunksZ() const { return (m_nz + kChunk - 1) / kChunk; }
        int ChunkCount() const { return ChunksX() * ChunksY() * ChunksZ(); }
        bool ChunkDirty(int chunk) const { return m_dirty[static_cast<size_t>(chunk)] != 0; }
        void SetChunkClean(int chunk) { m_dirty[static_cast<size_t>(chunk)] = 0; }
        void MarkAllDirty();

        // A copy of the cells, and putting them back, for rewinding.
        const std::vector<std::uint64_t>& Bits() const { return m_bits; }
        void SetBits(const std::vector<std::uint64_t>& bits);

    private:
        size_t Index(int i, int j, int k) const
        {
            return (static_cast<size_t>(k) * m_ny + j) * m_nx + i;
        }
        void MarkDirty(int i, int j, int k);

        Vec3 m_lower;
        double m_cell = 1.0;
        int m_nx = 0;
        int m_ny = 0;
        int m_nz = 0;
        std::vector<std::uint64_t> m_bits;
        std::vector<char> m_dirty;
    };

    // A tool's shape, round its axis: its radius at each height above the tip.
    struct ToolProfile
    {
        ToolType type = ToolType::FlatEndMill;
        double radius = 1.0;
        double length = 50.0;       // above this the tool isn't modelled (the holder isn't yet)
        double cornerRadius = 0.0;
        double tipRadius = 0.0;
        double slope = 0.0;         // tan of the half angle for V, chamfer and taper mills and drills

        // The radius at a height above the tip, or a negative number below the tip
        // or above the tool.
        double RadiusAt(double height) const;
    };

    // The shape of a tool, in millimetres (from ToolInMillimetres).
    ToolProfile ProfileOf(const Tool& tool);

    // Removes every cell whose centre is inside the tool as its tip moves from
    // `from` to `to` along a straight line, with the tool pointing along `axis`
    // (a unit vector from the tip up the tool). Returns how many cells it removed.
    long long CutMove(VoxelStock& stock, const ToolProfile& profile, const Vec3& from, const Vec3& to, const Vec3& axis);

    // A chunk's surface: the faces between filled and empty cells, merged into
    // rectangles where they lie flat together, as triangles. Vertices and normals
    // are three floats each; each triangle is 3 followed by its three vertex numbers.
    struct StockMesh
    {
        std::vector<float> vertices;
        std::vector<float> normals;
        std::vector<int> faces;

        size_t TriangleCount() const { return faces.size() / 4; }
    };
    StockMesh MeshChunk(const VoxelStock& stock, int chunk);

    // One straight move of the simulation, in assembly mm.
    struct SimMove
    {
        Vec3 from;
        Vec3 to;
        bool rapid = false;
        size_t operation = 0;       // which of the simulation's operations
        size_t toolpathMove = 0;    // which move of the operation's toolpath it comes from
    };

    // An operation to simulate: its generated toolpath, in its origin's frame.
    struct SimOperation
    {
        std::string name;
        ToolProfile profile;
        Frame frame;
        Toolpath toolpath;
    };

    // Steps through the operations' moves, cutting the stock.
    class Simulation
    {
    public:
        // The stock filled, and the operations' moves made ready. A triangle mesh
        // fills the box's cells inside it; none fills them all.
        void Start(const StockBox& box, const std::vector<Vec3>& stockTriangles,
                   const std::vector<SimOperation>& operations, double maxCells = 2.0e8);

        VoxelStock& Stock() { return m_stock; }
        const VoxelStock& Stock() const { return m_stock; }
        const std::vector<SimMove>& Moves() const { return m_moves; }
        const std::vector<SimOperation>& Operations() const { return m_operations; }

        // How many moves are done; the next to cut is this one.
        size_t Position() const { return m_position; }
        bool Finished() const { return m_position >= m_moves.size(); }

        // Where the tool's tip is, its axis, and its shape, at the current position.
        Vec3 ToolTip() const;
        const SimOperation* CurrentOperation() const;

        // Cuts up to count moves. Stops early at a rapid that cuts stock, which is
        // then the warning. Returns how many moves were done.
        size_t Step(size_t count);

        // To the end of the current operation, or of everything.
        size_t RunToOperationEnd();
        size_t RunToEnd() { return Step(m_moves.size()); }

        // Back to the start, with the stock whole again.
        void Rewind();

        // To a position, by rewinding and cutting up to it.
        void SeekTo(size_t position);

        // A rapid move that cut stock, and stopped stepping.
        bool HasWarning() const { return m_warning; }
        size_t WarningMove() const { return m_warningMove; }
        void ClearWarning() { m_warning = false; }

    private:
        VoxelStock m_stock;
        std::vector<std::uint64_t> m_start;     // the whole stock, to rewind to
        std::vector<SimOperation> m_operations;
        std::vector<SimMove> m_moves;
        size_t m_position = 0;
        bool m_warning = false;
        size_t m_warningMove = 0;
    };
}
