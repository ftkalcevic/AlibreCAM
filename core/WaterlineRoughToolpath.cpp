// WaterlineRoughToolpath.cpp : calculates a Waterline Roughing operation's toolpath.

#include "WaterlineRoughToolpath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

#include "Clearance.h"
#include "FacingToolpath.h"     // PassDirection
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

        // An open run with fewer points, its ends kept.
        Polyline SimplifyRun(const Polyline& run, double tolerance)
        {
            if (run.size() < 3)
                return run;
            std::vector<char> keep(run.size(), 0);
            keep.front() = keep.back() = 1;
            Simplify(run, 0, run.size() - 1, tolerance, keep);
            Polyline result;
            for (size_t i = 0; i < run.size(); ++i)
            {
                if (keep[i])
                    result.push_back(run[i]);
            }
            return result;
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

    namespace
    {
        // A stretch of toolpath at one level: a closed loop, or an open run.
        struct Piece
        {
            Polyline points;
            bool closed = true;
            bool fixedDirection = true;     // false: it may be cut either way round
            int component = -1;             // which separate part of the level's area it's in
        };

        // The cells a 4-connected flood reaches from each free cell, numbered from 0.
        int LabelComponents(const std::vector<char>& free, int nx, int ny, std::vector<int>& labels)
        {
            labels.assign(free.size(), -1);
            int count = 0;
            std::vector<size_t> stack;
            for (size_t seed = 0; seed < free.size(); ++seed)
            {
                if (!free[seed] || labels[seed] >= 0)
                    continue;
                labels[seed] = count;
                stack.push_back(seed);
                while (!stack.empty())
                {
                    const size_t q = stack.back();
                    stack.pop_back();
                    const int i = static_cast<int>(q % nx), j = static_cast<int>(q / nx);
                    const size_t around[4] = { i > 0 ? q - 1 : q, i + 1 < nx ? q + 1 : q, j > 0 ? q - nx : q,
                                               j + 1 < ny ? q + nx : q };
                    for (size_t n : around)
                    {
                        if (free[n] && labels[n] < 0)
                        {
                            labels[n] = count;
                            stack.push_back(n);
                        }
                    }
                }
                ++count;
            }
            return count;
        }

        // A distance in mm from each cell to the nearest cell where `from` is set.
        std::vector<float> DistanceFrom(const std::vector<char>& from, int nx, int ny, double cell, bool& any)
        {
            std::vector<double> grid(from.size());
            any = false;
            for (size_t q = 0; q < from.size(); ++q)
            {
                grid[q] = from[q] ? 0.0 : kBig;
                any = any || from[q];
            }
            if (any)
                SquaredDistanceTransform(grid, nx, ny);
            std::vector<float> distance(from.size());
            for (size_t q = 0; q < from.size(); ++q)
                distance[q] = static_cast<float>(any ? std::sqrt(grid[q]) * cell : 1e9);
            return distance;
        }
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

        // The tool's sizes.
        const double s = settings.stockToLeave;
        const double radius = tool.diameter / 2.0;
        const double reach = radius + s;
        const double cornerRadius = (tool.type == ToolType::BallMill)           ? radius
                                    : (tool.type == ToolType::CornerRadiusMill) ? tool.cornerRadius
                                                                                : 0.0;
        const double pitch = WaterlineStepover(settings, tool.diameter, cornerRadius);
        if (!(pitch > 0.0))
            return Failure("The stepover must be more than 0.");

        // From the top down to the bottom: the stock's (or the parts'), unless forced.
        if (!settings.useTop && !input.haveStockMaxZ && !input.havePartMaxZ)
            return Failure("There's no stock or part to say where the top is.");
        const double top = settings.useTop ? settings.top : input.haveStockMaxZ ? input.stockMaxZ : input.partMaxZ;
        const double naturalBottom = input.haveStockMinZ ? input.stockMinZ : input.havePartMinZ ? input.partMinZ : top;
        const double bottom = settings.useBottom ? settings.bottom : naturalBottom;
        if (!(top > bottom + 1e-9))
            return Failure("There's nothing between the top and the bottom to cut.");
        if (top + 1.0 > clearance + 1e-9)
            return Failure("The clearance plane must be at least 1 mm above the top of the cut.");
        const double step = WaterlineStepdown(settings, tool.diameter, top, bottom);
        if (!(step > 0.0))
            return Failure("The stepdown must be more than 0.");

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
        const double margin = reach + settings.extension + 1.0;
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
        const auto cellAt = [&](const Vec3& p, size_t& q) {
            const int i = static_cast<int>(std::floor((p.x - xLow) / gridCell));
            const int j = static_cast<int>(std::floor((p.y - yLow) / gridCell));
            if (i < 0 || j < 0 || i >= nx || j >= ny)
                return false;
            q = static_cast<size_t>(j) * nx + i;
            return true;
        };

        // Where the tool's centre may go at all: the area, and past its open edges
        // by the linear extension (parts still keep it out below).
        std::vector<char> inArea(cells, 0);
        std::vector<double> partHeight(cells, -kBig);
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
        if (settings.extension > 0.0)
        {
            bool any = false;
            const std::vector<float> fromArea = DistanceFrom(inArea, nx, ny, gridCell, any);
            for (size_t q = 0; q < cells; ++q)
                inArea[q] = (fromArea[q] <= settings.extension + 1e-9) ? 1 : 0;
        }
        bool anyArea = false;
        std::vector<char> outsideArea(cells);
        for (size_t q = 0; q < cells; ++q)
            outsideArea[q] = inArea[q] ? 0 : 1;
        const std::vector<float> fromAreaEdge = DistanceFrom(outsideArea, nx, ny, gridCell, anyArea);

        const std::vector<double> levels =
            WaterlineLevels(top, bottom, settings.clearFlats ? FlatHeights(input.partTriangles) : std::vector<double>(),
                            s, step);

        // Each level's separate areas, their pieces of toolpath, and which area
        // above each sits under.
        struct Region
        {
            size_t level = 0;
            int parent = -1;            // in the level above, or -1
            std::vector<Piece> pieces;
            std::vector<int> children;  // indexes into regions
        };
        std::vector<Region> regions;
        std::vector<int> previousLabels;
        int previousBase = 0;
        std::vector<std::vector<float>> freeFields(levels.size());
        const double t0 = gridCell / 2.0;
        const Vec3 along = PassDirection(settings.cutAngle);
        const Vec3 across{ -along.y, along.x, 0.0 };

        for (size_t li = 0; li < levels.size(); ++li)
        {
            const double z = levels[li];

            // What's in the tool's way: part within the stock to leave below the level, or above it.
            std::vector<char> obstacle(cells);
            for (size_t q = 0; q < cells; ++q)
                obstacle[q] = (partHeight[q] > z - s + kHeightTolerance) ? 1 : 0;
            bool anyObstacle = false;
            const std::vector<float> fromPart = DistanceFrom(obstacle, nx, ny, gridCell, anyObstacle);

            std::vector<char> free(cells, 0);
            std::vector<char> notFree(cells, 1);
            bool anyFree = false;
            for (size_t q = 0; q < cells; ++q)
            {
                free[q] = inArea[q] && (!anyObstacle || fromPart[q] - gridCell / 2.0 >= reach);
                notFree[q] = free[q] ? 0 : 1;
                anyFree = anyFree || free[q];
            }
            if (!anyFree)
            {
                previousLabels.assign(cells, -1);
                continue;
            }
            bool anyEdge = false;
            std::vector<float>& fromFreeEdge = freeFields[li];
            fromFreeEdge = DistanceFrom(notFree, nx, ny, gridCell, anyEdge);

            std::vector<int> labels;
            const int count = LabelComponents(free, nx, ny, labels);
            const int base = static_cast<int>(regions.size());
            for (int c = 0; c < count; ++c)
            {
                Region region;
                region.level = li;
                regions.push_back(region);
            }
            // Each region's parent: the region above holding its first cell.
            for (size_t q = 0; q < cells; ++q)
            {
                if (labels[q] < 0)
                    continue;
                Region& region = regions[static_cast<size_t>(base + labels[q])];
                if (region.parent == -1 && !previousLabels.empty() && previousLabels[q] >= 0)
                {
                    region.parent = previousBase + previousLabels[q];
                    regions[static_cast<size_t>(region.parent)].children.push_back(base + labels[q]);
                }
            }

            // Pieces from a field's contour, kept to where the tool may go.
            const auto inside = [&](const Vec3& p) {
                size_t q = 0;
                return cellAt(p, q) && free[q] != 0;
            };
            std::vector<Piece> pieces;
            // Contours that cross the edge of where the tool may go are clipped to it,
            // so they start a cell in from it, where they're clearly inside.
            const auto addContours = [&](const std::vector<float>& field, double level, bool materialHigher,
                                         bool alternate, bool& flip, bool clip) {
                for (Polyline loop : IsoContours(field, nx, ny, xLow, yLow, gridCell, level, z))
                {
                    // Climb keeps the material on the right; the contours have the higher side on the left.
                    bool reverse = materialHigher;
                    if (settings.direction == CutDirection::Conventional)
                        reverse = !reverse;
                    if (settings.direction == CutDirection::Mixed && alternate)
                        reverse = flip;
                    if (reverse)
                        std::reverse(loop.begin(), loop.end());

                    // Split where it leaves the area the tool may reach.
                    size_t outsideAt = loop.size();
                    for (size_t p = 0; clip && p < loop.size(); ++p)
                    {
                        if (!inside(loop[p]))
                        {
                            outsideAt = p;
                            break;
                        }
                    }
                    // Clipped while it still has a point in every cell it crosses, then simplified.
                    if (outsideAt == loop.size())
                    {
                        Piece piece;
                        piece.points = SimplifyLoop(loop, gridCell / 4.0);
                        pieces.push_back(std::move(piece));
                        continue;
                    }
                    std::rotate(loop.begin(), loop.begin() + static_cast<std::ptrdiff_t>(outsideAt), loop.end());
                    Polyline run;
                    for (size_t p = 0; p <= loop.size(); ++p)
                    {
                        const bool in = p < loop.size() && inside(loop[p]);
                        if (in)
                        {
                            run.push_back(loop[p]);
                            continue;
                        }
                        if (run.size() >= 2)
                        {
                            Piece piece;
                            piece.points = SimplifyRun(run, gridCell / 4.0);
                            piece.closed = false;
                            pieces.push_back(std::move(piece));
                        }
                        run.clear();
                    }
                }
                flip = !flip;
            };

            bool flip = false;
            const WaterlinePattern pattern =
                (settings.pattern == WaterlinePattern::PartOffset && !anyObstacle) ? WaterlinePattern::StockOffset
                                                                                   : settings.pattern;
            const bool inward = settings.start == WaterlineStart::Inside;
            if (pattern == WaterlinePattern::PartOffset)
            {
                // Loops round the parts, each a stepover further out, cut off at the area's edge.
                std::vector<double> ts;
                float deepest = 0.0f;
                for (size_t q = 0; q < cells; ++q)
                {
                    if (free[q])
                        deepest = (std::max)(deepest, fromPart[q]);
                }
                for (double t = reach + gridCell; t <= deepest; t += pitch)
                    ts.push_back(t);
                if (!inward)
                    std::reverse(ts.begin(), ts.end());
                for (double t : ts)
                    addContours(fromPart, t, inward, true, flip, true);
            }
            else if (pattern == WaterlinePattern::StockOffset)
            {
                // Loops in from the area's edge, cut off at the parts, then a loop round the parts.
                std::vector<double> ts;
                float deepest = 0.0f;
                for (size_t q = 0; q < cells; ++q)
                {
                    if (free[q])
                        deepest = (std::max)(deepest, fromAreaEdge[q]);
                }
                for (double t = gridCell; t < deepest; t += pitch)
                    ts.push_back(t);
                if (inward)
                {
                    std::reverse(ts.begin(), ts.end());
                    if (anyObstacle)
                        addContours(fromPart, reach + gridCell, true, false, flip, true);
                }
                for (double t : ts)
                    addContours(fromAreaEdge, t, !inward, true, flip, true);
                if (!inward && anyObstacle)
                    addContours(fromPart, reach + gridCell, true, false, flip, true);
            }
            else
            {
                // Straight rows across, a stepover apart, then a lap round the edge.
                double low = 0.0, high = 0.0, uLow = 0.0, uHigh = 0.0;
                bool found = false;
                for (int j = 0; j < ny; ++j)
                {
                    for (int i = 0; i < nx; ++i)
                    {
                        if (!free[static_cast<size_t>(j) * nx + i])
                            continue;
                        const double v = area.X(i) * across.x + area.Y(j) * across.y;
                        const double u = area.X(i) * along.x + area.Y(j) * along.y;
                        low = found ? (std::min)(low, v) : v;
                        high = found ? (std::max)(high, v) : v;
                        uLow = found ? (std::min)(uLow, u) : u;
                        uHigh = found ? (std::max)(uHigh, u) : u;
                        found = true;
                    }
                }
                const int intervals = (std::max)(1, static_cast<int>(std::ceil((high - low) / pitch - 1e-9)));
                const double spacing = (high - low) / intervals;
                // Start at Bottom starts on the frame's -Y side, as Pocketing's Linear Cuts do.
                const bool acrossGoesUp = std::fabs(across.y) > 1e-9 ? across.y > 0.0 : across.x < 0.0;
                const bool fromLow = (settings.linearStart == PocketLinearStart::Bottom) == acrossGoesUp;
                const double stepSign = fromLow ? 1.0 : -1.0;
                // As Pocketing: the uncut side is the way the rows step, and climb keeps it on the right.
                const bool climbForward = stepSign < 0.0;
                bool forward = (settings.direction == CutDirection::Conventional) ? !climbForward : climbForward;
                const double sample = gridCell / 2.0;
                for (int r = 0; r <= intervals; ++r)
                {
                    const double v = fromLow ? low + r * spacing : high - r * spacing;
                    std::vector<Polyline> runs;
                    Polyline run;
                    for (double u = uLow - gridCell; u <= uHigh + gridCell + 1e-9; u += sample)
                    {
                        const Vec3 p = along * u + across * v;
                        const Vec3 point{ p.x, p.y, z };
                        if (inside(point))
                        {
                            run.push_back(point);
                            continue;
                        }
                        if (run.size() >= 2)
                            runs.push_back({ run.front(), run.back() });
                        run.clear();
                    }
                    if (run.size() >= 2)
                        runs.push_back({ run.front(), run.back() });
                    if (!forward)
                    {
                        std::reverse(runs.begin(), runs.end());
                        for (Polyline& each : runs)
                            std::reverse(each.begin(), each.end());
                    }
                    for (Polyline& each : runs)
                    {
                        Piece piece;
                        piece.points = each;
                        piece.closed = false;
                        pieces.push_back(std::move(piece));
                    }
                    if (settings.direction == CutDirection::Mixed)
                        forward = !forward;
                }
                addContours(fromFreeEdge, t0, true, false, flip, false);
            }

            for (Piece& piece : pieces)
            {
                size_t q = 0;
                if (!piece.points.empty() && cellAt(piece.points.front(), q) && labels[q] >= 0)
                    piece.component = labels[q];
                else
                    piece.component = 0;
                piece.fixedDirection = !(settings.direction == CutDirection::Mixed && !piece.closed &&
                                         pattern != WaterlinePattern::Linear);
                regions[static_cast<size_t>(base + piece.component)].pieces.push_back(std::move(piece));
            }
            previousLabels = labels;
            previousBase = base;
        }

        // The order regions are cut in: level by level, or each down to its bottom first.
        std::vector<int> order;
        if (settings.order == CutLevelOrder::LevelFirst)
        {
            for (size_t r = 0; r < regions.size(); ++r)
                order.push_back(static_cast<int>(r));
        }
        else
        {
            std::vector<int> stack;
            for (int r = static_cast<int>(regions.size()) - 1; r >= 0; --r)
            {
                if (regions[static_cast<size_t>(r)].parent == -1)
                    stack.push_back(r);
            }
            while (!stack.empty())
            {
                const int r = stack.back();
                stack.pop_back();
                order.push_back(r);
                const std::vector<int>& children = regions[static_cast<size_t>(r)].children;
                for (auto c = children.rbegin(); c != children.rend(); ++c)
                    stack.push_back(*c);
            }
        }

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

        for (int r : order)
        {
            Region& region = regions[static_cast<size_t>(r)];
            if (region.pieces.empty())
                continue;
            const double z = levels[region.level];
            const double above = (region.parent >= 0) ? levels[regions[static_cast<size_t>(region.parent)].level]
                                                      : (region.level > 0 ? levels[region.level - 1] : top);
            const double enter = (std::min)(above + 1.0, clearance);
            const std::vector<float>& field = freeFields[region.level];

            // A move stays down if it's short and stays where the tool may be.
            const auto canStayDown = [&](const Vec3& from, const Vec3& to) {
                const double length = std::hypot(to.x - from.x, to.y - from.y);
                if (length > 3.0 * pitch)
                    return false;
                const int samples = (std::max)(1, static_cast<int>(std::ceil(length / (gridCell / 2.0))));
                for (int k = 0; k <= samples; ++k)
                {
                    const double f = static_cast<double>(k) / samples;
                    size_t q = 0;
                    if (!cellAt({ from.x + (to.x - from.x) * f, from.y + (to.y - from.y) * f, z }, q) ||
                        field[q] < t0)
                        return false;
                }
                return true;
            };

            lift();
            bool down = false;
            for (Piece& piece : region.pieces)
            {
                Polyline& points = piece.points;
                if (points.empty())
                    continue;
                if (piece.closed)
                {
                    // A loop starts at its point nearest the tool.
                    size_t nearest = 0;
                    double best = -1.0;
                    for (size_t p = 0; p < points.size(); ++p)
                    {
                        const double d = std::hypot(points[p].x - at.x, points[p].y - at.y);
                        if (best < 0.0 || d < best)
                        {
                            best = d;
                            nearest = p;
                        }
                    }
                    std::rotate(points.begin(), points.begin() + static_cast<std::ptrdiff_t>(nearest), points.end());
                }
                else if (!piece.fixedDirection &&
                         std::hypot(points.back().x - at.x, points.back().y - at.y) <
                             std::hypot(points.front().x - at.x, points.front().y - at.y))
                {
                    std::reverse(points.begin(), points.end());
                }

                const Vec3& start = points.front();
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
                for (size_t p = 1; p < points.size(); ++p)
                    add(MoveKind::Feed, points[p], feeds.cut);
                if (piece.closed)
                    add(MoveKind::Feed, start, feeds.cut);
                if (path.moves.size() > kMostMoves)
                    return Failure("The toolpath would be too long. Make the stepdown or stepover larger.");
            }
            lift();
        }
        if (!started)
            return Failure("There's nothing to cut: the tool doesn't fit anywhere in the area.");

        result.ok = true;
        return result;
    }
}
