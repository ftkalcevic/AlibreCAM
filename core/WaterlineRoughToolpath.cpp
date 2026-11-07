// WaterlineRoughToolpath.cpp : calculates a Waterline Roughing operation's toolpath.

#include "WaterlineRoughToolpath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

#include "Clearance.h"
#include "WaterlineRoughing.h"

namespace camcore
{
    namespace
    {
        const double kBig = 1e12;                 // more than any squared distance in cells
        const double kHeightTolerance = 1e-3;     // mm: heights closer than this are the same
        const size_t kMostMoves = 4000000;

        ToolpathResult Failure(const std::string& problem)
        {
            ToolpathResult result;
            result.problem = problem;
            return result;
        }

        // The distance from p to the segment a-b, in XY.
        double SegmentDistance(const Vec3& p, const Vec3& a, const Vec3& b)
        {
            const double ex = b.x - a.x, ey = b.y - a.y;
            const double lengthSquared = ex * ex + ey * ey;
            double t = 0.0;
            if (lengthSquared > 1e-18)
                t = (std::min)(1.0, (std::max)(0.0, ((p.x - a.x) * ex + (p.y - a.y) * ey) / lengthSquared));
            return std::hypot(p.x - (a.x + ex * t), p.y - (a.y + ey * t));
        }

        // Fewer points, none of the left out further than tolerance from the line kept.
        void Simplify(const Polyline& points, size_t first, size_t last, double tolerance, std::vector<char>& keep)
        {
            if (last <= first + 1)
                return;
            double worst = -1.0;
            size_t at = first;
            for (size_t i = first + 1; i < last; ++i)
            {
                const double d = SegmentDistance(points[i], points[first], points[last]);
                if (d > worst)
                {
                    worst = d;
                    at = i;
                }
            }
            if (worst > tolerance)
            {
                keep[at] = 1;
                Simplify(points, first, at, tolerance, keep);
                Simplify(points, at, last, tolerance, keep);
            }
        }

        // A closed loop with fewer points: split at the point furthest from its start.
        Polyline SimplifyLoop(const Polyline& loop, double tolerance)
        {
            if (loop.size() < 4)
                return loop;
            size_t far = 0;
            double best = -1.0;
            for (size_t i = 1; i < loop.size(); ++i)
            {
                const double d = std::hypot(loop[i].x - loop[0].x, loop[i].y - loop[0].y);
                if (d > best)
                {
                    best = d;
                    far = i;
                }
            }
            Polyline closed = loop;
            closed.push_back(loop[0]);
            std::vector<char> keep(closed.size(), 0);
            keep[0] = keep[far] = keep[closed.size() - 1] = 1;
            Simplify(closed, 0, far, tolerance, keep);
            Simplify(closed, far, closed.size() - 1, tolerance, keep);
            Polyline result;
            for (size_t i = 0; i + 1 < closed.size(); ++i)
            {
                if (keep[i])
                    result.push_back(closed[i]);
            }
            return result;
        }
    }

    std::vector<double> FlatHeights(const std::vector<Vec3>& triangles)
    {
        const double flat = std::cos(1.0 * 3.14159265358979323846 / 180.0);

        // Which way the triangles go round: a closed solid whose triangles go
        // anticlockwise seen from outside has a positive signed volume. If it's
        // negative, they go the other way, so each normal is turned round.
        double volume = 0.0;
        for (size_t t = 0; t + 2 < triangles.size(); t += 3)
        {
            const Vec3& a = triangles[t];
            const Vec3& b = triangles[t + 1];
            const Vec3& c = triangles[t + 2];
            volume += a.x * (b.y * c.z - b.z * c.y) - a.y * (b.x * c.z - b.z * c.x) + a.z * (b.x * c.y - b.y * c.x);
        }
        const double outward = (volume < 0.0) ? -1.0 : 1.0;

        std::vector<double> heights;
        for (size_t t = 0; t + 2 < triangles.size(); t += 3)
        {
            const Vec3& a = triangles[t];
            const Vec3& b = triangles[t + 1];
            const Vec3& c = triangles[t + 2];
            const Vec3 u = b - a, v = c - a;
            const Vec3 n{ u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
            const double length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            if (length < 1e-12 || outward * n.z / length < flat)
                continue;
            heights.push_back((a.z + b.z + c.z) / 3.0);
        }
        std::sort(heights.begin(), heights.end(), std::greater<double>());
        std::vector<double> merged;
        for (double h : heights)
        {
            if (merged.empty() || merged.back() - h > 1e-3)
                merged.push_back(h);
        }
        return merged;
    }

    std::vector<double> WaterlineLevels(double top, double bottom, const std::vector<double>& flatHeights,
                                        double stockToLeave, double step)
    {
        std::vector<double> levels;
        if (!(top > bottom + 1e-9) || !(step > 0.0))
            return levels;

        // The heights each gap runs between: the top, each flat face's level, the bottom.
        std::vector<double> marks{ top };
        for (double h : flatHeights)
        {
            const double level = h + stockToLeave;
            if (level < top - 1e-6 && level > bottom + 1e-6)
                marks.push_back(level);
        }
        marks.push_back(bottom);
        std::sort(marks.begin(), marks.end(), std::greater<double>());
        marks.erase(std::unique(marks.begin(), marks.end(), [](double a, double b) { return a - b < 1e-6; }),
                    marks.end());

        for (size_t m = 0; m + 1 < marks.size(); ++m)
        {
            const double from = marks[m], to = marks[m + 1];
            const int steps = (std::max)(1, static_cast<int>(std::ceil((from - to) / step - 1e-9)));
            for (int k = 1; k <= steps; ++k)
                levels.push_back(from - (from - to) * k / steps);
        }
        return levels;
    }

    std::vector<Polyline> IsoContours(const std::vector<float>& values, int nx, int ny, double xLow, double yLow,
                                      double cell, double level, double z)
    {
        std::vector<Polyline> loops;
        if (nx < 2 || ny < 2)
            return loops;
        const auto value = [&](int i, int j) { return static_cast<double>(values[static_cast<size_t>(j) * nx + i]); };
        const auto inside = [&](int i, int j) { return value(i, j) > level; };

        // Each grid edge between two centres: horizontal from (i, j) to (i + 1, j), or upright to (i, j + 1).
        const auto horizontal = [&](int i, int j) { return 2 * (static_cast<std::int64_t>(j) * nx + i); };
        const auto upright = [&](int i, int j) { return 2 * (static_cast<std::int64_t>(j) * nx + i) + 1; };
        const auto crossing = [&](std::int64_t edge) {
            const std::int64_t cellIndex = edge / 2;
            const int i = static_cast<int>(cellIndex % nx);
            const int j = static_cast<int>(cellIndex / nx);
            const int i2 = (edge % 2 == 0) ? i + 1 : i;
            const int j2 = (edge % 2 == 0) ? j : j + 1;
            const double v1 = value(i, j), v2 = value(i2, j2);
            const double t = (std::fabs(v2 - v1) > 1e-12) ? (level - v1) / (v2 - v1) : 0.5;
            const double x1 = xLow + (i + 0.5) * cell, y1 = yLow + (j + 0.5) * cell;
            const double x2 = xLow + (i2 + 0.5) * cell, y2 = yLow + (j2 + 0.5) * cell;
            return Vec3{ x1 + (x2 - x1) * t, y1 + (y2 - y1) * t, z };
        };

        // Each square of four centres gives segments with the higher side on their left.
        std::unordered_map<std::int64_t, std::int64_t> next;    // from the edge a segment starts on, to where it ends
        for (int j = 0; j + 1 < ny; ++j)
        {
            for (int i = 0; i + 1 < nx; ++i)
            {
                // Corners and edges anticlockwise: a (i, j), b (i+1, j), c (i+1, j+1), d (i, j+1).
                const bool in[4] = { inside(i, j), inside(i + 1, j), inside(i + 1, j + 1), inside(i, j + 1) };
                if (in[0] == in[1] && in[1] == in[2] && in[2] == in[3])
                    continue;
                const std::int64_t edges[4] = { horizontal(i, j), upright(i + 1, j), horizontal(i, j + 1), upright(i, j) };
                int outs[2], ins[2];
                int nOut = 0, nIn = 0;
                int order[4];
                int crossings = 0;
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
                // A saddle: the middle decides which corners are cut off.
                const bool middle = (value(i, j) + value(i + 1, j) + value(i + 1, j + 1) + value(i, j + 1)) / 4.0 > level;
                for (int k = 0; k < 4; ++k)
                {
                    const int e = order[k];
                    if (!in[e])
                        continue;   // not an out crossing
                    const int partner = middle ? order[(k + 1) % 4] : order[(k + 3) % 4];
                    next[edges[e]] = edges[partner];
                }
            }
        }

        // Joined into loops.
        while (!next.empty())
        {
            const std::int64_t start = next.begin()->first;
            Polyline loop;
            std::int64_t at = start;
            for (;;)
            {
                const auto found = next.find(at);
                if (found == next.end())
                    break;
                loop.push_back(crossing(at));
                const std::int64_t to = found->second;
                next.erase(found);
                at = to;
                if (at == start)
                    break;
            }
            if (loop.size() >= 3)
                loops.push_back(std::move(loop));
        }
        return loops;
    }

    ToolpathResult GenerateWaterlineRoughToolpath(const WaterlineRoughToolpathInput& input)
    {
        const SequenceStep* pOperation = input.operation;
        if (pOperation == nullptr || pOperation->type != StepType::WaterlineRoughing)
            return Failure("This isn't a waterline roughing operation.");
        if (input.tool == nullptr)
            return Failure("Choose a tool on the Tool tab.");
        if (input.outlines.empty() && input.faces.empty())
            return Failure("There's no area to rough. Pick faces, or edges making closed loops, or add the stock "
                           "box's footprint.");

        const WaterlineRoughingSettings& settings = pOperation->waterlineRoughing;
        for (const std::vector<std::string>& problems :
             { ValidateWaterlineRoughing(settings), ValidateFeeds(pOperation->feeds) })
        {
            if (!problems.empty())
                return Failure(problems.front());
        }
        const Tool& tool = *input.tool;
        if (!(tool.diameter > 0.0))
            return Failure("The tool's diameter must be more than 0.");

        double clearance = 0.0;
        std::string clearanceProblem;
        if (!ClearanceHeight(pOperation->clearance, input.havePartMaxZ, input.partMaxZ, clearance, clearanceProblem,
                             input.haveStockMaxZ, input.stockMaxZ))
            return Failure(clearanceProblem);

        // From the stock's top to its bottom, or the parts' without a stock.
        const double s = settings.stockToLeave;
        const double radius = tool.diameter / 2.0;
        const double reach = radius + s;
        const double pitch = tool.diameter * kWaterlineStepoverFraction;
        if (!input.haveStockMaxZ && !input.havePartMaxZ)
            return Failure("There's no stock or part to say where the top is.");
        const double top = input.haveStockMaxZ ? input.stockMaxZ : input.partMaxZ;
        const double bottom = input.haveStockMinZ ? input.stockMinZ : input.havePartMinZ ? input.partMinZ : top;
        if (!(top > bottom + 1e-9))
            return Failure("There's nothing between the stock's top and bottom to cut.");
        if (top + 1.0 > clearance + 1e-9)
            return Failure("The clearance plane must be at least 1 mm above the stock's top.");

        // The area, as Plunge Roughing's, and the parts' heights over it.
        double boxLowX = 0.0, boxHighX = 0.0, boxLowY = 0.0, boxHighY = 0.0;
        bool haveBox = false;
        const auto grow = [&](double x0, double x1, double y0, double y1) {
            boxLowX = haveBox ? (std::min)(boxLowX, x0) : x0;
            boxHighX = haveBox ? (std::max)(boxHighX, x1) : x1;
            boxLowY = haveBox ? (std::min)(boxLowY, y0) : y0;
            boxHighY = haveBox ? (std::max)(boxHighY, y1) : y1;
            haveBox = true;
        };
        for (const ProfileChain& outline : input.outlines)
        {
            if (!outline.closed)
                return Failure("Each group of edges must join into a closed loop.");
            double x0 = 0.0, x1 = 0.0, y0 = 0.0, y1 = 0.0;
            PathExtent(outline.path, { 1.0, 0.0, 0.0 }, x0, x1);
            PathExtent(outline.path, { 0.0, 1.0, 0.0 }, y0, y1);
            grow(x0, x1, y0, y1);
        }
        for (const std::vector<Vec3>& face : input.faces)
        {
            for (const Vec3& p : face)
                grow(p.x, p.x, p.y, p.y);
        }
        const double margin = reach + 1.0;
        const double cell = (std::min)(0.25, (std::max)(0.02, tool.diameter / 40.0));
        PlungeArea area;
        area.Reset(boxLowX, boxLowY, boxHighX, boxHighY, cell, margin);
        for (const ProfileChain& outline : input.outlines)
            area.AddLoop(outline.path);
        for (const std::vector<Vec3>& face : input.faces)
            area.AddFace(face);
        area.Finish();
        if (area.Empty())
            return Failure("The picked geometry covers no area seen from above.");

        PartHeightField heights;
        heights.Build(input.partTriangles, boxLowX - margin, boxLowY - margin, boxHighX + margin, boxHighY + margin,
                      (std::min)(0.25, (std::max)(0.02, tool.diameter / 40.0)));

        const int nx = area.CellsX(), ny = area.CellsY();
        const double gridCell = area.Cell();
        const double xLow = area.X(0) - gridCell / 2.0, yLow = area.Y(0) - gridCell / 2.0;
        const size_t cells = static_cast<size_t>(nx) * ny;
        std::vector<double> partHeight(cells, -kBig);
        std::vector<char> inArea(cells, 0);
        for (int j = 0; j < ny; ++j)
        {
            for (int i = 0; i < nx; ++i)
            {
                const size_t q = static_cast<size_t>(j) * nx + i;
                inArea[q] = area.Inside(i, j) ? 1 : 0;
                double h = 0.0;
                if (heights.HeightAt(area.X(i), area.Y(j), h))
                    partHeight[q] = h;
            }
        }

        const std::vector<double> levels = WaterlineLevels(top, bottom, FlatHeights(input.partTriangles), s,
                                                           settings.stepDown);
        const FeedSettings& feeds = pOperation->feeds;
        ToolpathResult result;
        Toolpath& path = result.toolpath;
        Vec3 at;
        bool started = false;
        const auto add = [&](MoveKind kind, const Vec3& to, double feed) {
            ToolpathMove move;
            move.kind = kind;
            move.to = to;
            move.feed = feed;
            path.moves.push_back(move);
            at = to;
        };
        const auto lift = [&]() {
            if (started && at.z < clearance - 1e-9)
                add(MoveKind::Rapid, { at.x, at.y, clearance }, 0.0);
        };

        double previous = top;
        const double t0 = gridCell / 2.0;
        std::vector<double> grid(cells);
        std::vector<float> free(cells);
        for (double z : levels)
        {
            // What's in the tool's way: part within the stock to leave below the level, or above it.
            // The heights are stored as floats, so a flat face exactly at the level
            // less the stock to leave must allow for their rounding.
            bool anyObstacle = false;
            for (size_t q = 0; q < cells; ++q)
            {
                const bool obstacle = partHeight[q] > z - s + kHeightTolerance;
                grid[q] = obstacle ? 0.0 : kBig;
                anyObstacle = anyObstacle || obstacle;
            }
            if (anyObstacle)
                SquaredDistanceTransform(grid, nx, ny);

            // Where the tool's centre may go, then how far each such place is from the edge of it.
            bool anyFree = false;
            for (size_t q = 0; q < cells; ++q)
            {
                const bool clear = inArea[q] && (!anyObstacle || std::sqrt(grid[q]) * gridCell - gridCell / 2.0 >= reach);
                grid[q] = clear ? kBig : 0.0;
                anyFree = anyFree || clear;
            }
            if (!anyFree)
            {
                previous = z;
                continue;
            }
            SquaredDistanceTransform(grid, nx, ny);
            float deepest = 0.0f;
            for (size_t q = 0; q < cells; ++q)
            {
                free[q] = static_cast<float>(std::sqrt(grid[q]) * gridCell);
                deepest = (std::max)(deepest, free[q]);
            }

            // The loops, a stepover apart from the edge in, each turned to cut climb.
            std::vector<std::vector<Polyline>> rings;
            for (double t = t0; t < deepest; t += pitch)
            {
                std::vector<Polyline> loops = IsoContours(free, nx, ny, xLow, yLow, gridCell, t, z);
                for (Polyline& loop : loops)
                {
                    loop = SimplifyLoop(loop, gridCell / 4.0);
                    std::reverse(loop.begin(), loop.end());
                }
                if (!loops.empty())
                    rings.push_back(std::move(loops));
            }

            // A move stays down if it's short and stays where the tool may be.
            const auto inside = [&](const Vec3& p) {
                const int i = static_cast<int>(std::floor((p.x - xLow) / gridCell));
                const int j = static_cast<int>(std::floor((p.y - yLow) / gridCell));
                return i >= 0 && j >= 0 && i < nx && j < ny && free[static_cast<size_t>(j) * nx + i] >= t0;
            };
            const auto canStayDown = [&](const Vec3& from, const Vec3& to) {
                const double length = std::hypot(to.x - from.x, to.y - from.y);
                if (length > 3.0 * pitch)
                    return false;
                const int samples = (std::max)(1, static_cast<int>(std::ceil(length / (gridCell / 2.0))));
                for (int k = 0; k <= samples; ++k)
                {
                    const double f = static_cast<double>(k) / samples;
                    if (!inside({ from.x + (to.x - from.x) * f, from.y + (to.y - from.y) * f, z }))
                        return false;
                }
                return true;
            };
            const double enter = (std::min)(previous + 1.0, clearance);

            bool down = false;
            for (std::vector<Polyline>& loops : rings)
            {
                std::vector<char> done(loops.size(), 0);
                for (size_t n = 0; n < loops.size(); ++n)
                {
                    // The nearest loop, from its nearest point.
                    size_t best = 0, bestPoint = 0;
                    double bestDistance = -1.0;
                    for (size_t l = 0; l < loops.size(); ++l)
                    {
                        if (done[l])
                            continue;
                        for (size_t p = 0; p < loops[l].size(); ++p)
                        {
                            const double d = std::hypot(loops[l][p].x - at.x, loops[l][p].y - at.y);
                            if (bestDistance < 0.0 || d < bestDistance)
                            {
                                bestDistance = d;
                                best = l;
                                bestPoint = p;
                            }
                        }
                    }
                    done[best] = 1;
                    Polyline& loop = loops[best];
                    std::rotate(loop.begin(), loop.begin() + static_cast<std::ptrdiff_t>(bestPoint), loop.end());
                    const Vec3& start = loop.front();

                    if (down && canStayDown(at, start))
                    {
                        add(MoveKind::Feed, start, feeds.cut);
                    }
                    else
                    {
                        lift();
                        add(MoveKind::Rapid, { start.x, start.y, clearance }, 0.0);
                        if (enter < clearance - 1e-9)
                            add(MoveKind::Rapid, { start.x, start.y, enter }, 0.0);
                        add(MoveKind::Feed, start, feeds.plunge);
                        started = true;
                        down = true;
                    }
                    for (size_t p = 1; p < loop.size(); ++p)
                        add(MoveKind::Feed, loop[p], feeds.cut);
                    add(MoveKind::Feed, start, feeds.cut);
                    if (path.moves.size() > kMostMoves)
                        return Failure("The toolpath would be too long. Make the cut depth step larger.");
                }
            }
            lift();
            previous = z;
        }
        if (!started)
            return Failure("There's nothing to cut: the tool doesn't fit anywhere in the area.");

        result.ok = true;
        return result;
    }
}
