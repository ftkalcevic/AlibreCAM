// Simulation.cpp : cutting the stock with the toolpaths, as a grid of voxels.

#include "Simulation.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <execution>

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace camcore
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;

        // Cell sizes, fine to coarse.
        const double kCellSizes[] = { 0.05, 0.1, 0.2, 0.25, 0.5, 1.0, 2.0, 2.5, 5.0, 10.0, 20.0, 25.0, 50.0, 100.0 };

        int CellsAlong(double length, double cell)
        {
            return (std::max)(1, static_cast<int>(std::ceil(length / cell - 1e-9)));
        }

        int CountBits(std::uint64_t x)
        {
            x = x - ((x >> 1) & 0x5555555555555555ULL);
            x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
            x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
            return static_cast<int>((x * 0x0101010101010101ULL) >> 56);
        }

        int LowestBit(std::uint64_t x)
        {
#ifdef _MSC_VER
            unsigned long index = 0;
            _BitScanForward64(&index, x);
            return static_cast<int>(index);
#else
            return __builtin_ctzll(x);
#endif
        }

        // The distance from p to the segment a-b.
        double DistanceToSegment(const Vec3& p, const Vec3& a, const Vec3& b)
        {
            const Vec3 ab = b - a;
            const double lengthSquared = Dot(ab, ab);
            double t = 0.0;
            if (lengthSquared > 1e-18)
                t = (std::min)(1.0, (std::max)(0.0, Dot(p - a, ab) / lengthSquared));
            return Length(p - (a + ab * t));
        }

        // Where the row at height y crosses the points within radius of the segment
        // a-b (in XY): from lo to hi, or false if it doesn't.
        bool CapsuleRow(const Vec3& a, const Vec3& b, double radius, double y, double& lo, double& hi)
        {
            lo = 1e300;
            hi = -1e300;
            for (const Vec3* c : { &a, &b })
            {
                const double dy = y - c->y;
                if (std::fabs(dy) <= radius)
                {
                    const double dx = std::sqrt((std::max)(0.0, radius * radius - dy * dy));
                    lo = (std::min)(lo, c->x - dx);
                    hi = (std::max)(hi, c->x + dx);
                }
            }
            const double ex = b.x - a.x, ey = b.y - a.y;
            const double length = std::hypot(ex, ey);
            if (length > 1e-12)
            {
                const double nx = -ey / length * radius, ny = ex / length * radius;
                const Vec3 corners[4] = { { a.x + nx, a.y + ny, 0 }, { b.x + nx, b.y + ny, 0 },
                                          { b.x - nx, b.y - ny, 0 }, { a.x - nx, a.y - ny, 0 } };
                for (int e = 0; e < 4; ++e)
                {
                    const Vec3& p = corners[e];
                    const Vec3& q = corners[(e + 1) % 4];
                    if ((p.y - y) * (q.y - y) > 0.0)
                        continue;
                    if (std::fabs(q.y - p.y) < 1e-15)
                    {
                        lo = (std::min)({ lo, p.x, q.x });
                        hi = (std::max)({ hi, p.x, q.x });
                        continue;
                    }
                    const double x = p.x + (y - p.y) * (q.x - p.x) / (q.y - p.y);
                    lo = (std::min)(lo, x);
                    hi = (std::max)(hi, x);
                }
            }
            return lo <= hi;
        }
    }

    std::vector<double> ResolutionStops(const StockBox& box, double maxCells)
    {
        const Vec3 size = box.upper - box.lower;
        std::vector<double> stops;
        for (double cell : kCellSizes)
        {
            const double cells = static_cast<double>(CellsAlong(size.x, cell)) * CellsAlong(size.y, cell) *
                                 CellsAlong(size.z, cell);
            if (cells <= maxCells && stops.size() < 7)
                stops.push_back(cell);
        }
        if (stops.empty())
            stops.push_back(kCellSizes[sizeof(kCellSizes) / sizeof(kCellSizes[0]) - 1]);
        return stops;
    }

    double AutoDivideCell(const StockBox& box, double maxColumns)
    {
        const Vec3 size = box.upper - box.lower;
        for (double cell : kCellSizes)
        {
            if (static_cast<double>(CellsAlong(size.x, cell)) * CellsAlong(size.y, cell) <= maxColumns)
                return cell;
        }
        return kCellSizes[sizeof(kCellSizes) / sizeof(kCellSizes[0]) - 1];
    }

    /////////////////////////////////////////////////////////////////////////
    // VoxelStock

    void VoxelStock::Reset(const StockBox& box, double cell, bool divide)
    {
        const Vec3 size = box.upper - box.lower;
        m_box = box;
        m_lower = box.lower;
        m_cell = cell;
        m_nx = CellsAlong(size.x, cell);
        m_ny = CellsAlong(size.y, cell);
        m_nz = CellsAlong(size.z, cell);
        m_bz = (m_nz + kWord - 1) / kWord;
        m_divide = divide;
        const size_t bricks = static_cast<size_t>(BricksX()) * BricksY() * m_bz;
        m_bricks.assign(bricks, kEmpty);
        m_pool.clear();
        m_free.clear();
        m_touched.assign(bricks, 0);
        m_touchedList.clear();
        m_dirty.assign(static_cast<size_t>(ChunkCount()), 1);
        if (!m_divide)
        {
            m_pool.reserve(bricks * 64);
            for (size_t b = 0; b < bricks; ++b)
                Split(b);
        }
    }

    std::uint64_t VoxelStock::TopMask(int w) const
    {
        if (w == m_bz - 1 && m_nz % kWord != 0)
            return (std::uint64_t(1) << (m_nz % kWord)) - 1;
        return ~std::uint64_t(0);
    }

    std::uint64_t VoxelStock::FullWord(int i, int j, int w) const
    {
        return (i < m_nx && j < m_ny) ? TopMask(w) : 0;
    }

    std::int32_t VoxelStock::Split(size_t brick)
    {
        const std::int32_t state = m_bricks[brick];
        if (state >= 0)
            return state;
        std::int32_t block;
        if (!m_free.empty())
        {
            block = m_free.back();
            m_free.pop_back();
        }
        else
        {
            block = static_cast<std::int32_t>(m_pool.size() / 64);
            m_pool.resize(m_pool.size() + 64);
        }
        const int bx = static_cast<int>(brick % BricksX());
        const int by = static_cast<int>((brick / BricksX()) % BricksY());
        const int w = static_cast<int>(brick / (static_cast<size_t>(BricksX()) * BricksY()));
        std::uint64_t* words = &m_pool[static_cast<size_t>(block) * 64];
        for (int y = 0; y < kBrick; ++y)
        {
            for (int x = 0; x < kBrick; ++x)
                words[y * kBrick + x] = (state == kFull) ? FullWord(bx * kBrick + x, by * kBrick + y, w) : 0;
        }
        m_bricks[brick] = block;
        return block;
    }

    void VoxelStock::MergeBrick(size_t brick)
    {
        const std::int32_t block = m_bricks[brick];
        if (block < 0)
            return;
        const int bx = static_cast<int>(brick % BricksX());
        const int by = static_cast<int>((brick / BricksX()) % BricksY());
        const int w = static_cast<int>(brick / (static_cast<size_t>(BricksX()) * BricksY()));
        const std::uint64_t* words = &m_pool[static_cast<size_t>(block) * 64];
        bool empty = true;
        bool full = true;
        for (int y = 0; y < kBrick && (empty || full); ++y)
        {
            for (int x = 0; x < kBrick; ++x)
            {
                const std::uint64_t word = words[y * kBrick + x];
                empty = empty && word == 0;
                full = full && word == FullWord(bx * kBrick + x, by * kBrick + y, w);
            }
        }
        if (empty || full)
        {
            m_bricks[brick] = empty ? kEmpty : kFull;
            m_free.push_back(block);
        }
    }

    void VoxelStock::Settle()
    {
        if (m_divide)
        {
            for (size_t brick : m_touchedList)
                MergeBrick(brick);
        }
        for (size_t brick : m_touchedList)
            m_touched[brick] = 0;
        m_touchedList.clear();
    }

    void VoxelStock::FillAll()
    {
        if (m_divide)
        {
            std::fill(m_bricks.begin(), m_bricks.end(), kFull);
            m_pool.clear();
            m_free.clear();
        }
        else
        {
            for (size_t b = 0; b < m_bricks.size(); ++b)
            {
                m_bricks[b] = kFull;
                m_free.push_back(static_cast<std::int32_t>(b));
            }
            // Split them again in the same order, so brick b has block b.
            std::reverse(m_free.begin(), m_free.end());
            for (size_t b = 0; b < m_bricks.size(); ++b)
                Split(b);
        }
        MarkAllDirty();
    }

    void VoxelStock::SetMask(int i, int j, int w, std::uint64_t mask)
    {
        mask &= FullWord(i, j, w);
        if (mask == 0)
            return;
        const size_t brick = BrickOf(i, j, w);
        if (m_bricks[brick] == kFull)
            return;
        const std::int32_t block = Split(brick);
        m_pool[static_cast<size_t>(block) * 64 + WordInBrick(i, j)] |= mask;
        if (!m_touched[brick])
        {
            m_touched[brick] = 1;
            m_touchedList.push_back(brick);
        }
    }

    void VoxelStock::FillMesh(const std::vector<Vec3>& triangles)
    {
        // Start empty.
        if (m_divide)
        {
            std::fill(m_bricks.begin(), m_bricks.end(), kEmpty);
            m_pool.clear();
            m_free.clear();
        }
        else
        {
            std::fill(m_pool.begin(), m_pool.end(), 0);
        }

        // The triangles crossing each row of column centres.
        std::vector<std::vector<int>> rows(static_cast<size_t>(m_ny));
        for (size_t t = 0; t + 2 < triangles.size(); t += 3)
        {
            const Vec3& a = triangles[t];
            const Vec3& b = triangles[t + 1];
            const Vec3& c = triangles[t + 2];
            const double minY = (std::min)({ a.y, b.y, c.y }), maxY = (std::max)({ a.y, b.y, c.y });
            const int j0 = (std::max)(0, static_cast<int>(std::floor((minY - m_lower.y) / m_cell - 0.5)));
            const int j1 = (std::min)(m_ny - 1, static_cast<int>(std::ceil((maxY - m_lower.y) / m_cell - 0.5)));
            for (int j = j0; j <= j1; ++j)
                rows[static_cast<size_t>(j)].push_back(static_cast<int>(t));
        }

        // A row at a time: where each column's centre line crosses the mesh. The
        // centre is nudged a little so it doesn't run exactly along an edge.
        const double nudgeX = m_cell * 1.37e-6;
        const double nudgeY = m_cell * 0.71e-6;
        std::vector<std::vector<float>> hits(static_cast<size_t>(m_nx));
        for (int j = 0; j < m_ny; ++j)
        {
            if (rows[static_cast<size_t>(j)].empty())
                continue;
            for (auto& column : hits)
                column.clear();
            const double y = m_lower.y + (j + 0.5) * m_cell + nudgeY;
            for (int t : rows[static_cast<size_t>(j)])
            {
                const Vec3& a = triangles[static_cast<size_t>(t)];
                const Vec3& b = triangles[static_cast<size_t>(t) + 1];
                const Vec3& c = triangles[static_cast<size_t>(t) + 2];
                const double area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
                if (std::fabs(area) < 1e-18)
                    continue;
                const double minX = (std::min)({ a.x, b.x, c.x }), maxX = (std::max)({ a.x, b.x, c.x });
                const int i0 = (std::max)(0, static_cast<int>(std::floor((minX - m_lower.x) / m_cell - 0.5)));
                const int i1 = (std::min)(m_nx - 1, static_cast<int>(std::ceil((maxX - m_lower.x) / m_cell - 0.5)));
                for (int i = i0; i <= i1; ++i)
                {
                    const double x = m_lower.x + (i + 0.5) * m_cell + nudgeX;
                    // Barycentric weights; all of one sign means inside.
                    const double wa = ((b.x - x) * (c.y - y) - (c.x - x) * (b.y - y)) / area;
                    const double wb = ((c.x - x) * (a.y - y) - (a.x - x) * (c.y - y)) / area;
                    const double wc = 1.0 - wa - wb;
                    if (wa < 0.0 || wb < 0.0 || wc < 0.0)
                        continue;
                    hits[static_cast<size_t>(i)].push_back(static_cast<float>(wa * a.z + wb * b.z + wc * c.z));
                }
            }

            // Between each pair of crossings is inside.
            for (int i = 0; i < m_nx; ++i)
            {
                std::vector<float>& column = hits[static_cast<size_t>(i)];
                std::sort(column.begin(), column.end());
                for (size_t h = 0; h + 1 < column.size(); h += 2)
                {
                    const int k0 = (std::max)(0, static_cast<int>(std::ceil((column[h] - m_lower.z) / m_cell - 0.5)));
                    const int k1 =
                        (std::min)(m_nz - 1, static_cast<int>(std::floor((column[h + 1] - m_lower.z) / m_cell - 0.5)));
                    for (int w = k0 / kWord; k0 <= k1 && w <= k1 / kWord; ++w)
                        SetMask(i, j, w, SpanMask(w, k0, k1));
                }
            }
        }
        Settle();
        MarkAllDirty();
    }

    std::uint64_t VoxelStock::SpanMask(int w, int k0, int k1)
    {
        const int from = (std::max)(k0 - w * kWord, 0);
        const int to = (std::min)(k1 - w * kWord, kWord - 1);
        if (from > to)
            return 0;
        const std::uint64_t upper = (to == kWord - 1) ? ~std::uint64_t(0) : ((std::uint64_t(1) << (to + 1)) - 1);
        return upper & ~((std::uint64_t(1) << from) - 1);
    }

    std::uint64_t VoxelStock::Word(int i, int j, int w) const
    {
        if (i < 0 || j < 0 || w < 0 || i >= m_nx || j >= m_ny || w >= m_bz)
            return 0;
        const std::int32_t state = m_bricks[BrickOf(i, j, w)];
        if (state == kEmpty)
            return 0;
        if (state == kFull)
            return TopMask(w);
        return m_pool[static_cast<size_t>(state) * 64 + WordInBrick(i, j)];
    }

    void VoxelStock::ReadWords(int i0, int j0, int ni, int nj, int w, std::uint64_t* out) const
    {
        if (w < 0 || w >= m_bz)
        {
            std::fill(out, out + static_cast<size_t>(ni) * nj, 0);
            return;
        }
        const std::uint64_t top = TopMask(w);
        for (int y = 0; y < nj; ++y)
        {
            const int j = j0 + y;
            std::uint64_t* row = out + static_cast<size_t>(y) * ni;
            if (j < 0 || j >= m_ny)
            {
                std::fill(row, row + ni, 0);
                continue;
            }
            const size_t brickRow = (static_cast<size_t>(w) * BricksY() + j / kBrick) * BricksX();
            const size_t wordRow = static_cast<size_t>(j % kBrick) * kBrick;
            for (int x = 0; x < ni; ++x)
            {
                const int i = i0 + x;
                if (i < 0 || i >= m_nx)
                {
                    row[x] = 0;
                    continue;
                }
                const std::int32_t state = m_bricks[brickRow + i / kBrick];
                row[x] = (state == kEmpty) ? 0
                         : (state == kFull) ? top
                                            : m_pool[static_cast<size_t>(state) * 64 + wordRow + i % kBrick];
            }
        }
    }

    long long VoxelStock::ClearMask(int i, int j, int w, std::uint64_t mask)
    {
        if (i < 0 || j < 0 || w < 0 || i >= m_nx || j >= m_ny || w >= m_bz)
            return 0;
        const std::uint64_t hit = Word(i, j, w) & mask;
        if (hit == 0)
            return 0;
        const size_t brick = BrickOf(i, j, w);
        const std::int32_t block = Split(brick);
        m_pool[static_cast<size_t>(block) * 64 + WordInBrick(i, j)] &= ~hit;
        if (!m_touched[brick])
        {
            m_touched[brick] = 1;
            m_touchedList.push_back(brick);
        }
        return CountBits(hit);
    }

    bool VoxelStock::Filled(int i, int j, int k) const
    {
        if (k < 0 || k >= m_nz)
            return false;
        return ((Word(i, j, k / kWord) >> (k % kWord)) & 1) != 0;
    }

    void VoxelStock::Clear(int i, int j, int k)
    {
        if (k < 0 || k >= m_nz)
            return;
        ClearMask(i, j, k / kWord, std::uint64_t(1) << (k % kWord));
        Settle();
        MarkDirty(i, j, k);
    }

    Vec3 VoxelStock::Centre(int i, int j, int k) const
    {
        return { m_lower.x + (i + 0.5) * m_cell, m_lower.y + (j + 0.5) * m_cell, m_lower.z + (k + 0.5) * m_cell };
    }

    long long VoxelStock::Count() const
    {
        long long count = 0;
        for (size_t brick = 0; brick < m_bricks.size(); ++brick)
        {
            const std::int32_t state = m_bricks[brick];
            if (state == kEmpty)
                continue;
            const int bx = static_cast<int>(brick % BricksX());
            const int by = static_cast<int>((brick / BricksX()) % BricksY());
            const int w = static_cast<int>(brick / (static_cast<size_t>(BricksX()) * BricksY()));
            if (state == kFull)
            {
                const long long columns = static_cast<long long>((std::min)(kBrick, m_nx - bx * kBrick)) *
                                          (std::min)(kBrick, m_ny - by * kBrick);
                count += columns * CountBits(TopMask(w));
                continue;
            }
            for (int q = 0; q < 64; ++q)
                count += CountBits(m_pool[static_cast<size_t>(state) * 64 + q]);
        }
        return count;
    }

    int VoxelStock::MixedBricks() const
    {
        return static_cast<int>(std::count_if(m_bricks.begin(), m_bricks.end(), [](std::int32_t s) { return s >= 0; }));
    }

    int VoxelStock::FullBricks() const
    {
        return static_cast<int>(std::count(m_bricks.begin(), m_bricks.end(), kFull));
    }

    void VoxelStock::MarkDirty(int i, int j, int k)
    {
        MarkDirtyBox(i, j, k, i, j, k);
    }

    void VoxelStock::MarkDirtyBox(int i0, int j0, int k0, int i1, int j1, int k1)
    {
        // A cut changes its own chunk, and a neighbour's surface where it's on a chunk's edge.
        i0 = (std::max)(0, i0 - 1);
        j0 = (std::max)(0, j0 - 1);
        k0 = (std::max)(0, k0 - 1);
        i1 = (std::min)(m_nx - 1, i1 + 1);
        j1 = (std::min)(m_ny - 1, j1 + 1);
        k1 = (std::min)(m_nz - 1, k1 + 1);
        if (i0 > i1 || j0 > j1 || k0 > k1)
            return;
        for (int cz = k0 / kWord; cz <= k1 / kWord; ++cz)
        {
            for (int cy = j0 / kChunk; cy <= j1 / kChunk; ++cy)
            {
                for (int cx = i0 / kChunk; cx <= i1 / kChunk; ++cx)
                    m_dirty[static_cast<size_t>((cz * ChunksY() + cy) * ChunksX() + cx)] = 1;
            }
        }
    }

    void VoxelStock::MarkAllDirty()
    {
        std::fill(m_dirty.begin(), m_dirty.end(), 1);
    }

    /////////////////////////////////////////////////////////////////////////
    // The tool

    double ToolProfile::RadiusAt(double h) const
    {
        if (h < -1e-9 || h > length)
            return -1.0;
        h = (std::max)(0.0, h);
        switch (type)
        {
        case ToolType::BallMill:
            return (h >= radius) ? radius : std::sqrt((std::max)(0.0, radius * radius - (radius - h) * (radius - h)));
        case ToolType::CornerRadiusMill:
        {
            const double r = (std::min)(cornerRadius, radius);
            if (h >= r)
                return radius;
            return (radius - r) + std::sqrt((std::max)(0.0, r * r - (r - h) * (r - h)));
        }
        case ToolType::VMill:
        case ToolType::ChamferMill:
        case ToolType::TaperMill:
            return (std::min)(radius, tipRadius + h * slope);
        case ToolType::Drill:
            return (std::min)(radius, h * slope);
        default:
            return radius;
        }
    }

    double ToolProfile::HeightForRadius(double r) const
    {
        if (r <= 0.0)
            return 0.0;
        if (r > radius)
            return -1.0;
        double h = 0.0;
        switch (type)
        {
        case ToolType::BallMill:
            h = radius - std::sqrt((std::max)(0.0, radius * radius - r * r));
            break;
        case ToolType::CornerRadiusMill:
        {
            const double c = (std::min)(cornerRadius, radius);
            const double flat = radius - c;
            if (r > flat)
                h = c - std::sqrt((std::max)(0.0, c * c - (r - flat) * (r - flat)));
            break;
        }
        case ToolType::VMill:
        case ToolType::ChamferMill:
        case ToolType::TaperMill:
            if (r > tipRadius)
            {
                if (slope <= 0.0)
                    return -1.0;
                h = (r - tipRadius) / slope;
            }
            break;
        case ToolType::Drill:
            if (slope <= 0.0)
                return -1.0;
            h = r / slope;
            break;
        default:
            break;
        }
        return (h > length) ? -1.0 : h;
    }

    ToolProfile ProfileOf(const Tool& tool)
    {
        ToolProfile profile;
        profile.type = tool.type;
        profile.radius = tool.diameter / 2.0;
        profile.length = (tool.overallLength > 0.0) ? tool.overallLength
                                                    : (std::max)(tool.fluteLength, 4.0 * profile.radius);
        profile.cornerRadius = tool.cornerRadius;
        profile.tipRadius = tool.tipDiameter / 2.0;
        switch (tool.type)
        {
        case ToolType::VMill:
        case ToolType::ChamferMill:
            profile.slope = std::tan(tool.includedAngle / 2.0 * kPi / 180.0);
            break;
        case ToolType::TaperMill:
            profile.slope = std::tan(tool.taperAngle * kPi / 180.0);
            break;
        case ToolType::Drill:
            profile.slope = std::tan(tool.pointAngle / 2.0 * kPi / 180.0);
            break;
        default:
            break;
        }
        return profile;
    }

    long long CutMove(VoxelStock& stock, const ToolProfile& profile, const Vec3& from, const Vec3& to,
                      const Vec3& axis, bool generalOnly)
    {
        Vec3 w;
        if (!Normalise(axis, w) || stock.SizeX() == 0)
            return 0;

        // A tool along the grid's Z is cut a column at a time.
        const bool alongZ = !generalOnly && std::fabs(w.x) < 1e-9 && std::fabs(w.y) < 1e-9;
        if (alongZ)
            w = { 0.0, 0.0, (w.z > 0.0) ? 1.0 : -1.0 };
        const double sign = w.z;

        // Short steps: none moving more than half a cell along the axis, or more
        // than a couple of tool widths across it, so each step's box is tight. A
        // move straight along the axis is one step.
        const double cell = stock.Cell();
        const Vec3 d = to - from;
        const double axial = Dot(d, w);
        const double across = Length(d - w * axial);
        const double reach = (std::max)(profile.radius, cell);
        const int axialSteps = (across < 1e-9) ? 1 : static_cast<int>(std::ceil(std::fabs(axial) / (0.5 * cell)));
        const int acrossSteps = (alongZ && std::fabs(axial) < 1e-12) ? 1 : static_cast<int>(std::ceil(across / (2.0 * reach)));
        const int steps = (std::max)({ 1, axialSteps, acrossSteps });

        long long removed = 0;
        const Vec3 lower = stock.Lower();
        for (int s = 0; s < steps; ++s)
        {
            const Vec3 a = from + d * (static_cast<double>(s) / steps);
            const Vec3 b = from + d * (static_cast<double>(s + 1) / steps);

            // The step as a sweep square to the axis. The tool reaches from the
            // lower tip, at its shape there, up to its length above the higher.
            const double tipLow = (std::min)(Dot(a, w), Dot(b, w));
            const double tipHigh = (std::max)(Dot(a, w), Dot(b, w));
            const Vec3 pa = a - w * Dot(a, w);
            const Vec3 pb = b - w * Dot(b, w);
            const auto inside = [&](double along, double distance) {
                if (along - tipHigh > profile.length)
                    return false;
                const double radius = profile.RadiusAt((std::min)(along - tipLow, profile.length));
                return radius >= 0.0 && distance <= radius + 1e-9;
            };

            // The cells the tool can reach in this step.
            Vec3 lo = a, hi = a;
            for (const Vec3& p : { a, b, a + w * profile.length, b + w * profile.length })
            {
                lo = { (std::min)(lo.x, p.x), (std::min)(lo.y, p.y), (std::min)(lo.z, p.z) };
                hi = { (std::max)(hi.x, p.x), (std::max)(hi.y, p.y), (std::max)(hi.z, p.z) };
            }
            const double r = profile.radius;
            const int i0 = (std::max)(0, static_cast<int>(std::floor((lo.x - r - lower.x) / cell)));
            const int j0 = (std::max)(0, static_cast<int>(std::floor((lo.y - r - lower.y) / cell)));
            const int k0 = (std::max)(0, static_cast<int>(std::floor((lo.z - r - lower.z) / cell)));
            const int i1 = (std::min)(stock.SizeX() - 1, static_cast<int>(std::floor((hi.x + r - lower.x) / cell)));
            const int j1 = (std::min)(stock.SizeY() - 1, static_cast<int>(std::floor((hi.y + r - lower.y) / cell)));
            const int k1 = (std::min)(stock.SizeZ() - 1, static_cast<int>(std::floor((hi.z + r - lower.z) / cell)));
            if (i0 > i1 || j0 > j1 || k0 > k1)
                continue;
            const int w0 = k0 / VoxelStock::kWord;
            const int w1 = k1 / VoxelStock::kWord;

            long long stepRemoved = 0;
            for (int j = j0; j <= j1; ++j)
            {
                int rowI0 = i0, rowI1 = i1;
                if (alongZ)
                {
                    double xLo, xHi;
                    if (!CapsuleRow(pa, pb, r + 1e-6, lower.y + (j + 0.5) * cell, xLo, xHi))
                        continue;
                    rowI0 = (std::max)(i0, static_cast<int>(std::floor((xLo - lower.x) / cell - 0.5)));
                    rowI1 = (std::min)(i1, static_cast<int>(std::ceil((xHi - lower.x) / cell - 0.5)));
                }
                for (int i = rowI0; i <= rowI1; ++i)
                {
                    double distance = 0.0;
                    if (alongZ)
                    {
                        const Vec3 c = stock.Centre(i, j, 0);
                        distance = DistanceToSegment({ c.x, c.y, 0.0 }, pa, pb);
                        if (distance > r + 1e-9)
                            continue;
                    }
                    bool any = false;
                    for (int q = w0; q <= w1 && !any; ++q)
                        any = (stock.Word(i, j, q) & VoxelStock::SpanMask(q, k0, k1)) != 0;
                    if (!any)
                        continue;

                    if (alongZ)
                    {
                        // The column's distance from the step's line, and the span
                        // of it inside the tool.
                        const double h = profile.HeightForRadius(distance - 1e-9);
                        if (h < 0.0 && !inside(tipLow + profile.length, distance))
                            continue;
                        const double alongLow = tipLow + (std::max)(0.0, h);
                        const double alongHigh = tipHigh + profile.length;
                        const double zLow = (sign > 0.0) ? alongLow : -alongHigh;
                        const double zHigh = (sign > 0.0) ? alongHigh : -alongLow;
                        int kA = (std::max)(k0, static_cast<int>(std::ceil((zLow - lower.z) / cell - 0.5)));
                        int kB = (std::min)(k1, static_cast<int>(std::floor((zHigh - lower.z) / cell - 0.5)));
                        const auto test = [&](int k) {
                            return inside(sign * (lower.z + (k + 0.5) * cell), distance);
                        };
                        // Settle the ends exactly, cell by cell.
                        kA = (std::min)(kA, k1 + 1);
                        kB = (std::max)(kB, k0 - 1);
                        while (kA > k0 && test(kA - 1))
                            --kA;
                        while (kA <= k1 && kA <= kB && !test(kA))
                            ++kA;
                        while (kB < k1 && kB >= kA && test(kB + 1))
                            ++kB;
                        while (kB >= kA && !test(kB))
                            --kB;
                        if (kA > kB)
                        {
                            // The estimate missed: look along the column.
                            for (kA = k0; kA <= k1 && !test(kA); ++kA)
                            {
                            }
                            for (kB = kA; kB + 1 <= k1 && test(kB + 1); ++kB)
                            {
                            }
                            if (kA > k1)
                                continue;
                        }
                        for (int q = kA / VoxelStock::kWord; q <= kB / VoxelStock::kWord; ++q)
                            stepRemoved += stock.ClearMask(i, j, q, VoxelStock::SpanMask(q, kA, kB));
                        continue;
                    }

                    // Cell by cell, the filled ones only.
                    for (int q = w0; q <= w1; ++q)
                    {
                        std::uint64_t bits = stock.Word(i, j, q) & VoxelStock::SpanMask(q, k0, k1);
                        std::uint64_t clear = 0;
                        while (bits != 0)
                        {
                            const int bit = LowestBit(bits);
                            bits &= bits - 1;
                            const Vec3 c = stock.Centre(i, j, q * VoxelStock::kWord + bit);
                            const double along = Dot(c, w);
                            if (inside(along, DistanceToSegment(c - w * along, pa, pb)))
                                clear |= std::uint64_t(1) << bit;
                        }
                        if (clear != 0)
                            stepRemoved += stock.ClearMask(i, j, q, clear);
                    }
                }
            }
            if (stepRemoved > 0)
            {
                stock.MarkDirtyBox(i0, j0, k0, i1, j1, k1);
                removed += stepRemoved;
            }
        }
        stock.Settle();
        return removed;
    }

    /////////////////////////////////////////////////////////////////////////
    // Meshing

    namespace
    {
        // Merges one plane of faces into rectangles, greedily. The faces lie across
        // axis a, facing sign s, in layer `layer` of the chunk; mask is
        // size[v] rows of size[u], with u = a + 1 and v = a + 2 (mod 3).
        void MergePlane(StockMesh& mesh, std::vector<char>& mask, int a, int s, int layer, const int start[3],
                        const int size[3], const double lower[3], double cell)
        {
            const int u = (a + 1) % 3;
            const int v = (a + 2) % 3;
            for (int y = 0; y < size[v]; ++y)
            {
                for (int x = 0; x < size[u];)
                {
                    if (!mask[static_cast<size_t>(y) * size[u] + x])
                    {
                        ++x;
                        continue;
                    }
                    int width = 1;
                    while (x + width < size[u] && mask[static_cast<size_t>(y) * size[u] + x + width])
                        ++width;
                    int height = 1;
                    for (bool grow = true; grow && y + height < size[v];)
                    {
                        for (int q = 0; q < width; ++q)
                        {
                            if (!mask[static_cast<size_t>(y + height) * size[u] + x + q])
                            {
                                grow = false;
                                break;
                            }
                        }
                        if (grow)
                            ++height;
                    }
                    for (int dy = 0; dy < height; ++dy)
                    {
                        for (int q = 0; q < width; ++q)
                            mask[static_cast<size_t>(y + dy) * size[u] + x + q] = 0;
                    }

                    // The rectangle's corners, anticlockwise seen from outside.
                    const double plane = lower[a] + (start[a] + layer + (s > 0 ? 1 : 0)) * cell;
                    const double u0 = lower[u] + (start[u] + x) * cell, u1 = u0 + width * cell;
                    const double v0 = lower[v] + (start[v] + y) * cell, v1 = v0 + height * cell;
                    double corners[4][3];
                    const double us[4] = { u0, u1, u1, u0 };
                    const double vs[4] = { v0, v0, v1, v1 };
                    for (int c = 0; c < 4; ++c)
                    {
                        corners[c][a] = plane;
                        corners[c][u] = us[c];
                        corners[c][v] = vs[c];
                    }
                    const int base = static_cast<int>(mesh.vertices.size() / 3);
                    for (int c = 0; c < 4; ++c)
                    {
                        const int corner = (s > 0) ? c : 3 - c;
                        for (int axisIndex = 0; axisIndex < 3; ++axisIndex)
                        {
                            mesh.vertices.push_back(static_cast<float>(corners[corner][axisIndex]));
                            mesh.normals.push_back(axisIndex == a ? static_cast<float>(s) : 0.0f);
                        }
                    }
                    mesh.faces.insert(mesh.faces.end(), { 3, base, base + 1, base + 2, 3, base, base + 2, base + 3 });
                    x += width;
                }
            }
        }
    }

    StockMesh MeshChunk(const VoxelStock& stock, int chunk)
    {
        StockMesh mesh;
        const int n = VoxelStock::kChunk;
        const int cx = chunk % stock.ChunksX();
        const int cy = (chunk / stock.ChunksX()) % stock.ChunksY();
        const int cz = chunk / (stock.ChunksX() * stock.ChunksY());
        const int start[3] = { cx * n, cy * n, cz * VoxelStock::kWord };
        const int size[3] = { (std::min)(n, stock.SizeX() - start[0]), (std::min)(n, stock.SizeY() - start[1]),
                              (std::min)(VoxelStock::kWord, stock.SizeZ() - start[2]) };
        const double lower[3] = { stock.Lower().x, stock.Lower().y, stock.Lower().z };
        const int ni = size[0];
        const int nj = size[1];

        // The chunk's columns with a ring of their neighbours, and the words above
        // and below. The faces of each column are then found 64 cells at a time: a
        // filled cell whose neighbour that way is empty (or off the grid).
        const int pi = ni + 2;
        std::vector<std::uint64_t> padded(static_cast<size_t>(pi) * (nj + 2));
        stock.ReadWords(start[0] - 1, start[1] - 1, pi, nj + 2, cz, padded.data());
        bool anyFilled = false;
        for (int y = 0; y < nj && !anyFilled; ++y)
        {
            for (int x = 0; x < ni && !anyFilled; ++x)
                anyFilled = padded[static_cast<size_t>(y + 1) * pi + x + 1] != 0;
        }
        if (!anyFilled)
            return mesh;
        std::vector<std::uint64_t> above(static_cast<size_t>(ni) * nj, 0);
        std::vector<std::uint64_t> below(static_cast<size_t>(ni) * nj, 0);
        stock.ReadWords(start[0], start[1], ni, nj, cz + 1, above.data());
        stock.ReadWords(start[0], start[1], ni, nj, cz - 1, below.data());
        const auto at = [&](int x, int y) { return padded[static_cast<size_t>(y + 1) * pi + x + 1]; };

        std::vector<std::uint64_t> faces(static_cast<size_t>(ni) * nj);
        std::vector<char> mask;
        for (int a = 0; a < 3; ++a)
        {
            const int u = (a + 1) % 3;
            const int v = (a + 2) % 3;
            for (int s = -1; s <= 1; s += 2)
            {
                std::uint64_t anyFace = 0;
                for (int y = 0; y < nj; ++y)
                {
                    for (int x = 0; x < ni; ++x)
                    {
                        const std::uint64_t word = at(x, y);
                        const size_t q = static_cast<size_t>(y) * ni + x;
                        std::uint64_t neighbour;
                        if (a == 2)
                            neighbour = (s > 0) ? ((word >> 1) | ((above[q] & 1) << 63)) : ((word << 1) | (below[q] >> 63));
                        else if (a == 0)
                            neighbour = at(x + s, y);
                        else
                            neighbour = at(x, y + s);
                        const std::uint64_t face = word & ~neighbour;
                        faces[q] = face;
                        anyFace |= face;
                    }
                }
                if (anyFace == 0)
                    continue;

                // MergePlane clears what it merges, so the mask stays empty between
                // layers, and only faces need setting.
                mask.assign(static_cast<size_t>(size[u]) * size[v], 0);
                if (a == 2)
                {
                    // Layers up the word: mask rows are Y, columns X.
                    for (std::uint64_t layers = anyFace; layers != 0; layers &= layers - 1)
                    {
                        const int layer = LowestBit(layers);
                        const std::uint64_t bit = std::uint64_t(1) << layer;
                        for (size_t q = 0; q < faces.size(); ++q)
                        {
                            if (faces[q] & bit)
                                mask[q] = 1;
                        }
                        MergePlane(mesh, mask, a, s, layer, start, size, lower, stock.Cell());
                    }
                }
                else if (a == 0)
                {
                    // A layer for each X: mask rows are Z, columns Y.
                    for (int x = 0; x < ni; ++x)
                    {
                        bool found = false;
                        for (int y = 0; y < nj; ++y)
                        {
                            for (std::uint64_t face = faces[static_cast<size_t>(y) * ni + x]; face != 0; face &= face - 1)
                            {
                                mask[static_cast<size_t>(LowestBit(face)) * nj + y] = 1;
                                found = true;
                            }
                        }
                        if (found)
                            MergePlane(mesh, mask, a, s, x, start, size, lower, stock.Cell());
                    }
                }
                else
                {
                    // A layer for each Y: mask rows are X, columns Z.
                    for (int y = 0; y < nj; ++y)
                    {
                        bool found = false;
                        for (int x = 0; x < ni; ++x)
                        {
                            for (std::uint64_t face = faces[static_cast<size_t>(y) * ni + x]; face != 0; face &= face - 1)
                            {
                                mask[static_cast<size_t>(x) * size[2] + LowestBit(face)] = 1;
                                found = true;
                            }
                        }
                        if (found)
                            MergePlane(mesh, mask, a, s, y, start, size, lower, stock.Cell());
                    }
                }
            }
        }
        return mesh;
    }

    void MeshChunks(const VoxelStock& stock, const std::vector<int>& chunks, std::vector<StockMesh>& meshes)
    {
        meshes.resize((std::max)(meshes.size(), static_cast<size_t>(stock.ChunkCount())));
        std::for_each(std::execution::par, chunks.begin(), chunks.end(),
                      [&](int chunk) { meshes[static_cast<size_t>(chunk)] = MeshChunk(stock, chunk); });
    }

    std::vector<int> DirtyChunks(const VoxelStock& stock)
    {
        std::vector<int> chunks;
        for (int c = 0; c < stock.ChunkCount(); ++c)
        {
            if (stock.ChunkDirty(c))
                chunks.push_back(c);
        }
        return chunks;
    }

    StockMesh JoinMeshes(const std::vector<StockMesh>& chunks)
    {
        StockMesh mesh;
        for (const StockMesh& chunk : chunks)
        {
            const int base = static_cast<int>(mesh.vertices.size() / 3);
            mesh.vertices.insert(mesh.vertices.end(), chunk.vertices.begin(), chunk.vertices.end());
            mesh.normals.insert(mesh.normals.end(), chunk.normals.begin(), chunk.normals.end());
            for (size_t f = 0; f + 3 < chunk.faces.size(); f += 4)
                mesh.faces.insert(mesh.faces.end(),
                                  { 3, chunk.faces[f + 1] + base, chunk.faces[f + 2] + base, chunk.faces[f + 3] + base });
        }
        return mesh;
    }

    StockMesh MeshStock(const VoxelStock& stock)
    {
        std::vector<int> all(static_cast<size_t>(stock.ChunkCount()));
        for (size_t c = 0; c < all.size(); ++c)
            all[c] = static_cast<int>(c);
        std::vector<StockMesh> chunks;
        MeshChunks(stock, all, chunks);
        return JoinMeshes(chunks);
    }

    StockDifference CompareStock(const VoxelStock& stock, const VoxelStock& part)
    {
        StockDifference difference;
        difference.notEnough.ResetLike(stock);
        difference.tooMuch.ResetLike(stock);
        const int n = VoxelStock::kChunk;
        const int count = stock.ChunkCount();

        // Each chunk on its own core, keeping the columns that differ.
        struct Column
        {
            int i;
            int j;
            std::uint64_t notEnough;
            std::uint64_t tooMuch;
        };
        std::vector<std::vector<Column>> found(static_cast<size_t>(count));
        std::vector<int> chunks(static_cast<size_t>(count));
        for (int c = 0; c < count; ++c)
            chunks[static_cast<size_t>(c)] = c;
        std::for_each(std::execution::par, chunks.begin(), chunks.end(), [&](int chunk) {
            const int cx = chunk % stock.ChunksX();
            const int cy = (chunk / stock.ChunksX()) % stock.ChunksY();
            const int w = chunk / (stock.ChunksX() * stock.ChunksY());
            const int i0 = cx * n;
            const int j0 = cy * n;
            const int ni = (std::min)(n, stock.SizeX() - i0);
            const int nj = (std::min)(n, stock.SizeY() - j0);
            const int pi = ni + 2;
            const int pj = nj + 2;
            const size_t padded = static_cast<size_t>(pi) * pj;

            // The part's words with a ring round them, and those above and below.
            std::vector<std::uint64_t> here(padded), above(padded), below(padded);
            part.ReadWords(i0 - 1, j0 - 1, pi, pj, w, here.data());
            part.ReadWords(i0 - 1, j0 - 1, pi, pj, w + 1, above.data());
            part.ReadWords(i0 - 1, j0 - 1, pi, pj, w - 1, below.data());

            // Grown and shrunk up and down each column first.
            std::vector<std::uint64_t> grownZ(padded), shrunkZ(padded);
            bool anyPart = false;
            for (size_t q = 0; q < padded; ++q)
            {
                const std::uint64_t word = here[q];
                const std::uint64_t up = (word >> 1) | ((above[q] & 1) << 63);      // each cell's neighbour above
                const std::uint64_t down = (word << 1) | (below[q] >> 63);          // and below
                grownZ[q] = word | up | down;
                shrunkZ[q] = word & up & down;
                anyPart = anyPart || word != 0;
            }

            std::vector<std::uint64_t> stockWords(static_cast<size_t>(ni) * nj);
            stock.ReadWords(i0, j0, ni, nj, w, stockWords.data());
            std::vector<Column>& columns = found[static_cast<size_t>(chunk)];
            for (int y = 0; y < nj; ++y)
            {
                for (int x = 0; x < ni; ++x)
                {
                    std::uint64_t grown = 0;
                    std::uint64_t shrunk = ~std::uint64_t(0);
                    if (anyPart)
                    {
                        for (int dy = 0; dy < 3; ++dy)
                        {
                            for (int dx = 0; dx < 3; ++dx)
                            {
                                const size_t q = static_cast<size_t>(y + dy) * pi + x + dx;
                                grown |= grownZ[q];
                                shrunk &= shrunkZ[q];
                            }
                        }
                    }
                    else
                    {
                        shrunk = 0;
                    }
                    const std::uint64_t cells = stockWords[static_cast<size_t>(y) * ni + x];
                    const std::uint64_t notEnough = cells & ~grown;
                    const std::uint64_t tooMuch = shrunk & ~cells;
                    if (notEnough != 0 || tooMuch != 0)
                        columns.push_back({ i0 + x, j0 + y, notEnough, tooMuch });
                }
            }
        });

        // Into the grids, one at a time: splitting bricks isn't safe on several cores.
        for (int chunk = 0; chunk < count; ++chunk)
        {
            const int w = chunk / (stock.ChunksX() * stock.ChunksY());
            for (const Column& column : found[static_cast<size_t>(chunk)])
            {
                if (column.notEnough != 0)
                    difference.notEnough.SetCells(column.i, column.j, w, column.notEnough);
                if (column.tooMuch != 0)
                    difference.tooMuch.SetCells(column.i, column.j, w, column.tooMuch);
            }
        }
        difference.notEnough.Settle();
        difference.tooMuch.Settle();
        const double cellVolume = stock.Cell() * stock.Cell() * stock.Cell();
        difference.notEnoughVolume = static_cast<double>(difference.notEnough.Count()) * cellVolume;
        difference.tooMuchVolume = static_cast<double>(difference.tooMuch.Count()) * cellVolume;
        return difference;
    }

    std::string StockMeshToStl(const StockMesh& mesh, const std::string& title)
    {
        const size_t triangles = mesh.TriangleCount();
        std::string bytes(84 + 50 * triangles, '\0');
        bytes.replace(0, (std::min)(title.size(), size_t(80)), title.substr(0, 80));
        const auto putUint32 = [&](size_t at, std::uint32_t value) {
            for (int b = 0; b < 4; ++b)
                bytes[at + b] = static_cast<char>((value >> (8 * b)) & 0xFF);
        };
        const auto putFloat = [&](size_t at, float value) {
            std::uint32_t raw;
            static_assert(sizeof(raw) == sizeof(value), "a float is 4 bytes");
            std::memcpy(&raw, &value, sizeof(raw));
            putUint32(at, raw);
        };
        putUint32(80, static_cast<std::uint32_t>(triangles));
        for (size_t t = 0; t < triangles; ++t)
        {
            const size_t at = 84 + 50 * t;
            const int* corners = &mesh.faces[t * 4 + 1];
            for (int a = 0; a < 3; ++a)
                putFloat(at + 4 * a, mesh.normals[static_cast<size_t>(corners[0]) * 3 + a]);
            for (int c = 0; c < 3; ++c)
            {
                for (int a = 0; a < 3; ++a)
                    putFloat(at + 12 + 12 * c + 4 * a, mesh.vertices[static_cast<size_t>(corners[c]) * 3 + a]);
            }
        }
        return bytes;
    }

    /////////////////////////////////////////////////////////////////////////
    // Stepping

    void Simulation::Start(const StockBox& box, const std::vector<Vec3>& stockTriangles,
                           const std::vector<SimOperation>& operations, const SimResolution& resolution)
    {
        double cell = resolution.cell;
        if (cell <= 0.0)
        {
            if (resolution.autoDivide)
            {
                cell = AutoDivideCell(box);
            }
            else
            {
                const std::vector<double> stops = ResolutionStops(box);
                cell = stops[stops.size() / 2];
            }
        }
        m_stock.Reset(box, cell, resolution.autoDivide);
        if (stockTriangles.empty())
            m_stock.FillAll();
        else
            m_stock.FillMesh(stockTriangles);
        m_start = m_stock;
        m_operations = operations;
        m_moves.clear();
        m_position = 0;
        m_warning = false;
        // Each operation's moves in assembly mm. Arcs become chords within half a
        // cell, and dwells go nowhere.
        const double tolerance = m_stock.Cell() / 2.0;
        for (size_t o = 0; o < m_operations.size(); ++o)
        {
            const SimOperation& operation = m_operations[o];
            bool havePrevious = false;
            Vec3 previous;
            for (size_t m = 0; m < operation.toolpath.moves.size(); ++m)
            {
                const ToolpathMove& move = operation.toolpath.moves[m];
                if (move.kind == MoveKind::Dwell)
                    continue;
                if (!havePrevious)
                    previous = move.to;     // where the operation starts; the move there isn't known

                std::vector<Vec3> points{ previous };
                if (move.arc)
                {
                    const Vec3 c = move.centre;
                    const double r = std::hypot(previous.x - c.x, previous.y - c.y);
                    double a0 = std::atan2(previous.y - c.y, previous.x - c.x);
                    double a1 = std::atan2(move.to.y - c.y, move.to.x - c.x);
                    double sweep = a1 - a0;
                    if (move.clockwise)
                    {
                        while (sweep > 0.0)
                            sweep -= 2.0 * kPi;
                    }
                    else
                    {
                        while (sweep < 0.0)
                            sweep += 2.0 * kPi;
                    }
                    const double step = (r > tolerance) ? 2.0 * std::acos(1.0 - tolerance / r) : kPi / 4.0;
                    const int count = (std::max)(1, static_cast<int>(std::ceil(std::fabs(sweep) / step)));
                    for (int i = 1; i < count; ++i)
                    {
                        const double t = static_cast<double>(i) / count;
                        const double angle = a0 + sweep * t;
                        points.push_back({ c.x + r * std::cos(angle), c.y + r * std::sin(angle),
                                           previous.z + (move.to.z - previous.z) * t });
                    }
                }
                points.push_back(move.to);

                for (size_t p = 0; p + 1 < points.size(); ++p)
                {
                    SimMove sim;
                    sim.from = FramePointToWorld(operation.frame, points[p]);
                    sim.to = FramePointToWorld(operation.frame, points[p + 1]);
                    sim.rapid = move.kind == MoveKind::Rapid;
                    sim.operation = o;
                    sim.toolpathMove = m;
                    m_moves.push_back(sim);
                }
                previous = move.to;
                havePrevious = true;
            }
        }
    }

    Vec3 Simulation::ToolTip() const
    {
        if (m_moves.empty())
            return Vec3();
        return (m_position < m_moves.size()) ? m_moves[m_position].from : m_moves.back().to;
    }

    const SimOperation* Simulation::CurrentOperation() const
    {
        if (m_moves.empty())
            return nullptr;
        const size_t at = (std::min)(m_position, m_moves.size() - 1);
        return &m_operations[m_moves[at].operation];
    }

    size_t Simulation::Step(size_t count)
    {
        size_t done = 0;
        while (done < count && m_position < m_moves.size())
        {
            const SimMove& move = m_moves[m_position];
            const SimOperation& operation = m_operations[move.operation];
            const long long removed = CutMove(m_stock, operation.profile, move.from, move.to, operation.frame.zAxis);
            ++m_position;
            ++done;
            if (move.rapid && removed > 0)
            {
                m_warning = true;
                m_warningMove = m_position - 1;
                break;
            }
        }
        return done;
    }

    size_t Simulation::RunToOperationEnd()
    {
        if (Finished())
            return 0;
        const size_t operation = m_moves[m_position].operation;
        size_t end = m_position;
        while (end < m_moves.size() && m_moves[end].operation == operation)
            ++end;
        return Step(end - m_position);
    }

    void Simulation::Rewind()
    {
        m_stock = m_start;
        m_stock.MarkAllDirty();
        m_position = 0;
        m_warning = false;
    }

    void Simulation::SeekTo(size_t position)
    {
        position = (std::min)(position, m_moves.size());
        if (position < m_position)
            Rewind();
        // Through any warnings on the way, keeping the last.
        while (m_position < position)
        {
            if (Step(position - m_position) == 0)
                break;
        }
    }
}
