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
    // The cell sizes the Resolution slider offers, fine to coarse: the ones whose
    // whole grid fits in maxCells, at most seven.
    std::vector<double> ResolutionStops(const StockBox& box, double maxCells = 2.0e8);

    // The cell size when the stock is divided only where it's cut: the finest whose
    // top has at most maxColumns columns of cells.
    double AutoDivideCell(const StockBox& box, double maxColumns = 1.6e7);

    // How the simulation's stock is divided into cells.
    struct SimResolution
    {
        bool autoDivide = true;     // solid stock kept whole, divided into cells only where it's cut
        double cell = 0.0;          // the cell size in mm; 0 chooses one (AutoDivideCell, or the middle stop)
    };

    // The stock as cells in a grid over its box. Cells are kept 64 at a time up
    // each column, as the bits of one word. Bricks of 8 x 8 columns, one word tall,
    // are either empty, full, or mixed with their own 64 words. When the stock is
    // divided (the usual way), full and empty bricks take no memory, a brick is
    // split the first time it's cut, and goes back to empty or full when it can.
    class VoxelStock
    {
    public:
        static constexpr int kBrick = 8;    // columns along each side of a brick
        static constexpr int kChunk = 32;   // cells across a chunk, which is meshed on its own; a chunk is one word tall
        static constexpr int kWord = 64;    // cells up a column's word

        // An empty grid over the box. Its size is rounded up to whole cells. An
        // undivided grid keeps every brick's words, and never merges them.
        void Reset(const StockBox& box, double cell, bool divide = true);

        // An empty grid over the same box, with the same cells and division.
        void ResetLike(const VoxelStock& other) { Reset(other.m_box, other.m_cell, other.m_divide); }
        void FillAll();

        // Fills the cells inside a closed triangle mesh (three points to a
        // triangle), by counting its crossings up each column.
        void FillMesh(const std::vector<Vec3>& triangles);

        int SizeX() const { return m_nx; }
        int SizeY() const { return m_ny; }
        int SizeZ() const { return m_nz; }
        int WordsZ() const { return m_bz; }
        double Cell() const { return m_cell; }
        const Vec3& Lower() const { return m_lower; }
        bool Divided() const { return m_divide; }

        bool Filled(int i, int j, int k) const;
        void Clear(int i, int j, int k);
        Vec3 Centre(int i, int j, int k) const;
        long long Count() const;

        // A column's word: cells w * 64 up to w * 64 + 63, the lowest in bit 0.
        // Outside the grid it's empty.
        std::uint64_t Word(int i, int j, int w) const;

        // Word w of the columns from (i0, j0), ni across and nj along, into out row
        // by row; columns outside the grid are empty.
        void ReadWords(int i0, int j0, int ni, int nj, int w, std::uint64_t* out) const;

        // The bits of word w that are cells k0 to k1 (both included).
        static std::uint64_t SpanMask(int w, int k0, int k1);

        // Clears the cells of a column's word that are set in mask, and returns how
        // many were filled. Doesn't mark chunks; MarkDirtyBox does.
        long long ClearMask(int i, int j, int w, std::uint64_t mask);

        // Sets the cells of a column's word that are set in mask. Call Settle after.
        void SetCells(int i, int j, int w, std::uint64_t mask) { SetMask(i, j, w, mask); }

        // Merges the bricks cut since the last call that are now all empty or full.
        void Settle();

        // The chunks, and which have changed since they were last meshed.
        int ChunksX() const { return (m_nx + kChunk - 1) / kChunk; }
        int ChunksY() const { return (m_ny + kChunk - 1) / kChunk; }
        int ChunksZ() const { return m_bz; }
        int ChunkCount() const { return ChunksX() * ChunksY() * ChunksZ(); }
        bool ChunkDirty(int chunk) const { return m_dirty[static_cast<size_t>(chunk)] != 0; }
        void SetChunkClean(int chunk) { m_dirty[static_cast<size_t>(chunk)] = 0; }
        void MarkAllDirty();

        // Marks the chunks holding any cell from (i0, j0, k0) to (i1, j1, k1), and
        // those next to it.
        void MarkDirtyBox(int i0, int j0, int k0, int i1, int j1, int k1);

        // How many bricks are mixed, and full.
        int MixedBricks() const;
        int FullBricks() const;

    private:
        static constexpr std::int32_t kEmpty = -1;
        static constexpr std::int32_t kFull = -2;

        int BricksX() const { return (m_nx + kBrick - 1) / kBrick; }
        int BricksY() const { return (m_ny + kBrick - 1) / kBrick; }
        size_t BrickOf(int i, int j, int w) const
        {
            return (static_cast<size_t>(w) * BricksY() + j / kBrick) * BricksX() + i / kBrick;
        }
        static size_t WordInBrick(int i, int j) { return static_cast<size_t>(j % kBrick) * kBrick + i % kBrick; }
        std::uint64_t TopMask(int w) const;
        std::uint64_t FullWord(int i, int j, int w) const;     // what a full brick's word holds
        std::int32_t Split(size_t brick);
        void SetMask(int i, int j, int w, std::uint64_t mask);
        void MergeBrick(size_t brick);
        void MarkDirty(int i, int j, int k);

        StockBox m_box;
        Vec3 m_lower;
        double m_cell = 1.0;
        int m_nx = 0;
        int m_ny = 0;
        int m_nz = 0;
        int m_bz = 0;
        bool m_divide = true;
        std::vector<std::int32_t> m_bricks;     // kEmpty, kFull, or a block in m_pool
        std::vector<std::uint64_t> m_pool;      // blocks of 64 words, a brick's columns row by row
        std::vector<std::int32_t> m_free;       // blocks no longer used
        std::vector<char> m_touched;            // bricks cut since Settle
        std::vector<size_t> m_touchedList;
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

        // The lowest height above the tip where the tool is at least this wide, or
        // a negative number if it never is. Close, not exact: callers check the
        // cells either side with RadiusAt.
        double HeightForRadius(double radius) const;
    };

    // The shape of a tool, in millimetres (from ToolInMillimetres).
    ToolProfile ProfileOf(const Tool& tool);

    // Removes every cell whose centre is inside the tool as its tip moves from
    // `from` to `to` along a straight line, with the tool pointing along `axis`
    // (a unit vector from the tip up the tool). Returns how many cells it removed.
    // A tool along the grid's Z (either way) is cut a column at a time; generalOnly
    // forces the cell by cell way, for tests.
    long long CutMove(VoxelStock& stock, const ToolProfile& profile, const Vec3& from, const Vec3& to, const Vec3& axis,
                      bool generalOnly = false);

    // A chunk's surface (32 x 32 cells across, one word tall): the faces between filled and empty cells, merged into
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

    // Meshes the listed chunks into their slots of meshes (one per chunk of the
    // stock), on all the processor's cores. The stock is only read.
    void MeshChunks(const VoxelStock& stock, const std::vector<int>& chunks, std::vector<StockMesh>& meshes);

    // Every chunk of the stock that has changed since it was last meshed.
    std::vector<int> DirtyChunks(const VoxelStock& stock);

    // The chunks' meshes as one, each chunk's vertex numbers moved past those before it.
    StockMesh JoinMeshes(const std::vector<StockMesh>& chunks);

    // The whole stock's surface, meshed on all cores.
    StockMesh MeshStock(const VoxelStock& stock);

    // Where the simulated stock differs from the parts, both in the same grid: stock
    // left outside the parts (not enough removed), and part material cut away (too
    // much removed). A difference within one cell of the part's surface isn't
    // counted: the part is grown by a cell for the first, and shrunk by one for the
    // second, over the 3 x 3 x 3 cells round each.
    struct StockDifference
    {
        VoxelStock notEnough;
        VoxelStock tooMuch;
        double notEnoughVolume = 0.0;   // mm^3
        double tooMuchVolume = 0.0;
    };
    StockDifference CompareStock(const VoxelStock& stock, const VoxelStock& part);

    // A mesh as the bytes of a binary STL file, in mm: an 80 byte header holding
    // the title, the number of triangles, then each triangle's normal and corners.
    std::string StockMeshToStl(const StockMesh& mesh, const std::string& title);

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
                   const std::vector<SimOperation>& operations, const SimResolution& resolution = SimResolution());

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
        VoxelStock m_start;                     // the whole stock, to rewind to
        std::vector<SimOperation> m_operations;
        std::vector<SimMove> m_moves;
        size_t m_position = 0;
        bool m_warning = false;
        size_t m_warningMove = 0;
    };
}
