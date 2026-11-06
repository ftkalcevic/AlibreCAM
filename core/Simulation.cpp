// Simulation.cpp : cutting the stock with the toolpaths, as a grid of voxels.

#include "Simulation.h"

#include <algorithm>
#include <cmath>

namespace camcore
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;

        int CellsAlong(double length, double cell)
        {
            return (std::max)(1, static_cast<int>(std::ceil(length / cell - 1e-9)));
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
    }

    double ChooseCellSize(const StockBox& box, double maxCells)
    {
        const Vec3 size = box.upper - box.lower;
        static const double kSteps[] = { 0.1, 0.2, 0.25, 0.5, 1.0, 2.0, 2.5, 5.0, 10.0, 20.0, 25.0, 50.0, 100.0 };
        for (double cell : kSteps)
        {
            const double cells = static_cast<double>(CellsAlong(size.x, cell)) * CellsAlong(size.y, cell) *
                                 CellsAlong(size.z, cell);
            if (cells <= maxCells)
                return cell;
        }
        return 100.0;
    }

    /////////////////////////////////////////////////////////////////////////
    // VoxelStock

    void VoxelStock::Reset(const StockBox& box, double cell)
    {
        const Vec3 size = box.upper - box.lower;
        m_lower = box.lower;
        m_cell = cell;
        m_nx = CellsAlong(size.x, cell);
        m_ny = CellsAlong(size.y, cell);
        m_nz = CellsAlong(size.z, cell);
        const size_t count = static_cast<size_t>(m_nx) * m_ny * m_nz;
        m_bits.assign((count + 63) / 64, 0);
        m_dirty.assign(static_cast<size_t>(ChunkCount()), 1);
    }

    void VoxelStock::FillAll()
    {
        const size_t count = static_cast<size_t>(m_nx) * m_ny * m_nz;
        std::fill(m_bits.begin(), m_bits.end(), ~std::uint64_t(0));
        if (count % 64 != 0 && !m_bits.empty())
            m_bits.back() = (std::uint64_t(1) << (count % 64)) - 1;
        MarkAllDirty();
    }

    void VoxelStock::FillMesh(const std::vector<Vec3>& triangles)
    {
        // Where each column's centre line crosses the mesh. The centre is nudged a
        // little so it doesn't run exactly along an edge.
        std::vector<std::vector<float>> hits(static_cast<size_t>(m_nx) * m_ny);
        const double nudgeX = m_cell * 1.37e-6;
        const double nudgeY = m_cell * 0.71e-6;
        for (size_t t = 0; t + 2 < triangles.size(); t += 3)
        {
            const Vec3& a = triangles[t];
            const Vec3& b = triangles[t + 1];
            const Vec3& c = triangles[t + 2];
            const double area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
            if (std::fabs(area) < 1e-18)
                continue;

            const double minX = (std::min)({ a.x, b.x, c.x }), maxX = (std::max)({ a.x, b.x, c.x });
            const double minY = (std::min)({ a.y, b.y, c.y }), maxY = (std::max)({ a.y, b.y, c.y });
            const int i0 = (std::max)(0, static_cast<int>(std::floor((minX - m_lower.x) / m_cell - 0.5)));
            const int i1 = (std::min)(m_nx - 1, static_cast<int>(std::ceil((maxX - m_lower.x) / m_cell - 0.5)));
            const int j0 = (std::max)(0, static_cast<int>(std::floor((minY - m_lower.y) / m_cell - 0.5)));
            const int j1 = (std::min)(m_ny - 1, static_cast<int>(std::ceil((maxY - m_lower.y) / m_cell - 0.5)));
            for (int j = j0; j <= j1; ++j)
            {
                const double y = m_lower.y + (j + 0.5) * m_cell + nudgeY;
                for (int i = i0; i <= i1; ++i)
                {
                    const double x = m_lower.x + (i + 0.5) * m_cell + nudgeX;
                    // Barycentric weights; all of one sign means inside.
                    const double wa = ((b.x - x) * (c.y - y) - (c.x - x) * (b.y - y)) / area;
                    const double wb = ((c.x - x) * (a.y - y) - (a.x - x) * (c.y - y)) / area;
                    const double wc = 1.0 - wa - wb;
                    if (wa < 0.0 || wb < 0.0 || wc < 0.0)
                        continue;
                    hits[static_cast<size_t>(j) * m_nx + i].push_back(
                        static_cast<float>(wa * a.z + wb * b.z + wc * c.z));
                }
            }
        }

        // Between each pair of crossings is inside.
        std::fill(m_bits.begin(), m_bits.end(), 0);
        for (int j = 0; j < m_ny; ++j)
        {
            for (int i = 0; i < m_nx; ++i)
            {
                std::vector<float>& column = hits[static_cast<size_t>(j) * m_nx + i];
                std::sort(column.begin(), column.end());
                for (size_t h = 0; h + 1 < column.size(); h += 2)
                {
                    for (int k = 0; k < m_nz; ++k)
                    {
                        const double z = m_lower.z + (k + 0.5) * m_cell;
                        if (z >= column[h] && z <= column[h + 1])
                        {
                            const size_t index = Index(i, j, k);
                            m_bits[index / 64] |= std::uint64_t(1) << (index % 64);
                        }
                    }
                }
            }
        }
        MarkAllDirty();
    }

    bool VoxelStock::Filled(int i, int j, int k) const
    {
        if (i < 0 || j < 0 || k < 0 || i >= m_nx || j >= m_ny || k >= m_nz)
            return false;
        const size_t index = Index(i, j, k);
        return ((m_bits[index / 64] >> (index % 64)) & 1) != 0;
    }

    void VoxelStock::Clear(int i, int j, int k)
    {
        const size_t index = Index(i, j, k);
        m_bits[index / 64] &= ~(std::uint64_t(1) << (index % 64));
        MarkDirty(i, j, k);
    }

    Vec3 VoxelStock::Centre(int i, int j, int k) const
    {
        return { m_lower.x + (i + 0.5) * m_cell, m_lower.y + (j + 0.5) * m_cell, m_lower.z + (k + 0.5) * m_cell };
    }

    long long VoxelStock::Count() const
    {
        long long count = 0;
        for (std::uint64_t word : m_bits)
        {
            for (; word != 0; word &= word - 1)
                ++count;
        }
        return count;
    }

    void VoxelStock::MarkDirty(int i, int j, int k)
    {
        // A cut changes its own chunk, and a neighbour's surface where it's on a chunk's edge.
        for (int dk = -1; dk <= 1; ++dk)
        {
            for (int dj = -1; dj <= 1; ++dj)
            {
                for (int di = -1; di <= 1; ++di)
                {
                    const int ci = (i + di) / kChunk, cj = (j + dj) / kChunk, ck = (k + dk) / kChunk;
                    if (i + di < 0 || j + dj < 0 || k + dk < 0 || ci >= ChunksX() || cj >= ChunksY() ||
                        ck >= ChunksZ())
                        continue;
                    m_dirty[static_cast<size_t>((ck * ChunksY() + cj) * ChunksX() + ci)] = 1;
                }
            }
        }
    }

    void VoxelStock::MarkAllDirty()
    {
        std::fill(m_dirty.begin(), m_dirty.end(), 1);
    }

    void VoxelStock::SetBits(const std::vector<std::uint64_t>& bits)
    {
        m_bits = bits;
        MarkAllDirty();
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
                      const Vec3& axis)
    {
        Vec3 w;
        if (!Normalise(axis, w) || stock.SizeX() == 0)
            return 0;

        // Short steps: none moving more than half a cell along the axis, or more
        // than a couple of tool widths across it, so each step's box is tight.
        const double cell = stock.Cell();
        const Vec3 d = to - from;
        const double axial = Dot(d, w);
        const double across = Length(d - w * axial);
        const double reach = (std::max)(profile.radius, cell);
        const int steps = (std::max)({ 1, static_cast<int>(std::ceil(std::fabs(axial) / (0.5 * cell))),
                                       static_cast<int>(std::ceil(across / (2.0 * reach))) });

        long long removed = 0;
        const Vec3 lower = stock.Lower();
        for (int s = 0; s < steps; ++s)
        {
            const Vec3 a = from + d * (static_cast<double>(s) / steps);
            const Vec3 b = from + d * (static_cast<double>(s + 1) / steps);

            // The step as a sweep square to the axis, with the tip at the lower end's height.
            const double tip = (std::min)(Dot(a, w), Dot(b, w));
            const Vec3 pa = a - w * Dot(a, w);
            const Vec3 pb = b - w * Dot(b, w);

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

            for (int k = k0; k <= k1; ++k)
            {
                for (int j = j0; j <= j1; ++j)
                {
                    for (int i = i0; i <= i1; ++i)
                    {
                        if (!stock.Filled(i, j, k))
                            continue;
                        const Vec3 c = stock.Centre(i, j, k);
                        const double along = Dot(c, w);
                        const double radius = profile.RadiusAt(along - tip);
                        if (radius < 0.0)
                            continue;
                        if (DistanceToSegment(c - w * along, pa, pb) <= radius + 1e-9)
                        {
                            stock.Clear(i, j, k);
                            ++removed;
                        }
                    }
                }
            }
        }
        return removed;
    }

    /////////////////////////////////////////////////////////////////////////
    // Meshing

    StockMesh MeshChunk(const VoxelStock& stock, int chunk)
    {
        StockMesh mesh;
        const int n = VoxelStock::kChunk;
        const int cx = chunk % stock.ChunksX();
        const int cy = (chunk / stock.ChunksX()) % stock.ChunksY();
        const int cz = chunk / (stock.ChunksX() * stock.ChunksY());
        const int start[3] = { cx * n, cy * n, cz * n };
        const int size[3] = { (std::min)(n, stock.SizeX() - start[0]), (std::min)(n, stock.SizeY() - start[1]),
                              (std::min)(n, stock.SizeZ() - start[2]) };
        const double cell = stock.Cell();
        const double lower[3] = { stock.Lower().x, stock.Lower().y, stock.Lower().z };

        const int sizes[3] = { stock.SizeX(), stock.SizeY(), stock.SizeZ() };
        const size_t strides[3] = { 1, static_cast<size_t>(sizes[0]),
                                    static_cast<size_t>(sizes[0]) * static_cast<size_t>(sizes[1]) };

        // Each face direction: across axis a, facing sign s. The face's own axes u and v
        // are chosen so u x v points along +a.
        for (int a = 0; a < 3; ++a)
        {
            const int u = (a + 1) % 3;
            const int v = (a + 2) % 3;
            for (int s = -1; s <= 1; s += 2)
            {
                std::vector<char> mask(static_cast<size_t>(size[u]) * size[v]);
                for (int layer = 0; layer < size[a]; ++layer)
                {
                    // Faces of filled cells whose neighbour that way is empty (or off the grid).
                    const int along = start[a] + layer;
                    const bool edge = along + s < 0 || along + s >= sizes[a];
                    for (int y = 0; y < size[v]; ++y)
                    {
                        size_t index = along * strides[a] + (start[u]) * strides[u] + (start[v] + y) * strides[v];
                        for (int x = 0; x < size[u]; ++x, index += strides[u])
                        {
                            bool face = stock.FilledAt(index);
                            if (face && !edge)
                                face = !stock.FilledAt(s > 0 ? index + strides[a] : index - strides[a]);
                            mask[static_cast<size_t>(y) * size[u] + x] = face;
                        }
                    }

                    // Merged into rectangles, greedily.
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
                            for (bool grow = true; grow && y + height < size[v]; )
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
        }
        return mesh;
    }

    /////////////////////////////////////////////////////////////////////////
    // Stepping

    void Simulation::Start(const StockBox& box, const std::vector<Vec3>& stockTriangles,
                           const std::vector<SimOperation>& operations, double maxCells)
    {
        m_stock.Reset(box, ChooseCellSize(box, maxCells));
        if (stockTriangles.empty())
            m_stock.FillAll();
        else
            m_stock.FillMesh(stockTriangles);
        m_start = m_stock.Bits();
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
        m_stock.SetBits(m_start);
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
