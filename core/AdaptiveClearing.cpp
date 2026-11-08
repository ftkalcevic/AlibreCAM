// AdaptiveClearing.cpp : adaptive (constant engagement) clearing of an area at one level. No Alibre or MFC code belongs here.

#include "AdaptiveClearing.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <queue>
#include <unordered_map>

#include "PlungeRoughToolpath.h"    // SquaredDistanceTransform
#include "WaterlineRoughToolpath.h" // IsoContours

namespace camcore
{
    std::vector<std::string> ValidateRampEntry(const RampEntry& ramp)
    {
        std::vector<std::string> problems;
        if (!(ramp.helixPercent >= 0.0 && ramp.helixPercent <= 200.0))
            problems.push_back("The helix diameter must be 0 to 200% of the tool's diameter.");
        if (!(ramp.rampAngle > 0.0 && ramp.rampAngle <= 45.0))
            problems.push_back("The ramp angle must be more than 0 and at most 45 degrees.");
        return problems;
    }

    namespace
    {
        const double kPi = 3.14159265358979323846;

        double g_contourMs = 0.0, g_widestMs = 0.0;     // timing, for finding where the time goes

        // ALIBRECAM_ADAPTIVE_DEBUG=1 prints each pass, for finding problems.
        bool Debugging()
        {
            static const bool on = [] {
                char* value = nullptr;
                size_t length = 0;
                const bool set = _dupenv_s(&value, &length, "ALIBRECAM_ADAPTIVE_DEBUG") == 0 && value != nullptr &&
                                 value[0] == '1';
                std::free(value);
                return set;
            }();
            return on;
        }

        double SegmentDistanceSquared(double px, double py, double ax, double ay, double bx, double by)
        {
            const double dx = bx - ax;
            const double dy = by - ay;
            const double length2 = dx * dx + dy * dy;
            double t = 0.0;
            if (length2 > 0.0)
                t = (std::max)(0.0, (std::min)(1.0, ((px - ax) * dx + (py - ay) * dy) / length2));
            const double ex = px - (ax + t * dx);
            const double ey = py - (ay + t * dy);
            return ex * ex + ey * ey;
        }

        // Douglas-Peucker in XY, keeping the ends.
        void Simplify(const Polyline& points, size_t first, size_t last, double tolerance, std::vector<char>& keep)
        {
            if (last <= first + 1)
                return;
            double worst = -1.0;
            size_t at = first;
            for (size_t k = first + 1; k < last; ++k)
            {
                const double d = SegmentDistanceSquared(points[k].x, points[k].y, points[first].x, points[first].y,
                                                        points[last].x, points[last].y);
                if (d > worst)
                {
                    worst = d;
                    at = k;
                }
            }
            if (worst > tolerance * tolerance)
            {
                keep[at] = 1;
                Simplify(points, first, at, tolerance, keep);
                Simplify(points, at, last, tolerance, keep);
            }
        }

        Polyline SimplifyPoints(const Polyline& points, double tolerance)
        {
            if (points.size() < 3)
                return points;
            std::vector<char> keep(points.size(), 0);
            keep.front() = 1;
            keep.back() = 1;
            Simplify(points, 0, points.size() - 1, tolerance, keep);
            Polyline result;
            for (size_t k = 0; k < points.size(); ++k)
            {
                if (keep[k])
                    result.push_back(points[k]);
            }
            return result;
        }

        // Clears the material by fronts: the distance from where the tool can
        // already be (air, or a helix's bore), measured round walls and islands,
        // cut at every optimal load. Each front is one load further into the
        // material than the last, so the tool's side cut stays about that wide.
        class Clearer
        {
        public:
            Clearer(AdaptiveGrid& grid, const AdaptiveSettings& settings) : m_g(grid), m_s(settings)
            {
                m_r = settings.toolRadius;
                m_load = settings.optimalLoad;
                m_cells = static_cast<size_t>(grid.nx) * static_cast<size_t>(grid.ny);

                // The grid's edge is never free, so the distances below see it as a wall.
                for (int i = 0; i < grid.nx; ++i)
                {
                    grid.free[grid.Index(i, 0)] = 0;
                    grid.free[grid.Index(i, grid.ny - 1)] = 0;
                }
                for (int j = 0; j < grid.ny; ++j)
                {
                    grid.free[grid.Index(0, j)] = 0;
                    grid.free[grid.Index(grid.nx - 1, j)] = 0;
                }

                // Room round each free cell, to the nearest cell that isn't free.
                std::vector<double> d(m_cells);
                for (size_t q = 0; q < m_cells; ++q)
                    d[q] = grid.free[q] ? 1e12 : 0.0;
                SquaredDistanceTransform(d, grid.nx, grid.ny);
                m_room.resize(m_cells);
                for (size_t q = 0; q < m_cells; ++q)
                    m_room[q] = static_cast<float>(std::sqrt(d[q]) * grid.cell);

                // Only material the tool can reach counts: within its radius of a free cell.
                for (size_t q = 0; q < m_cells; ++q)
                    d[q] = grid.free[q] ? 0.0 : 1e12;
                SquaredDistanceTransform(d, grid.nx, grid.ny);
                for (size_t q = 0; q < m_cells; ++q)
                {
                    if (grid.material[q] && std::sqrt(d[q]) * grid.cell > m_r + 1e-9)
                        grid.material[q] = 0;
                }
                m_toUncut.resize(m_cells);
                m_done.assign(m_cells, 0);
            }

            AdaptiveResult Run(const std::function<void(double)>& progress)
            {
                AdaptiveResult result;
                const size_t initial = Remaining();
                if (initial == 0)
                    return result;
                m_progress = progress;
                m_initial = initial;

                bool haveAt = false;
                Vec3 at;
                for (int phase = 0; phase < 10000; ++phase)
                {
                    UpdateToUncut();
                    if (Remaining() == 0)
                        break;

                    // Where the tool can already stand: free cells, not yet worked from,
                    // with nothing to cut under them.
                    std::vector<size_t> sources;
                    bool touches = false;
                    for (size_t q = 0; q < m_cells; ++q)
                    {
                        if (m_g.free[q] && !m_done[q] && m_toUncut[q] > m_r + 0.5 * m_g.cell)
                        {
                            sources.push_back(q);
                            touches = touches || m_toUncut[q] <= m_r + m_load + 2.0 * m_g.cell;
                        }
                    }
                    if (!touches)
                    {
                        // None next to the material: into it with a helix at the roomiest
                        // place that reaches it, or a plunge with no room.
                        size_t best = m_cells;
                        float room = -1.0f;
                        for (size_t q = 0; q < m_cells; ++q)
                        {
                            if (m_g.free[q] && !m_done[q] && m_toUncut[q] <= m_r + 1e-9 && m_room[q] > room)
                            {
                                room = m_room[q];
                                best = q;
                            }
                        }
                        if (best == m_cells)
                            break;      // nothing left but slivers along the walls
                        const Vec3 centre = CentreOf(best);
                        const double wanted = m_s.ramp.helixPercent / 100.0 * m_r;
                        const double radius = (std::min)(wanted, static_cast<double>(room) - 0.75 * m_g.cell);
                        AdaptiveCut cut;
                        if (radius >= (std::max)(2.0 * m_g.cell, 0.1 * m_r))
                        {
                            cut.entry = AdaptiveCut::Entry::Helix;
                            cut.helixCentre = centre;
                            cut.helixRadius = radius;
                            cut.points.push_back({ centre.x + radius, centre.y, 0.0 });
                            ClearDisc(centre, radius + m_r);
                            ++result.helixes;
                        }
                        else
                        {
                            cut.entry = AdaptiveCut::Entry::Plunge;
                            cut.points.push_back(centre);
                            ClearDisc(centre, m_r);
                            ++result.plunges;
                        }
                        at = cut.points.back();
                        haveAt = true;
                        result.cuts.push_back(cut);
                        // The fronts spread from the bore.
                        sources.clear();
                        const double reach = (std::max)(radius, 0.0);
                        for (size_t q = 0; q < m_cells; ++q)
                        {
                            if (!m_g.free[q] || m_done[q])
                                continue;
                            const Vec3 c = CentreOf(q);
                            if (std::hypot(c.x - centre.x, c.y - centre.y) <= reach + 0.5 * m_g.cell)
                                sources.push_back(q);
                        }
                        if (sources.empty())
                            sources.push_back(best);
                        UpdateToUncut();
                    }
                    if (Debugging())
                        std::printf("[adaptive] phase %d: %zu sources, %zu cells left of %zu" "\n", phase, sources.size(),
                                    Remaining(), initial);
                    CutFronts(sources, haveAt, at, result);
                }
                if (m_s.wallLaps)
                    WallLaps(haveAt, at, result);
                if (Debugging())
                    std::printf("[adaptive] contours %.0f ms, widest cut %.0f ms\n", g_contourMs, g_widestMs);
                result.leftArea = static_cast<double>(Remaining()) * m_g.cell * m_g.cell;
                if (progress)
                    progress(1.0);
                return result;
            }

        private:
            size_t Remaining() const
            {
                size_t count = 0;
                for (size_t q = 0; q < m_cells; ++q)
                    count += m_g.material[q] ? 1 : 0;
                return count;
            }

            Vec3 CentreOf(size_t q) const
            {
                const int i = static_cast<int>(q % static_cast<size_t>(m_g.nx));
                const int j = static_cast<int>(q / static_cast<size_t>(m_g.nx));
                return m_g.Centre(i, j);
            }

            bool CellAt(const Vec3& p, size_t& q) const
            {
                const int i = static_cast<int>(std::lround((p.x - m_g.xLow) / m_g.cell));
                const int j = static_cast<int>(std::lround((p.y - m_g.yLow) / m_g.cell));
                if (i < 0 || j < 0 || i >= m_g.nx || j >= m_g.ny)
                    return false;
                q = m_g.Index(i, j);
                return true;
            }

            void UpdateToUncut()
            {
                std::vector<double> d(m_cells);
                for (size_t q = 0; q < m_cells; ++q)
                    d[q] = m_g.material[q] ? 0.0 : 1e12;
                SquaredDistanceTransform(d, m_g.nx, m_g.ny);
                for (size_t q = 0; q < m_cells; ++q)
                    m_toUncut[q] = static_cast<float>(std::sqrt(d[q]) * m_g.cell);
            }

            // Counts (and with clear, removes) the material within the tool's
            // radius of the segment from a to b.
            size_t Sweep(const Vec3& a, const Vec3& b, bool clear)
            {
                const double r2 = m_r * m_r;
                const int i0 = (std::max)(0, static_cast<int>(std::floor(((std::min)(a.x, b.x) - m_r - m_g.xLow) / m_g.cell)));
                const int i1 = (std::min)(m_g.nx - 1, static_cast<int>(std::ceil(((std::max)(a.x, b.x) + m_r - m_g.xLow) / m_g.cell)));
                const int j0 = (std::max)(0, static_cast<int>(std::floor(((std::min)(a.y, b.y) - m_r - m_g.yLow) / m_g.cell)));
                const int j1 = (std::min)(m_g.ny - 1, static_cast<int>(std::ceil(((std::max)(a.y, b.y) + m_r - m_g.yLow) / m_g.cell)));
                size_t count = 0;
                for (int j = j0; j <= j1; ++j)
                {
                    const double y = m_g.yLow + j * m_g.cell;
                    unsigned char* row = &m_g.material[m_g.Index(0, j)];
                    for (int i = i0; i <= i1; ++i)
                    {
                        if (!row[i])
                            continue;
                        const double x = m_g.xLow + i * m_g.cell;
                        if (SegmentDistanceSquared(x, y, a.x, a.y, b.x, b.y) <= r2)
                        {
                            ++count;
                            if (clear)
                                row[i] = 0;
                        }
                    }
                }
                return count;
            }

            void ClearDisc(const Vec3& centre, double radius)
            {
                const double r2 = radius * radius;
                const int i0 = (std::max)(0, static_cast<int>(std::floor((centre.x - radius - m_g.xLow) / m_g.cell)));
                const int i1 = (std::min)(m_g.nx - 1, static_cast<int>(std::ceil((centre.x + radius - m_g.xLow) / m_g.cell)));
                const int j0 = (std::max)(0, static_cast<int>(std::floor((centre.y - radius - m_g.yLow) / m_g.cell)));
                const int j1 = (std::min)(m_g.ny - 1, static_cast<int>(std::ceil((centre.y + radius - m_g.yLow) / m_g.cell)));
                for (int j = j0; j <= j1; ++j)
                {
                    for (int i = i0; i <= i1; ++i)
                    {
                        const double dx = m_g.xLow + i * m_g.cell - centre.x;
                        const double dy = m_g.yLow + j * m_g.cell - centre.y;
                        if (dx * dx + dy * dy <= r2)
                            m_g.material[m_g.Index(i, j)] = 0;
                    }
                }
            }

            // Is a straight move from a to b on free cells, and either a short step
            // into the material (from one front to the next), or not into it by more
            // than the load (the tool, less the load, touching none)?
            bool CanLink(const Vec3& a, const Vec3& b, bool stepAcross) const
            {
                const double length = std::hypot(b.x - a.x, b.y - a.y);
                const bool shortStep = length <= (stepAcross ? 2.5 * m_r + 2.0 * m_load : 2.0 * m_load) + m_g.cell;
                if (!shortStep && length > (std::max)(30.0, 10.0 * m_r))
                    return false;
                const int samples = (std::max)(1, static_cast<int>(std::ceil(length / (0.5 * m_g.cell))));
                for (int k = 0; k <= samples; ++k)
                {
                    const double t = static_cast<double>(k) / samples;
                    size_t q = 0;
                    if (!CellAt({ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, 0.0 }, q) || !m_g.free[q])
                        return false;
                }
                return shortStep || !Touches(a, b, m_r - m_load - m_g.cell);
            }

            // Is every point of a straight move on free cells?
            bool FreeAlong(const Vec3& a, const Vec3& b) const
            {
                const double length = std::hypot(b.x - a.x, b.y - a.y);
                const int samples = (std::max)(1, static_cast<int>(std::ceil(length / (0.5 * m_g.cell))));
                for (int k = 0; k <= samples; ++k)
                {
                    const double t = static_cast<double>(k) / samples;
                    size_t q = 0;
                    if (!CellAt({ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, 0.0 }, q) || !m_g.free[q])
                        return false;
                }
                return true;
            }

            // Is there material within the radius of the segment from a to b?
            bool Touches(const Vec3& a, const Vec3& b, double radius) const
            {
                if (radius <= 0.0)
                    return false;
                const double r2 = radius * radius;
                const int i0 = (std::max)(0, static_cast<int>(std::floor(((std::min)(a.x, b.x) - radius - m_g.xLow) / m_g.cell)));
                const int i1 = (std::min)(m_g.nx - 1, static_cast<int>(std::ceil(((std::max)(a.x, b.x) + radius - m_g.xLow) / m_g.cell)));
                const int j0 = (std::max)(0, static_cast<int>(std::floor(((std::min)(a.y, b.y) - radius - m_g.yLow) / m_g.cell)));
                const int j1 = (std::min)(m_g.ny - 1, static_cast<int>(std::ceil(((std::max)(a.y, b.y) + radius - m_g.yLow) / m_g.cell)));
                for (int j = j0; j <= j1; ++j)
                {
                    const double y = m_g.yLow + j * m_g.cell;
                    const unsigned char* row = &m_g.material[m_g.Index(0, j)];
                    for (int i = i0; i <= i1; ++i)
                    {
                        if (row[i] && SegmentDistanceSquared(m_g.xLow + i * m_g.cell, y, a.x, a.y, b.x, b.y) <= r2)
                            return true;
                    }
                }
                return false;
            }

            // The distance from the sources through free cells not yet worked from, by
            // fast marching: the eikonal equation solved upwind, second order along an
            // axis where two cells that way are known (falling back to first order),
            // so fronts come out round rather than faceted. Walls and cells worked
            // from before are never crossed.
            std::vector<float> Distances(const std::vector<size_t>& sources) const
            {
                const double infinite = std::numeric_limits<double>::infinity();
                std::vector<double> distance(m_cells, infinite);
                std::vector<unsigned char> known(m_cells, 0);
                using Entry = std::pair<double, size_t>;
                std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> queue;
                const double h = m_g.cell;
                const auto open = [&](int i, int j) {
                    if (i < 0 || j < 0 || i >= m_g.nx || j >= m_g.ny)
                        return false;
                    const size_t q = m_g.Index(i, j);
                    return m_g.free[q] && !m_done[q];
                };
                const auto knownAt = [&](int i, int j, double& value) {
                    if (i < 0 || j < 0 || i >= m_g.nx || j >= m_g.ny)
                        return false;
                    const size_t q = m_g.Index(i, j);
                    if (!known[q])
                        return false;
                    value = distance[q];
                    return true;
                };
                // The value at (i, j) from its known neighbours.
                const auto solve = [&](int i, int j) {
                    double weights[2] = { 0.0, 0.0 }, targets[2] = { 0.0, 0.0 };
                    int axes = 0;
                    for (int axis = 0; axis < 2; ++axis)
                    {
                        const int di = axis == 0 ? 1 : 0, dj = axis == 0 ? 0 : 1;
                        double best = infinite, weight = 1.0, target = 0.0;
                        for (int side : { -1, 1 })
                        {
                            double t1 = 0.0;
                            if (!knownAt(i + side * di, j + side * dj, t1))
                                continue;
                            double t2 = 0.0;
                            double w = 1.0, t = t1;
                            if (knownAt(i + 2 * side * di, j + 2 * side * dj, t2) && t2 <= t1)
                            {
                                w = 2.25;
                                t = (4.0 * t1 - t2) / 3.0;
                            }
                            // The side giving the smaller first-order value leads.
                            if (t1 < best)
                            {
                                best = t1;
                                weight = w;
                                target = t;
                            }
                        }
                        if (best < infinite)
                        {
                            weights[axes] = weight;
                            targets[axes] = target;
                            ++axes;
                        }
                    }
                    if (axes == 0)
                        return infinite;
                    // Both axes: (sum w)T^2 - 2(sum w t)T + sum w t^2 - h^2 = 0, the larger root.
                    if (axes == 2)
                    {
                        const double a = weights[0] + weights[1];
                        const double b = weights[0] * targets[0] + weights[1] * targets[1];
                        const double c = weights[0] * targets[0] * targets[0] + weights[1] * targets[1] * targets[1] - h * h;
                        const double disc = b * b - a * c;
                        if (disc >= 0.0)
                        {
                            const double t = (b + std::sqrt(disc)) / a;
                            if (t >= (std::max)(targets[0], targets[1]))
                                return t;
                        }
                        // Only the nearer axis.
                        const int k = (targets[0] + h / std::sqrt(weights[0]) <= targets[1] + h / std::sqrt(weights[1])) ? 0 : 1;
                        return targets[k] + h / std::sqrt(weights[k]);
                    }
                    return targets[0] + h / std::sqrt(weights[0]);
                };

                for (size_t q : sources)
                {
                    distance[q] = 0.0;
                    queue.push({ 0.0, q });
                }
                static const int around[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
                while (!queue.empty())
                {
                    const Entry top = queue.top();
                    queue.pop();
                    if (known[top.second] || top.first > distance[top.second])
                        continue;
                    known[top.second] = 1;
                    const int i = static_cast<int>(top.second % static_cast<size_t>(m_g.nx));
                    const int j = static_cast<int>(top.second / static_cast<size_t>(m_g.nx));
                    for (const auto& step : around)
                    {
                        const int a = i + step[0], b = j + step[1];
                        if (!open(a, b))
                            continue;
                        const size_t q = m_g.Index(a, b);
                        if (known[q])
                            continue;
                        const double value = solve(a, b);
                        if (value < distance[q])
                        {
                            distance[q] = value;
                            queue.push({ value, q });
                        }
                    }
                }
                std::vector<float> result(m_cells);
                for (size_t q = 0; q < m_cells; ++q)
                    result[q] = static_cast<float>(distance[q]);
                return result;
            }

            // Cuts the fronts spreading from the sources, every optimal load.
            void CutFronts(const std::vector<size_t>& sources, bool& haveAt, Vec3& at, AdaptiveResult& result)
            {
                const std::vector<float> distance = Distances(sources);
                float furthest = 0.0f;
                for (size_t q = 0; q < m_cells; ++q)
                {
                    if (std::isfinite(distance[q]))
                        furthest = (std::max)(furthest, distance[q]);
                }

                // Worked out once a front: cells reached are the front's distance, those
                // worked from before are behind every front, and the rest are walls.
                const float wall = 1e9f;
                std::vector<float> field(m_cells);
                for (size_t q = 0; q < m_cells; ++q)
                    field[q] = std::isfinite(distance[q]) ? distance[q] : (m_done[q] ? -1.0f : wall);
                m_sorted.clear();
                for (size_t q = 0; q < m_cells; ++q)
                {
                    if (std::isfinite(distance[q]))
                        m_sorted.push_back({ distance[q], q });
                }
                std::sort(m_sorted.begin(), m_sorted.end());
                if (m_stamp.size() != m_cells)
                    m_stamp.assign(m_cells, 0);

                // Each front a load further on, or nearer where it would cut wider than
                // that (round a small bore, into corners): the widest 2 mm of it within
                // 1.2 loads.
                double previous = 0.0;
                for (int guard = 0; guard < 100000 && previous <= furthest + m_load; ++guard)
                {
                    double step = m_load;
                    double level = previous + step;
                    std::vector<Polyline> pieces;
                    std::vector<char> closed;
                    for (int attempt = 0; attempt < 8; ++attempt)
                    {
                        level = previous + step;
                        const auto t0 = std::chrono::steady_clock::now();
                        FrontPieces(field, wall, level, pieces, closed);
                        const auto t1 = std::chrono::steady_clock::now();
                        double typical = 0.0;
                        const double widest = WidestCut(pieces, closed, typical);
                        const auto t2 = std::chrono::steady_clock::now();
                        g_contourMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
                        g_widestMs += std::chrono::duration<double, std::milli>(t2 - t1).count();
                        const double over = (std::max)(typical / (1.15 * m_load), widest / (2.0 * m_load));
                        if (over <= 1.0 || step <= 0.15 * m_load)
                            break;
                        step *= (std::max)(0.3, (std::min)(0.9, 1.0 / over));
                    }
                    previous = level;

                    // Nearest first.
                    std::vector<char> used(pieces.size(), 0);
                    for (size_t n = 0; n < pieces.size(); ++n)
                    {
                        size_t best = pieces.size();
                        double bestDistance = 0.0;
                        size_t bestStart = 0;
                        for (size_t k = 0; k < pieces.size(); ++k)
                        {
                            if (used[k])
                                continue;
                            const Polyline& piece = pieces[k];
                            const size_t candidates = closed[k] ? piece.size() : 1;
                            for (size_t s = 0; s < candidates; ++s)
                            {
                                const double d = haveAt ? std::hypot(piece[s].x - at.x, piece[s].y - at.y) : 0.0;
                                if (best == pieces.size() || d < bestDistance)
                                {
                                    best = k;
                                    bestDistance = d;
                                    bestStart = s;
                                }
                            }
                        }
                        used[best] = 1;
                        Polyline piece = pieces[best];
                        if (closed[best])
                        {
                            // Start a little way round from the nearest point, so the step
                            // out from the last loop goes slantwise across the new band
                            // rather than straight into it.
                            const double ahead = 2.0 * m_r;
                            double along = 0.0;
                            size_t start = bestStart;
                            for (size_t k = 0; k < piece.size() && along < ahead; ++k)
                            {
                                const size_t a = (bestStart + k) % piece.size();
                                const size_t b = (a + 1) % piece.size();
                                along += std::hypot(piece[b].x - piece[a].x, piece[b].y - piece[a].y);
                                start = b;
                            }
                            std::rotate(piece.begin(), piece.begin() + static_cast<ptrdiff_t>(start), piece.end());
                            piece.push_back(piece.front());
                        }
                        CutPiece(piece, haveAt, at, result, closed[best] != 0, &distance);
                    }
                    if (m_progress)
                        m_progress(1.0 - static_cast<double>(Remaining()) / static_cast<double>(m_initial));
                }
                for (size_t q = 0; q < m_cells; ++q)
                {
                    if (std::isfinite(distance[q]))
                        m_done[q] = 1;
                }
            }

            // The front at a level, cut climb or conventional, less where it runs along
            // walls, simplified. Only the squares of cells near the level are looked at.
            void FrontPieces(const std::vector<float>& field, float wall, double level, std::vector<Polyline>& pieces,
                             std::vector<char>& closed)
            {
                pieces.clear();
                closed.clear();
                const float low = static_cast<float>(level - 1.5 * m_g.cell);
                const float high = static_cast<float>(level + 1.5 * m_g.cell);
                auto first = std::lower_bound(m_sorted.begin(), m_sorted.end(), std::pair<float, size_t>(low, 0));
                ++m_stampNow;
                if (m_stampNow == 0)
                {
                    std::fill(m_stamp.begin(), m_stamp.end(), 0u);
                    m_stampNow = 1;
                }

                // Each square (lower left corner i, j) gives segments with the higher
                // values on their left, from the edge it starts on to the edge it ends on.
                const int nx = m_g.nx, ny = m_g.ny;
                const auto value = [&](int i, int j) { return static_cast<double>(field[m_g.Index(i, j)]); };
                const auto horizontal = [&](int i, int j) { return 2 * (static_cast<std::int64_t>(j) * nx + i); };
                const auto upright = [&](int i, int j) { return 2 * (static_cast<std::int64_t>(j) * nx + i) + 1; };
                std::unordered_map<std::int64_t, std::int64_t> next;
                for (auto it = first; it != m_sorted.end() && it->first <= high; ++it)
                {
                    const int ci = static_cast<int>(it->second % static_cast<size_t>(nx));
                    const int cj = static_cast<int>(it->second / static_cast<size_t>(nx));
                    for (int j = cj - 1; j <= cj; ++j)
                    {
                        for (int i = ci - 1; i <= ci; ++i)
                        {
                            if (i < 0 || j < 0 || i + 1 >= nx || j + 1 >= ny)
                                continue;
                            const size_t square = m_g.Index(i, j);
                            if (m_stamp[square] == m_stampNow)
                                continue;
                            m_stamp[square] = m_stampNow;
                            const double v[4] = { value(i, j), value(i + 1, j), value(i + 1, j + 1), value(i, j + 1) };
                            if (v[0] >= wall || v[1] >= wall || v[2] >= wall || v[3] >= wall)
                                continue;   // along a wall
                            const bool in[4] = { v[0] > level, v[1] > level, v[2] > level, v[3] > level };
                            if (in[0] == in[1] && in[1] == in[2] && in[2] == in[3])
                                continue;
                            const std::int64_t edges[4] = { horizontal(i, j), upright(i + 1, j), horizontal(i, j + 1),
                                                            upright(i, j) };
                            int outs[2], ins[2], order[4];
                            int nOut = 0, nIn = 0, crossings = 0;
                            for (int e = 0; e < 4; ++e)
                            {
                                const bool from = in[e], to = in[(e + 1) % 4];
                                if (from == to)
                                    continue;
                                order[crossings++] = e;
                                if (from)
                                    outs[nOut++] = e;
                                else
                                    ins[nIn++] = e;
                            }
                            if (crossings == 2)
                            {
                                next[edges[outs[0]]] = edges[ins[0]];
                                continue;
                            }
                            const bool middle = (v[0] + v[1] + v[2] + v[3]) / 4.0 > level;
                            for (int k = 0; k < 4; ++k)
                            {
                                const int e = order[k];
                                if (!in[e])
                                    continue;
                                const int partner = middle ? order[(k + 1) % 4] : order[(k + 3) % 4];
                                next[edges[e]] = edges[partner];
                            }
                        }
                    }
                }
                const auto crossing = [&](std::int64_t edge) {
                    const std::int64_t cellIndex = edge / 2;
                    const int i = static_cast<int>(cellIndex % nx);
                    const int j = static_cast<int>(cellIndex / nx);
                    const int i2 = (edge % 2 == 0) ? i + 1 : i;
                    const int j2 = (edge % 2 == 0) ? j : j + 1;
                    const double v1 = value(i, j), v2 = value(i2, j2);
                    const double t = (std::fabs(v2 - v1) > 1e-12) ? (level - v1) / (v2 - v1) : 0.5;
                    const Vec3 a = m_g.Centre(i, j), b = m_g.Centre(i2, j2);
                    return Vec3{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, 0.0 };
                };

                // Joined: open chains from the edges nothing leads to, then loops.
                std::unordered_map<std::int64_t, char> led;
                for (const auto& link : next)
                    led[link.second] = 1;
                const auto take = [&](std::int64_t start, bool loop) {
                    Polyline chain;
                    std::int64_t at = start;
                    for (;;)
                    {
                        chain.push_back(crossing(at));
                        const auto found = next.find(at);
                        if (found == next.end())
                            break;
                        const std::int64_t to = found->second;
                        next.erase(found);
                        at = to;
                        if (loop && at == start)
                            break;
                    }
                    if (chain.size() < 2)
                        return;
                    if (m_s.climb)
                        std::reverse(chain.begin(), chain.end());   // the material on the right
                    if (loop)
                    {
                        chain.push_back(chain.front());
                        chain = SimplifyPoints(chain, m_s.simplify);
                        chain.pop_back();
                        if (chain.size() < 3)
                            return;
                    }
                    else
                    {
                        chain = SimplifyPoints(chain, m_s.simplify);
                    }
                    pieces.push_back(std::move(chain));
                    closed.push_back(loop ? 1 : 0);
                };
                std::vector<std::int64_t> starts;
                for (const auto& link : next)
                {
                    if (led.find(link.first) == led.end())
                        starts.push_back(link.first);
                }
                for (std::int64_t start : starts)
                    take(start, false);
                while (!next.empty())
                    take(next.begin()->first, true);
                (void)wall;
            }

            // How wide the pieces would cut over their widest 2 mm (the area removed
            // over the length the tool's centre goes), leaving the material as it was.
            // Typical is the 85th percentile of the 2 mm stretches.
            double WidestCut(const std::vector<Polyline>& pieces, const std::vector<char>& closed, double& typical)
            {
                std::vector<size_t> cleared;
                std::vector<double> widths;
                double widest = 0.0;
                const double area = m_g.cell * m_g.cell;
                for (size_t k = 0; k < pieces.size(); ++k)
                {
                    const Polyline& piece = pieces[k];
                    const size_t segments = closed[k] ? piece.size() : piece.size() - 1;
                    double length = 0.0;
                    size_t cells = 0;
                    for (size_t a = 0; a < segments; ++a)
                    {
                        const Vec3& from = piece[a];
                        const Vec3& to = piece[(a + 1) % piece.size()];
                        cells += SweepRecording(from, to, cleared);
                        length += std::hypot(to.x - from.x, to.y - from.y);
                        if (length >= 2.0)
                        {
                            widths.push_back(cells * area / length);
                            length = 0.0;
                            cells = 0;
                        }
                    }
                    if (length >= 1.0)
                        widths.push_back(cells * area / length);
                }
                for (size_t q : cleared)
                    m_g.material[q] = 1;
                typical = 0.0;
                if (!widths.empty())
                {
                    widest = *std::max_element(widths.begin(), widths.end());
                    const size_t at = static_cast<size_t>(0.85 * static_cast<double>(widths.size() - 1));
                    std::nth_element(widths.begin(), widths.begin() + static_cast<ptrdiff_t>(at), widths.end());
                    typical = widths[at];
                }
                return widest;
            }

            // Sweep's clearing, noting the cells cleared.
            size_t SweepRecording(const Vec3& a, const Vec3& b, std::vector<size_t>& cleared)
            {
                const double r2 = m_r * m_r;
                const int i0 = (std::max)(0, static_cast<int>(std::floor(((std::min)(a.x, b.x) - m_r - m_g.xLow) / m_g.cell)));
                const int i1 = (std::min)(m_g.nx - 1, static_cast<int>(std::ceil(((std::max)(a.x, b.x) + m_r - m_g.xLow) / m_g.cell)));
                const int j0 = (std::max)(0, static_cast<int>(std::floor(((std::min)(a.y, b.y) - m_r - m_g.yLow) / m_g.cell)));
                const int j1 = (std::min)(m_g.ny - 1, static_cast<int>(std::ceil(((std::max)(a.y, b.y) + m_r - m_g.yLow) / m_g.cell)));
                size_t count = 0;
                for (int j = j0; j <= j1; ++j)
                {
                    const double y = m_g.yLow + j * m_g.cell;
                    const size_t rowStart = m_g.Index(0, j);
                    for (int i = i0; i <= i1; ++i)
                    {
                        const size_t q = rowStart + static_cast<size_t>(i);
                        if (!m_g.material[q])
                            continue;
                        const double x = m_g.xLow + i * m_g.cell;
                        if (SegmentDistanceSquared(x, y, a.x, a.y, b.x, b.y) <= r2)
                        {
                            m_g.material[q] = 0;
                            cleared.push_back(q);
                            ++count;
                        }
                    }
                }
                return count;
            }

            // Cuts a piece, leaving off its ends where they'd cut nothing.
            void CutPiece(const Polyline& piece, bool& haveAt, Vec3& at, AdaptiveResult& result, bool stepAcross = false,
                          const std::vector<float>* pDistance = nullptr)
            {
                if (piece.size() < 2)
                    return;
                std::vector<size_t> removed(piece.size() - 1);
                for (size_t k = 0; k + 1 < piece.size(); ++k)
                    removed[k] = Sweep(piece[k], piece[k + 1], false);
                size_t first = 0, last = removed.size();
                while (first < last && removed[first] == 0)
                    ++first;
                while (last > first && removed[last - 1] == 0)
                    --last;
                if (first >= last)
                    return;
                AdaptiveCut cut;
                for (size_t k = first; k <= last; ++k)
                    cut.points.push_back(piece[k]);
                for (size_t k = 0; k + 1 < cut.points.size(); ++k)
                    Sweep(cut.points[k], cut.points[k + 1], true);
                cut.entry = (haveAt && CanLink(at, cut.points.front(), stepAcross)) ? AdaptiveCut::Entry::Link
                                                                                     : AdaptiveCut::Entry::Plunge;
                if (cut.entry == AdaptiveCut::Entry::Plunge && pDistance != nullptr)
                {
                    // Down in the air the fronts spread from, nearest the start, then across into the cut.
                    const Vec3 start = cut.points.front();
                    size_t q0 = 0;
                    if (CellAt(start, q0))
                    {
                        const int reach = static_cast<int>(std::ceil((1.5 * m_load) / m_g.cell)) + 3;
                        const int ci = static_cast<int>(q0 % static_cast<size_t>(m_g.nx));
                        const int cj = static_cast<int>(q0 / static_cast<size_t>(m_g.nx));
                        double best = 1e18;
                        Vec3 air;
                        for (int j = (std::max)(1, cj - reach); j <= (std::min)(m_g.ny - 2, cj + reach); ++j)
                        {
                            for (int i = (std::max)(1, ci - reach); i <= (std::min)(m_g.nx - 2, ci + reach); ++i)
                            {
                                const size_t q = m_g.Index(i, j);
                                if (!m_g.free[q] || (*pDistance)[q] != 0.0f)
                                    continue;
                                const Vec3 c = m_g.Centre(i, j);
                                const double d = std::hypot(c.x - start.x, c.y - start.y);
                                if (d < best && FreeAlong(c, start))
                                {
                                    best = d;
                                    air = c;
                                }
                            }
                        }
                        if (best < 1e17)
                            cut.points.insert(cut.points.begin(), air);
                    }
                }
                cut.points = SimplifyPoints(cut.points, m_s.simplify);
                at = cut.points.back();
                haveAt = true;
                result.cuts.push_back(std::move(cut));
                ++result.passes;
            }

            // The last of the material, slivers along the walls and round islands:
            // cut by laps along the edge of the free cells, only where there's material.
            void WallLaps(bool& haveAt, Vec3& at, AdaptiveResult& result)
            {
                std::vector<Polyline> loops = IsoContours(m_room, m_g.nx, m_g.ny, m_g.xLow - 0.5 * m_g.cell,
                                                          m_g.yLow - 0.5 * m_g.cell, m_g.cell, m_g.cell, 0.0);
                const long extra = (std::max)(2L, static_cast<long>(std::ceil(1.0 / m_g.cell)));
                for (Polyline& loop : loops)
                {
                    if (loop.size() < 3)
                        continue;
                    if (m_s.climb)
                        std::reverse(loop.begin(), loop.end());     // the walls (lower room) on the right
                    const long n = static_cast<long>(loop.size());
                    std::vector<char> wanted(static_cast<size_t>(n), 0);
                    bool any = false, all = true;
                    for (long k = 0; k < n; ++k)
                    {
                        if (Sweep(loop[static_cast<size_t>(k)], loop[static_cast<size_t>((k + 1) % n)], false) > 0)
                        {
                            any = true;
                            for (long e = -extra; e <= extra; ++e)
                                wanted[static_cast<size_t>(((k + e) % n + n) % n)] = 1;
                        }
                    }
                    for (long k = 0; k < n; ++k)
                        all = all && wanted[static_cast<size_t>(k)];
                    if (!any)
                        continue;
                    long first = 0;
                    if (!all)
                    {
                        while (wanted[static_cast<size_t>(first)])
                            ++first;
                    }
                    long k = first;
                    for (long visited = 0; visited < n;)
                    {
                        if (!wanted[static_cast<size_t>(k % n)])
                        {
                            ++k;
                            ++visited;
                            continue;
                        }
                        Polyline run;
                        while (visited < n && wanted[static_cast<size_t>(k % n)])
                        {
                            run.push_back(loop[static_cast<size_t>(k % n)]);
                            ++k;
                            ++visited;
                        }
                        run.push_back(loop[static_cast<size_t>(k % n)]);
                        CutPiece(run, haveAt, at, result);
                    }
                }
            }

            AdaptiveGrid& m_g;
            const AdaptiveSettings& m_s;
            double m_r = 1.0;
            double m_load = 0.5;
            size_t m_cells = 0;
            size_t m_initial = 1;
            std::function<void(double)> m_progress;
            std::vector<float> m_room;
            std::vector<float> m_toUncut;
            std::vector<unsigned char> m_done;     // worked from by an earlier set of fronts
            std::vector<std::pair<float, size_t>> m_sorted;     // this set's cells by distance
            std::vector<unsigned> m_stamp;                      // squares looked at for the current front
            unsigned m_stampNow = 0;
        };
    }

    AdaptiveResult AdaptiveClear(AdaptiveGrid grid, const AdaptiveSettings& settings,
                                 const std::function<void(double)>& progress)
    {
        if (grid.nx < 3 || grid.ny < 3 || grid.free.size() != static_cast<size_t>(grid.nx) * grid.ny ||
            grid.material.size() != grid.free.size() || !(settings.toolRadius > 0.0) || !(settings.optimalLoad > 0.0))
            return AdaptiveResult();
        Clearer clearer(grid, settings);
        return clearer.Run(progress);
    }

    double AdaptiveCell(double toolDiameter, double load, double width, double height)
    {
        double cell = (std::min)(0.25, (std::max)(0.02, toolDiameter / 40.0));
        if (load > 0.0)
            cell = (std::min)(cell, (std::max)(0.01, load / 3.0));
        const double area = (std::max)(1.0, width) * (std::max)(1.0, height);
        if (area / (cell * cell) > 8e6)
            cell = std::sqrt(area / 8e6);
        return cell;
    }

    AdaptiveGrid AdaptiveGridFromOutlines(const std::vector<Path>& outlines, double cell, double inset, double toolRadius)
    {
        AdaptiveGrid grid;
        if (outlines.empty() || !(cell > 0.0))
            return grid;
        double xMin = 0.0, xMax = 0.0, yMin = 0.0, yMax = 0.0;
        for (size_t k = 0; k < outlines.size(); ++k)
        {
            double a = 0.0, b = 0.0, c = 0.0, d = 0.0;
            PathExtent(outlines[k], { 1.0, 0.0, 0.0 }, a, b);
            PathExtent(outlines[k], { 0.0, 1.0, 0.0 }, c, d);
            xMin = (k == 0) ? a : (std::min)(xMin, a);
            xMax = (k == 0) ? b : (std::max)(xMax, b);
            yMin = (k == 0) ? c : (std::min)(yMin, c);
            yMax = (k == 0) ? d : (std::max)(yMax, d);
        }
        const double margin = (std::max)(0.0, -inset) + toolRadius + 4.0 * cell;
        grid.cell = cell;
        grid.xLow = xMin - margin;
        grid.yLow = yMin - margin;
        grid.nx = static_cast<int>(std::ceil((xMax - xMin + 2.0 * margin) / cell)) + 1;
        grid.ny = static_cast<int>(std::ceil((yMax - yMin + 2.0 * margin) / cell)) + 1;
        const size_t cells = static_cast<size_t>(grid.nx) * static_cast<size_t>(grid.ny);
        grid.material.assign(cells, 0);
        grid.free.assign(cells, 0);

        // Inside, a row at a time.
        for (int j = 0; j < grid.ny; ++j)
        {
            const double y = grid.yLow + j * cell;
            std::vector<double> crossings;
            for (const Path& outline : outlines)
            {
                for (double x : PathCrossings(outline, { 1.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0 }, y))
                    crossings.push_back(x);
            }
            std::sort(crossings.begin(), crossings.end());
            for (size_t k = 0; k + 1 < crossings.size(); k += 2)
            {
                const int i0 = (std::max)(0, static_cast<int>(std::ceil((crossings[k] - grid.xLow) / cell)));
                const int i1 = (std::min)(grid.nx - 1, static_cast<int>(std::floor((crossings[k + 1] - grid.xLow) / cell)));
                for (int i = i0; i <= i1; ++i)
                    grid.material[grid.Index(i, j)] = 1;
            }
        }

        // Where the centre may go, from the distance to the outlines' edge.
        std::vector<double> d(cells);
        const bool inside = inset >= 0.0;
        for (size_t q = 0; q < cells; ++q)
            d[q] = ((grid.material[q] != 0) == inside) ? 1e12 : 0.0;
        SquaredDistanceTransform(d, grid.nx, grid.ny);
        for (size_t q = 0; q < cells; ++q)
        {
            const double distance = std::sqrt(d[q]) * cell;
            grid.free[q] = inside ? ((grid.material[q] && distance >= inset + 0.75 * cell) ? 1 : 0)
                                  : ((distance <= -inset - 0.75 * cell) ? 1 : 0);
        }
        return grid;
    }

    void WriteAdaptiveCuts(const std::vector<AdaptiveCut>& cuts, const AdaptiveWriting& writing, Toolpath& toolpath)
    {
        const auto add = [&](MoveKind kind, const Vec3& to, double feed) {
            ToolpathMove move;
            move.kind = kind;
            move.to = to;
            move.feed = feed;
            toolpath.moves.push_back(move);
        };
        const auto arc = [&](const Vec3& to, const Vec3& centre, double feed) {
            ToolpathMove move;
            move.kind = MoveKind::Feed;
            move.to = to;
            move.feed = feed;
            move.arc = true;
            move.centre = { centre.x, centre.y, to.z };
            move.clockwise = !writing.climb;    // climb goes anticlockwise, the material outside on the right
            toolpath.moves.push_back(move);
        };
        const auto lift = [&]() {
            if (!toolpath.moves.empty() && toolpath.moves.back().to.z < writing.clearance - 1e-9)
            {
                const Vec3 at = toolpath.moves.back().to;
                add(MoveKind::Rapid, { at.x, at.y, writing.clearance }, 0.0);
            }
        };
        const double enter = (std::min)(writing.enter, writing.clearance);
        const double z = writing.z;
        bool down = false;

        for (const AdaptiveCut& cut : cuts)
        {
            if (cut.points.empty())
                continue;
            const Vec3 first{ cut.points.front().x, cut.points.front().y, z };
            if (cut.entry == AdaptiveCut::Entry::Link && down)
            {
                add(MoveKind::Feed, first, writing.feeds.cut);
            }
            else if (cut.entry == AdaptiveCut::Entry::Helix && cut.helixRadius > 0.0)
            {
                lift();
                add(MoveKind::Rapid, { first.x, first.y, writing.clearance }, 0.0);
                const double top = (std::max)(writing.top, z);
                if (enter < writing.clearance - 1e-9 && enter > top + 1e-9)
                    add(MoveKind::Rapid, { first.x, first.y, enter }, 0.0);
                add(MoveKind::Feed, { first.x, first.y, top }, writing.feeds.plunge);

                // Half turns down at the ramp angle, then a flat turn at the level.
                const Vec3& c = cut.helixCentre;
                const double r = cut.helixRadius;
                double angle = std::atan2(first.y - c.y, first.x - c.x);
                const double turnSign = writing.climb ? 1.0 : -1.0;
                const double dropPerHalf = kPi * r * std::tan(writing.rampAngle * kPi / 180.0);
                const int halves = (top - z > 1e-9 && dropPerHalf > 1e-9)
                                       ? (std::max)(1, static_cast<int>(std::ceil((top - z) / dropPerHalf)))
                                       : 0;
                for (int k = 1; k <= halves + 2; ++k)
                {
                    angle += turnSign * kPi;
                    const double h = (k <= halves) ? top - (top - z) * k / halves : z;
                    arc({ c.x + r * std::cos(angle), c.y + r * std::sin(angle), h }, c, writing.feeds.engage);
                }
            }
            else
            {
                lift();
                add(MoveKind::Rapid, { first.x, first.y, writing.clearance }, 0.0);
                if (enter < writing.clearance - 1e-9 && enter > z + 1e-9)
                    add(MoveKind::Rapid, { first.x, first.y, enter }, 0.0);
                add(MoveKind::Feed, first, writing.feeds.plunge);
            }
            down = true;
            for (size_t k = 1; k < cut.points.size(); ++k)
                add(MoveKind::Feed, { cut.points[k].x, cut.points[k].y, z }, writing.feeds.cut);
        }
        lift();
    }
}
