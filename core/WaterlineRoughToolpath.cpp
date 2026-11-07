// WaterlineRoughToolpath.cpp : calculates a Waterline Roughing operation's toolpath.

#include "WaterlineRoughToolpath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <execution>
#include <limits>
#include <unordered_map>

#include "Clearance.h"
#include "DropCutter.h"
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

        // The cells a 4-connected flood reaches from each set cell, numbered from 0.
        int LabelComponents(const std::vector<char>& set, int nx, int ny, std::vector<int>& labels)
        {
            labels.assign(set.size(), -1);
            int count = 0;
            std::vector<size_t> stack;
            for (size_t seed = 0; seed < set.size(); ++seed)
            {
                if (!set[seed] || labels[seed] >= 0)
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
                        if (set[n] && labels[n] < 0)
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

        // Everything both waterline operations work from: the settings and tool, the
        // top, bottom and levels, and on a grid in the frame's XY, the area and how
        // low the tool's tip can go in each cell.
        struct WaterlineSetup
        {
            const WaterlineRoughingSettings* pSettings = nullptr;
            double clearance = 0.0;
            double s = 0.0;                 // the stock to leave
            double radius = 0.0;
            double pitch = 0.0;             // the stepover
            double top = 0.0;
            double bottom = 0.0;
            std::vector<double> levels;

            int nx = 0;
            int ny = 0;
            double cell = 1.0;
            double xLow = 0.0;
            double yLow = 0.0;
            size_t cells = 0;
            std::vector<char> inArea;       // where the tool's centre may go at all
            std::vector<float> tips;        // the lowest the tip may go; lowest() with no part near

            bool CellAt(const Vec3& p, size_t& q) const
            {
                const int i = static_cast<int>(std::floor((p.x - xLow) / cell));
                const int j = static_cast<int>(std::floor((p.y - yLow) / cell));
                if (i < 0 || j < 0 || i >= nx || j >= ny)
                    return false;
                q = static_cast<size_t>(j) * nx + i;
                return true;
            }

            // Blocked at a level: the tool can't come down that low there.
            bool Blocked(size_t q, double z) const { return tips[q] > z + kHeightTolerance; }
        };

        bool PrepareWaterline(const WaterlineRoughToolpathInput& input, StepType type, WaterlineSetup& w,
                              std::string& problem)
        {
            const SequenceStep* pOperation = input.operation;
            const bool roughing = type == StepType::WaterlineRoughing;
            if (pOperation == nullptr || pOperation->type != type)
            {
                problem = roughing                              ? "This isn't a waterline roughing operation."
                          : type == StepType::WaterlineFinishing ? "This isn't a waterline finishing operation."
                                                                 : "This isn't a parallel finishing operation.";
                return false;
            }
            if (input.tool == nullptr)
            {
                problem = "Choose a tool on the Tool tab.";
                return false;
            }
            if (input.outlines.empty() && input.faces.empty())
            {
                problem = "There's no area to cut. Pick faces, or edges making closed loops, or add the stock box's "
                          "footprint.";
                return false;
            }

            const WaterlineRoughingSettings& settings = WaterlineSettingsOf(*pOperation);
            w.pSettings = &settings;
            for (const std::vector<std::string>& problems :
                 { ValidateWaterlineRoughing(settings), ValidateFeeds(pOperation->feeds) })
            {
                if (!problems.empty())
                {
                    problem = problems.front();
                    return false;
                }
            }
            const Tool& tool = *input.tool;
            if (!(tool.diameter > 0.0))
            {
                problem = "The tool's diameter must be more than 0.";
                return false;
            }
            if (!ClearanceHeight(pOperation->clearance, input.havePartMaxZ, input.partMaxZ, w.clearance, problem,
                                 input.haveStockMaxZ, input.stockMaxZ))
                return false;

            // The tool's sizes.
            w.s = settings.stockToLeave;
            w.radius = tool.diameter / 2.0;
            const double reach = w.radius + w.s;
            const double cornerRadius = (tool.type == ToolType::BallMill)           ? w.radius
                                        : (tool.type == ToolType::CornerRadiusMill) ? tool.cornerRadius
                                                                                    : 0.0;
            w.pitch = WaterlineStepover(settings, tool.diameter, cornerRadius);
            if (!(w.pitch > 0.0))
            {
                problem = "The stepover must be more than 0.";
                return false;
            }

            // From the top down to the bottom: the stock's (or the parts'), unless forced.
            if (!settings.useTop && !input.haveStockMaxZ && !input.havePartMaxZ)
            {
                problem = "There's no stock or part to say where the top is.";
                return false;
            }
            w.top = settings.useTop ? settings.top : input.haveStockMaxZ ? input.stockMaxZ : input.partMaxZ;
            const double naturalBottom = input.haveStockMinZ ? input.stockMinZ : input.havePartMinZ ? input.partMinZ : w.top;
            w.bottom = settings.useBottom ? settings.bottom : naturalBottom;
            if (!(w.top > w.bottom + 1e-9))
            {
                problem = "There's nothing between the top and the bottom to cut.";
                return false;
            }
            if (w.top + 1.0 > w.clearance + 1e-9)
            {
                problem = "The clearance plane must be at least 1 mm above the top of the cut.";
                return false;
            }
            // Parallel Finishing is one pass over the whole depth: it has no stepdown.
            const bool singlePass = type == StepType::ParallelFinishing;
            if (!singlePass && settings.stepdownMode == WaterlineStepdownMode::Scallop &&
                tool.type != ToolType::BallMill)
            {
                problem = "A stepdown by scallop height needs a ball nose end mill. Choose another stepdown on the "
                          "Cut Levels tab, or a ball mill.";
                return false;
            }
            const double step = singlePass ? (w.top - w.bottom)
                                           : WaterlineStepdown(settings, tool.diameter, w.top, w.bottom,
                                                               tool.type == ToolType::BallMill ? w.radius : 0.0);
            if (!(step > 0.0))
            {
                problem = "The stepdown must be more than 0.";
                return false;
            }

            // The area, as Plunge Roughing's.
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
                {
                    problem = "Each group of edges must join into a closed loop.";
                    return false;
                }
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
            const double extension = (type == StepType::WaterlineFinishing) ? 0.0 : settings.extension;
            const double margin = reach + extension + 1.0;
            PlungeArea area;
            area.Reset(boxLowX, boxLowY, boxHighX, boxHighY, (std::min)(0.25, (std::max)(0.02, tool.diameter / 40.0)),
                       margin);
            for (const ProfileChain& outline : input.outlines)
                area.AddLoop(outline.path);
            for (const std::vector<Vec3>& face : input.faces)
                area.AddFace(face);
            area.Finish();
            if (area.Empty())
            {
                problem = "The picked geometry covers no area seen from above.";
                return false;
            }

            w.nx = area.CellsX();
            w.ny = area.CellsY();
            w.cell = area.Cell();
            w.xLow = area.X(0) - w.cell / 2.0;
            w.yLow = area.Y(0) - w.cell / 2.0;
            w.cells = static_cast<size_t>(w.nx) * w.ny;

            // How low the tip can go in each cell, for the tool's own shape.
            PartHeightField heights;
            heights.BuildGrid(input.partTriangles, w.xLow, w.yLow, w.nx, w.ny, w.cell);
            w.tips = heights.TipHeights(ProfileOf(tool), w.s);

            // Where the tool's centre may go at all: the area, and for roughing past
            // its open edges by the linear extension (parts still keep it out below).
            w.inArea.assign(w.cells, 0);
            for (int j = 0; j < w.ny; ++j)
            {
                for (int i = 0; i < w.nx; ++i)
                    w.inArea[static_cast<size_t>(j) * w.nx + i] = area.Inside(i, j) ? 1 : 0;
            }
            if (extension > 0.0)
            {
                bool any = false;
                const std::vector<float> fromArea = DistanceFrom(w.inArea, w.nx, w.ny, w.cell, any);
                for (size_t q = 0; q < w.cells; ++q)
                    w.inArea[q] = (fromArea[q] <= extension + 1e-9) ? 1 : 0;
            }

            // Finish Flats needs a level at each flat, with Clear Flats or not.
            const bool flatLevels = settings.clearFlats || (type == StepType::WaterlineFinishing && settings.finishFlats);
            if (singlePass)
                w.levels = { w.bottom };
            else
                w.levels = WaterlineLevels(w.top, w.bottom,
                                           flatLevels ? FlatHeights(input.partTriangles) : std::vector<double>(),
                                           w.s, step);
            return true;
        }

        // Writes the moves, lifting to the clearance plane and coming down again
        // where a link can't stay down.
        class MoveWriter
        {
        public:
            MoveWriter(Toolpath& path, double clearance) : m_path(path), m_clearance(clearance) {}

            void Add(MoveKind kind, const Vec3& to, double feed)
            {
                ToolpathMove move;
                move.kind = kind;
                move.to = to;
                move.feed = feed;
                m_path.moves.push_back(move);
                m_at = to;
            }

            void Lift()
            {
                if (m_started && m_at.z < m_clearance - 1e-9)
                    Add(MoveKind::Rapid, { m_at.x, m_at.y, m_clearance }, 0.0);
            }

            // Over the start at the clearance plane, down at rapid to enter, then
            // feeding down at the plunge feed.
            void Enter(const Vec3& start, double enter, double plunge)
            {
                Lift();
                Add(MoveKind::Rapid, { start.x, start.y, m_clearance }, 0.0);
                if (enter < m_clearance - 1e-9 && enter > start.z)
                    Add(MoveKind::Rapid, { start.x, start.y, enter }, 0.0);
                Add(MoveKind::Feed, start, plunge);
                m_started = true;
            }

            const Vec3& At() const { return m_at; }
            bool Started() const { return m_started; }
            size_t Count() const { return m_path.moves.size(); }

        private:
            Toolpath& m_path;
            double m_clearance;
            Vec3 m_at;
            bool m_started = false;
        };

        // A loop starts at its point nearest p.
        void StartNearest(Polyline& points, const Vec3& p)
        {
            size_t nearest = 0;
            double best = -1.0;
            for (size_t i = 0; i < points.size(); ++i)
            {
                const double d = std::hypot(points[i].x - p.x, points[i].y - p.y);
                if (best < 0.0 || d < best)
                {
                    best = d;
                    nearest = i;
                }
            }
            std::rotate(points.begin(), points.begin() + static_cast<std::ptrdiff_t>(nearest), points.end());
        }

        // A contour split where it leaves the cells `keep` allows, each piece simplified.
        std::vector<Piece> ClipContour(Polyline loop, const WaterlineSetup& w, const std::vector<char>& keep, bool clip)
        {
            std::vector<Piece> pieces;
            const auto inside = [&](const Vec3& p) {
                size_t q = 0;
                return w.CellAt(p, q) && keep[q] != 0;
            };
            size_t outsideAt = loop.size();
            for (size_t p = 0; clip && p < loop.size(); ++p)
            {
                if (!inside(loop[p]))
                {
                    outsideAt = p;
                    break;
                }
            }
            if (outsideAt == loop.size())
            {
                Piece piece;
                piece.points = SimplifyLoop(loop, w.cell / 4.0);
                pieces.push_back(std::move(piece));
                return pieces;
            }
            std::rotate(loop.begin(), loop.begin() + static_cast<std::ptrdiff_t>(outsideAt), loop.end());
            Polyline run;
            for (size_t p = 0; p <= loop.size(); ++p)
            {
                if (p < loop.size() && inside(loop[p]))
                {
                    run.push_back(loop[p]);
                    continue;
                }
                if (run.size() >= 2)
                {
                    Piece piece;
                    piece.points = SimplifyRun(run, w.cell / 4.0);
                    piece.closed = false;
                    pieces.push_back(std::move(piece));
                }
                run.clear();
            }
            return pieces;
        }
    }

    ToolpathResult GenerateWaterlineRoughToolpath(const WaterlineRoughToolpathInput& input)
    {
        WaterlineSetup w;
        std::string problem;
        if (!PrepareWaterline(input, StepType::WaterlineRoughing, w, problem))
            return Failure(problem);
        const WaterlineRoughingSettings& settings = *w.pSettings;
        const int nx = w.nx, ny = w.ny;
        const size_t cells = w.cells;
        const double gridCell = w.cell;
        const double pitch = w.pitch;
        const std::vector<double>& levels = w.levels;

        std::vector<char> outsideArea(cells);
        for (size_t q = 0; q < cells; ++q)
            outsideArea[q] = w.inArea[q] ? 0 : 1;
        bool anyArea = false;
        const std::vector<float> fromAreaEdge = DistanceFrom(outsideArea, nx, ny, gridCell, anyArea);

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

            // Where the tool can't come down to this level, how far each cell is from
            // there, and so where its centre may go.
            std::vector<char> blocked(cells);
            for (size_t q = 0; q < cells; ++q)
                blocked[q] = w.Blocked(q, z) ? 1 : 0;
            bool anyObstacle = false;
            const std::vector<float> fromPart = DistanceFrom(blocked, nx, ny, gridCell, anyObstacle);

            std::vector<char> free(cells, 0);
            std::vector<char> notFree(cells, 1);
            bool anyFree = false;
            for (size_t q = 0; q < cells; ++q)
            {
                free[q] = w.inArea[q] && !blocked[q];
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

            // Pieces from a field's contour, kept to where the tool may go. Contours
            // that cross the edge of that are clipped to it, so they start a cell in
            // from it, where they're clearly inside.
            std::vector<Piece> pieces;
            const auto addContours = [&](const std::vector<float>& field, double level, bool materialHigher,
                                         bool alternate, bool& flip, bool clip) {
                for (Polyline loop : IsoContours(field, nx, ny, w.xLow, w.yLow, gridCell, level, z))
                {
                    // Climb keeps the material on the right; the contours have the higher side on the left.
                    bool reverse = materialHigher;
                    if (settings.direction == CutDirection::Conventional)
                        reverse = !reverse;
                    if (settings.direction == CutDirection::Mixed && alternate)
                        reverse = flip;
                    if (reverse)
                        std::reverse(loop.begin(), loop.end());
                    for (Piece& piece : ClipContour(loop, w, free, clip))
                        pieces.push_back(std::move(piece));
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
                for (double t = gridCell; t <= deepest; t += pitch)
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
                        addContours(fromPart, gridCell, true, false, flip, true);
                }
                for (double t : ts)
                    addContours(fromAreaEdge, t, !inward, true, flip, true);
                if (!inward && anyObstacle)
                    addContours(fromPart, gridCell, true, false, flip, true);
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
                        const double x = w.xLow + (i + 0.5) * gridCell, y = w.yLow + (j + 0.5) * gridCell;
                        const double v = x * across.x + y * across.y;
                        const double u = x * along.x + y * along.y;
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
                const auto inside = [&](const Vec3& p) {
                    size_t q = 0;
                    return w.CellAt(p, q) && free[q] != 0;
                };
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
                if (!piece.points.empty() && w.CellAt(piece.points.front(), q) && labels[q] >= 0)
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

        const FeedSettings& feeds = input.operation->feeds;
        ToolpathResult result;
        MoveWriter writer(result.toolpath, w.clearance);
        for (int r : order)
        {
            Region& region = regions[static_cast<size_t>(r)];
            if (region.pieces.empty())
                continue;
            const double z = levels[region.level];
            const double above = (region.parent >= 0) ? levels[regions[static_cast<size_t>(region.parent)].level]
                                                      : (region.level > 0 ? levels[region.level - 1] : w.top);
            const double enter = (std::min)(above + 1.0, w.clearance);
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
                    if (!w.CellAt({ from.x + (to.x - from.x) * f, from.y + (to.y - from.y) * f, z }, q) ||
                        field[q] < t0)
                        return false;
                }
                return true;
            };

            writer.Lift();
            bool down = false;
            for (Piece& piece : region.pieces)
            {
                Polyline& points = piece.points;
                if (points.empty())
                    continue;
                if (piece.closed)
                    StartNearest(points, writer.At());
                else if (!piece.fixedDirection &&
                         std::hypot(points.back().x - writer.At().x, points.back().y - writer.At().y) <
                             std::hypot(points.front().x - writer.At().x, points.front().y - writer.At().y))
                    std::reverse(points.begin(), points.end());

                const Vec3 start = points.front();
                if (down && canStayDown(writer.At(), start))
                {
                    writer.Add(MoveKind::Feed, start, feeds.cut);
                }
                else
                {
                    writer.Enter(start, enter, feeds.plunge);
                    down = true;
                }
                for (size_t p = 1; p < points.size(); ++p)
                    writer.Add(MoveKind::Feed, points[p], feeds.cut);
                if (piece.closed)
                    writer.Add(MoveKind::Feed, start, feeds.cut);
                if (writer.Count() > kMostMoves)
                    return Failure("The toolpath would be too long. Make the stepdown or stepover larger.");
            }
            writer.Lift();
        }
        if (!writer.Started())
            return Failure("There's nothing to cut: the tool doesn't fit anywhere in the area.");

        result.ok = true;
        return result;
    }

    ToolpathResult GenerateWaterlineFinishToolpath(const WaterlineRoughToolpathInput& input)
    {
        WaterlineSetup w;
        std::string problem;
        if (!PrepareWaterline(input, StepType::WaterlineFinishing, w, problem))
            return Failure(problem);
        const WaterlineRoughingSettings& settings = *w.pSettings;
        const int nx = w.nx, ny = w.ny;
        const size_t cells = w.cells;

        // The laps are where the tip's lowest height crosses each level: the
        // contour of its negative, so the free side (higher) is on the left and
        // the part on the right, which is climb.
        std::vector<float> lowness(cells);
        for (size_t q = 0; q < cells; ++q)
            lowness[q] = (w.tips[q] == std::numeric_limits<float>::lowest()) ? 1e9f : -w.tips[q];
        const auto laps = [&](double z) {
            std::vector<Piece> pieces;
            for (Polyline& loop : IsoContours(lowness, nx, ny, w.xLow, w.yLow, w.cell, -(z + kHeightTolerance), z))
            {
                for (Piece& piece : ClipContour(loop, w, w.inArea, true))
                    pieces.push_back(std::move(piece));
            }
            return pieces;
        };

        // More levels where the laps of two levels would be further apart across
        // than the stepover, on shallow slopes.
        std::vector<double> levels;
        double previous = w.top;
        bool havePrevious = false;
        for (double z : w.levels)
        {
            if (havePrevious)
            {
                std::vector<char> blocked(cells);
                for (size_t q = 0; q < cells; ++q)
                    blocked[q] = w.Blocked(q, previous) ? 1 : 0;
                bool any = false;
                const std::vector<float> fromAbove = DistanceFrom(blocked, nx, ny, w.cell, any);
                double gap = 0.0;
                if (any)
                {
                    for (const Piece& piece : laps(z))
                    {
                        for (const Vec3& p : piece.points)
                        {
                            size_t q = 0;
                            if (w.CellAt(p, q))
                                gap = (std::max)(gap, static_cast<double>(fromAbove[q]));
                        }
                    }
                }
                const int extra = (std::min)(200, (std::max)(0, static_cast<int>(std::ceil(gap / w.pitch - 1e-9)) - 1));
                const int pieces = (std::max)(1, (std::min)(extra + 1, static_cast<int>((previous - z) / 0.01)));
                for (int k = 1; k < pieces; ++k)
                    levels.push_back(previous - (previous - z) * k / pieces);
            }
            levels.push_back(z);
            previous = z;
            havePrevious = true;
        }

        // Each lap, with the part it goes round: the separate parts at the bottom
        // level, for Depth First.
        std::vector<char> blockedAtBottom(cells);
        for (size_t q = 0; q < cells; ++q)
            blockedAtBottom[q] = w.Blocked(q, levels.empty() ? w.bottom : levels.back()) ? 1 : 0;
        std::vector<int> islands;
        LabelComponents(blockedAtBottom, nx, ny, islands);
        const auto islandOf = [&](const Polyline& points) {
            if (points.size() < 2)
                return 0;
            // A few cells to the right of the start, where the part is.
            const Vec3 d = points[1] - points[0];
            const double length = std::hypot(d.x, d.y);
            if (length < 1e-12)
                return 0;
            const Vec3 right{ d.y / length, -d.x / length, 0.0 };
            for (int k = 1; k <= 4; ++k)
            {
                size_t q = 0;
                if (w.CellAt(points[0] + right * (k * w.cell), q) && islands[q] >= 0)
                    return islands[q];
            }
            return 0;
        };

        // Finish Flats: the levels that are a flat face's height plus the stock to leave.
        std::vector<double> flats;
        if (settings.finishFlats)
        {
            for (double h : FlatHeights(input.partTriangles))
            {
                const double z = h + w.s;
                if (z <= w.top + kHeightTolerance && z >= w.bottom - kHeightTolerance)
                    flats.push_back(z);
            }
        }
        const auto isFlat = [&](double z) {
            for (double f : flats)
            {
                if (std::fabs(f - z) < 1e-4)
                    return true;
            }
            return false;
        };
        // The part a flat pass is over: under its start, or a few cells either side.
        const auto islandNear = [&](const Polyline& points) {
            if (points.empty())
                return 0;
            Vec3 side{ 0.0, 0.0, 0.0 };
            if (points.size() > 1)
            {
                const Vec3 d = points[1] - points[0];
                const double length = std::hypot(d.x, d.y);
                if (length > 1e-12)
                    side = Vec3{ d.y / length, -d.x / length, 0.0 };
            }
            for (int k = 0; k <= 4; ++k)
            {
                for (double sign : { 1.0, -1.0 })
                {
                    size_t q = 0;
                    if (w.CellAt(points[0] + side * (sign * k * w.cell), q) && islands[q] >= 0)
                        return islands[q];
                }
            }
            return 0;
        };
        // Loops over a flat at z, outside in a stepover apart, where the tool's
        // tip rests on the flat. Climb keeps the uncut middle on the right.
        const auto flatPasses = [&](double z) {
            std::vector<Piece> pieces;
            std::vector<char> flat(cells, 0), notFlat(cells, 1);
            bool anyFlat = false;
            for (size_t q = 0; q < cells; ++q)
            {
                flat[q] = w.inArea[q] && !w.Blocked(q, z) && w.tips[q] >= z - kHeightTolerance;
                notFlat[q] = flat[q] ? 0 : 1;
                anyFlat = anyFlat || flat[q];
            }
            if (!anyFlat)
                return pieces;
            bool any = false;
            const std::vector<float> fromEdge = DistanceFrom(notFlat, nx, ny, w.cell, any);
            float deepest = 0.0f;
            for (size_t q = 0; q < cells; ++q)
            {
                if (flat[q])
                    deepest = (std::max)(deepest, fromEdge[q]);
            }
            bool alternate = false;
            for (double t = w.cell; t < deepest; t += w.pitch)
            {
                for (Polyline& loop : IsoContours(fromEdge, nx, ny, w.xLow, w.yLow, w.cell, t, z))
                {
                    bool reverse = settings.direction != CutDirection::Conventional;
                    if (settings.direction == CutDirection::Mixed)
                        reverse = alternate;
                    if (reverse)
                        std::reverse(loop.begin(), loop.end());
                    for (Piece& piece : ClipContour(loop, w, flat, true))
                        pieces.push_back(std::move(piece));
                }
                alternate = !alternate;
            }
            return pieces;
        };

        struct Lap
        {
            size_t level;
            int island;
            Piece piece;
        };
        std::vector<Lap> all;
        bool flip = false;
        for (size_t li = 0; li < levels.size(); ++li)
        {
            for (Piece& piece : laps(levels[li]))
            {
                bool reverse = settings.direction == CutDirection::Conventional;
                if (settings.direction == CutDirection::Mixed)
                    reverse = flip;
                if (reverse)
                    std::reverse(piece.points.begin(), piece.points.end());
                const int island = islandOf(reverse ? Polyline(piece.points.rbegin(), piece.points.rend()) : piece.points);
                all.push_back({ li, island, std::move(piece) });
            }
            flip = !flip;
            // The flat's own passes, after the laps round its walls.
            if (isFlat(levels[li]))
            {
                for (Piece& piece : flatPasses(levels[li]))
                {
                    const int island = islandNear(piece.points);
                    all.push_back({ li, island, std::move(piece) });
                }
            }
        }
        if (settings.order == CutLevelOrder::DepthFirst)
        {
            std::stable_sort(all.begin(), all.end(), [](const Lap& a, const Lap& b) {
                return a.island < b.island || (a.island == b.island && a.level < b.level);
            });
        }

        const FeedSettings& feeds = input.operation->feeds;
        ToolpathResult result;
        MoveWriter writer(result.toolpath, w.clearance);
        bool down = false;
        int lastIsland = -1;
        for (Lap& lap : all)
        {
            const double z = levels[lap.level];
            Polyline& points = lap.piece.points;
            if (points.empty())
                continue;
            if (lap.piece.closed)
                StartNearest(points, writer.At());
            const Vec3 start = points.front();

            // Straight down from the last lap if it's close and the tool fits all the way.
            bool stay = down && lap.island == lastIsland;
            if (stay)
            {
                const Vec3 from = writer.At();
                const double length = std::hypot(start.x - from.x, start.y - from.y);
                stay = length <= 3.0 * w.pitch;
                const int samples = (std::max)(1, static_cast<int>(std::ceil(length / (w.cell / 2.0))));
                for (int k = 0; stay && k <= samples; ++k)
                {
                    const double f = static_cast<double>(k) / samples;
                    const Vec3 p{ from.x + (start.x - from.x) * f, from.y + (start.y - from.y) * f,
                                  from.z + (start.z - from.z) * f };
                    size_t q = 0;
                    stay = w.CellAt(p, q) && !w.Blocked(q, (std::min)(p.z, from.z));
                }
            }
            if (stay)
            {
                writer.Add(MoveKind::Feed, start, start.z < writer.At().z ? feeds.plunge : feeds.cut);
            }
            else
            {
                const double above = (lap.level > 0) ? levels[lap.level - 1] : w.top;
                writer.Enter(start, (std::min)(above + 1.0, w.clearance), feeds.plunge);
            }
            down = true;
            lastIsland = lap.island;
            for (size_t p = 1; p < points.size(); ++p)
                writer.Add(MoveKind::Feed, points[p], feeds.cut);
            if (lap.piece.closed)
                writer.Add(MoveKind::Feed, start, feeds.cut);
            if (writer.Count() > kMostMoves)
                return Failure("The toolpath would be too long. Make the stepdown or stepover larger.");
            (void)z;
        }
        writer.Lift();
        if (!writer.Started())
            return Failure("There's nothing to finish: no part comes within reach in the area between the top and "
                           "the bottom.");

        result.ok = true;
        return result;
    }

    namespace
    {
        // The distance from p to the segment a-b, in 3D.
        double SegmentDistance3(const Vec3& p, const Vec3& a, const Vec3& b)
        {
            const Vec3 e = b - a;
            const double lengthSquared = e.x * e.x + e.y * e.y + e.z * e.z;
            double t = 0.0;
            if (lengthSquared > 1e-18)
                t = (std::min)(1.0, (std::max)(0.0, ((p.x - a.x) * e.x + (p.y - a.y) * e.y + (p.z - a.z) * e.z) /
                                                        lengthSquared));
            const Vec3 d = p - (a + e * t);
            return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        }

        void Simplify3(const Polyline& points, size_t first, size_t last, double tolerance, std::vector<char>& keep)
        {
            if (last <= first + 1)
                return;
            double worst = -1.0;
            size_t at = first;
            for (size_t i = first + 1; i < last; ++i)
            {
                const double d = SegmentDistance3(points[i], points[first], points[last]);
                if (d > worst)
                {
                    worst = d;
                    at = i;
                }
            }
            if (worst > tolerance)
            {
                keep[at] = 1;
                Simplify3(points, first, at, tolerance, keep);
                Simplify3(points, at, last, tolerance, keep);
            }
        }

        // An open run with fewer points, none further than tolerance in 3D from the line kept.
        Polyline SimplifyRun3(const Polyline& run, double tolerance)
        {
            if (run.size() < 3)
                return run;
            std::vector<char> keep(run.size(), 0);
            keep.front() = keep.back() = 1;
            Simplify3(run, 0, run.size() - 1, tolerance, keep);
            Polyline result;
            for (size_t i = 0; i < run.size(); ++i)
            {
                if (keep[i])
                    result.push_back(run[i]);
            }
            return result;
        }
    }

    ToolpathResult GenerateParallelFinishToolpath(const WaterlineRoughToolpathInput& input)
    {
        WaterlineSetup w;
        std::string problem;
        if (!PrepareWaterline(input, StepType::ParallelFinishing, w, problem))
            return Failure(problem);
        const WaterlineRoughingSettings& settings = *w.pSettings;
        if (settings.stepoverMode == WaterlineStepoverMode::Scallop && input.tool->type != ToolType::BallMill)
            return Failure("A stepover by scallop height needs a ball nose end mill. Choose another stepover on the "
                           "Parallel Finishing tab, or a ball mill.");
        const int nx = w.nx, ny = w.ny;
        const size_t cells = w.cells;
        const double floorZ = w.bottom - 1.0;   // below anything a level reaches

        // How low the tip may go at p: the tool dropped onto the parts' triangles,
        // exactly for its shape (not the grid, which would make the rows step).
        const DropCutter cutter(input.partTriangles, ProfileOf(*input.tool), w.s, input.partTriangleFaces);
        const auto surfaceAt = [&](double x, double y) {
            double tip = 0.0;
            return cutter.TipAt(x, y, tip) ? (std::max)(tip, floorZ) : floorZ;
        };

        // A ball's contact moved onto the face's true surface: its centre straight
        // up or down until it's the ball's radius plus the stock to leave from the
        // surface, by Newton's method. Kept only if it settles on the face near the
        // triangles' answer, and never below another face's.
        const bool refine = input.tool->type == ToolType::BallMill && input.closestPoint &&
                            !input.partTriangleFaces.empty();
        const double ballRadius = input.tool->diameter / 2.0;
        size_t refined = 0, refineTries = 0, refineCalls = 0;
        size_t noAnswer = 0, onSurface = 0, sideContact = 0, unsettled = 0, offFace = 0, tooFar = 0, shared = 0;
        // Refines one contact, starting with the centre moved by guess from the
        // triangles' answer (a neighbour's correction, as it changes slowly). Each
        // step squares the error, so once the next would be under 0.01 microns
        // it stops without asking again. False, keeping the triangles' answer, if
        // it can't be done.
        const auto refineOnce = [&](double x, double y, const DropCutter::Contact& contact, double guess,
                                    double& tip) {
            if (contact.face < 0)
                return false;
            ++refineTries;
            const double gap = ballRadius + w.s;
            double centreZ = contact.tip + ballRadius + guess;
            bool settled = false;
            Vec3 closest;
            bool onFace = false;
            for (int k = 0; k < 8 && !settled; ++k)
            {
                const Vec3 centre{ x, y, centreZ };
                ++refineCalls;
                if (!input.closestPoint(contact.face, centre, closest, onFace))
                {
                    ++noAnswer;
                    return false;
                }
                const Vec3 d = centre - closest;
                const double distance = Length(d);
                if (distance < 1e-9)
                {
                    ++onSurface;
                    return false;
                }
                const double nz = d.z / distance;
                const double error = distance - gap;
                if (std::fabs(error) < 1e-5)
                {
                    settled = true;
                    break;
                }
                if (nz < 0.05)
                {
                    ++sideContact;
                    return false;   // a contact on the ball's side: leave it to the triangles
                }
                const double step = error / nz;
                centreZ -= step;
                // How far off the next answer would be, from the distance's curvature.
                const double next = (1.0 - nz * nz) / (2.0 * gap * nz * nz * nz) * step * step;
                settled = next < 1e-5;
            }
            if (!settled || !onFace)
            {
                ++(settled ? offFace : unsettled);
                return false;
            }
            tip = centreZ - ballRadius;
            if (std::fabs(tip - contact.tip) > 0.5)
            {
                ++tooFar;
                return false;
            }
            ++refined;
            return true;
        };
        // The height for a contact, refined when it can be: one thread only.
        const auto refinedHeight = [&](double x, double y, const DropCutter::Contact& contact, double guess) {
            double tip = 0.0;
            if (refineOnce(x, y, contact, guess, tip))
                return (std::max)({ tip, contact.others, floorZ });
            return (std::max)(contact.tip, floorZ);
        };
        const auto exactAt = [&](double x, double y) {
            DropCutter::Contact contact;
            if (!cutter.ContactAt(x, y, contact))
                return floorZ;
            return refine ? refinedHeight(x, y, contact, 0.0) : (std::max)(contact.tip, floorZ);
        };
        const double tolerance = (std::min)(w.cell / 4.0, 0.01);
        const auto inArea = [&](double x, double y) {
            size_t q = 0;
            return w.CellAt({ x, y, 0.0 }, q) && w.inArea[q] != 0;
        };

        // The rows, across the area along the angle of cuts, as Waterline Roughing's Linear.
        const Vec3 along = PassDirection(settings.cutAngle);
        const Vec3 across{ -along.y, along.x, 0.0 };
        double low = 0.0, high = 0.0, uLow = 0.0, uHigh = 0.0;
        bool found = false;
        for (int j = 0; j < ny; ++j)
        {
            for (int i = 0; i < nx; ++i)
            {
                if (!w.inArea[static_cast<size_t>(j) * nx + i])
                    continue;
                const double x = w.xLow + (i + 0.5) * w.cell, y = w.yLow + (j + 0.5) * w.cell;
                const double v = x * across.x + y * across.y;
                const double u = x * along.x + y * along.y;
                low = found ? (std::min)(low, v) : v;
                high = found ? (std::max)(high, v) : v;
                uLow = found ? (std::min)(uLow, u) : u;
                uHigh = found ? (std::max)(uHigh, u) : u;
                found = true;
            }
        }
        if (!found)
            return Failure("The picked geometry covers no area seen from above.");
        const int intervals = (std::max)(1, static_cast<int>(std::ceil((high - low) / w.pitch - 1e-9)));
        if (intervals > 200000)
            return Failure("The toolpath would be too long. Make the stepover larger.");
        const double spacing = (high - low) / intervals;
        const bool acrossGoesUp = std::fabs(across.y) > 1e-9 ? across.y > 0.0 : across.x < 0.0;
        const bool fromLow = (settings.linearStart == PocketLinearStart::Bottom) == acrossGoesUp;
        const double stepSign = fromLow ? 1.0 : -1.0;
        // As Pocketing: the uncut side is the way the rows step, and climb keeps it on the right.
        const bool climbForward = stepSign < 0.0;
        const double sample = w.cell / 2.0;

        // Each row's runs inside the area, in XY, with their surface heights, cut
        // the row's own way.
        struct Run
        {
            Polyline points;            // z is the surface's height
            int component = 0;
        };
        std::vector<int> components;
        LabelComponents(w.inArea, nx, ny, components);
        std::vector<std::vector<Run>> rows;
        bool forward = (settings.direction == CutDirection::Conventional) ? !climbForward : climbForward;
        for (int r = 0; r <= intervals; ++r)
        {
            const double v = fromLow ? low + r * spacing : high - r * spacing;
            std::vector<Run> runs;
            Run run;
            const auto finish = [&]() {
                if (run.points.size() >= 2)
                {
                    size_t q = 0;
                    run.component = (w.CellAt(run.points.front(), q) && components[q] >= 0) ? components[q] : 0;
                    runs.push_back(std::move(run));
                }
                run = Run();
            };
            for (double u = uLow - w.cell; u <= uHigh + w.cell + 1e-9; u += sample)
            {
                const Vec3 p = along * u + across * v;
                if (inArea(p.x, p.y))
                    run.points.push_back({ p.x, p.y, 0.0 });
                else
                    finish();
            }
            finish();
            if (!forward)
            {
                std::reverse(runs.begin(), runs.end());
                for (Run& each : runs)
                    std::reverse(each.points.begin(), each.points.end());
            }
            rows.push_back(std::move(runs));
            if (settings.direction == CutDirection::Mixed)
                forward = !forward;
        }
        // The surface under every point, on all cores.
        std::vector<Vec3*> samples;
        std::vector<std::pair<size_t, size_t>> runSpans;   // each run's samples
        for (std::vector<Run>& runs : rows)
        {
            for (Run& run : runs)
            {
                const size_t first = samples.size();
                for (Vec3& p : run.points)
                    samples.push_back(&p);
                runSpans.push_back({ first, samples.size() });
            }
        }
        if (refine)
        {
            // The triangles' contacts on all cores, then each refined on one thread.
            std::vector<DropCutter::Contact> contacts(samples.size());
            std::vector<char> found(samples.size(), 0);
            std::vector<size_t> order(samples.size());
            for (size_t k = 0; k < order.size(); ++k)
                order[k] = k;
            std::for_each(std::execution::par, order.begin(), order.end(), [&](size_t k) {
                found[k] = cutter.ContactAt(samples[k]->x, samples[k]->y, contacts[k]) ? 1 : 0;
            });
            // Along each run, every second point (and the last) is refined, starting
            // from the one before's correction. The point between two refined on
            // the same face shares their correction (the true surface less the
            // triangles', which changes smoothly); others are refined themselves.
            std::vector<char> done(samples.size(), 0);
            std::vector<double> correction(samples.size(), 0.0);
            const auto refinePoint = [&](size_t k, double guess) {
                Vec3& p = *samples[k];
                if (!found[k])
                {
                    p.z = floorZ;
                    return;
                }
                double tip = 0.0;
                if (refineOnce(p.x, p.y, contacts[k], guess, tip))
                {
                    done[k] = 1;
                    correction[k] = tip - contacts[k].tip;
                    p.z = (std::max)({ tip, contacts[k].others, floorZ });
                }
                else
                {
                    p.z = (std::max)(contacts[k].tip, floorZ);
                }
            };
            for (const auto& [first, last] : runSpans)
            {
                double guess = 0.0;
                int guessFace = -1;
                for (size_t k = first; k < last; ++k)
                {
                    if ((k - first) % 2 == 1 && k + 1 != last)
                        continue;
                    const int face = found[k] ? contacts[k].face : -1;
                    refinePoint(k, face == guessFace ? guess : 0.0);
                    guess = correction[k];
                    guessFace = done[k] ? face : -1;
                }
                for (size_t k = first + 1; k + 1 < last; k += 2)
                {
                    // Within one face the true heights are smooth, so the middle of two
                    // refined ones is off by only the curvature times the spacing
                    // squared over 8; the triangles' answer has kinks, so it isn't used,
                    // except as a check that nothing odd lies between.
                    const int face = found[k] ? contacts[k].face : -1;
                    bool sharedHere = false;
                    if (face >= 0 && done[k - 1] && done[k + 1] && contacts[k - 1].face == face &&
                        contacts[k + 1].face == face)
                    {
                        const double before = contacts[k - 1].tip + correction[k - 1];
                        const double after = contacts[k + 1].tip + correction[k + 1];
                        const double tip = 0.5 * (before + after);
                        const double check = contacts[k].tip + 0.5 * (correction[k - 1] + correction[k + 1]);
                        if (std::fabs(tip - check) < 0.05)
                        {
                            samples[k]->z = (std::max)({ tip, contacts[k].others, floorZ });
                            ++shared;
                            sharedHere = true;
                        }
                    }
                    if (!sharedHere)
                    {
                        refinePoint(k, done[k - 1] && contacts[k - 1].face == face ? correction[k - 1] : 0.0);
                    }
                }
            }
        }
        else
        {
            std::for_each(std::execution::par, samples.begin(), samples.end(),
                          [&](Vec3* p) { p->z = surfaceAt(p->x, p->y); });
        }

        // Each level's passes: the surface, but no lower than the level, trimmed
        // to where the level above left something.
        struct Pass
        {
            size_t level;
            int component;
            Polyline points;
        };
        std::vector<Pass> passes;
        const std::vector<double>& levels = w.levels;
        for (size_t li = 0; li < levels.size(); ++li)
        {
            const double z = levels[li];
            const double above = (li > 0) ? levels[li - 1] : w.top;
            for (const std::vector<Run>& runs : rows)
            {
                for (const Run& run : runs)
                {
                    size_t first = run.points.size(), last = 0;
                    for (size_t k = 0; k < run.points.size(); ++k)
                    {
                        if (run.points[k].z < above - kHeightTolerance)
                        {
                            first = (std::min)(first, k);
                            last = k;
                        }
                    }
                    if (first > last)
                        continue;
                    Polyline points;
                    for (size_t k = first; k <= last; ++k)
                        points.push_back({ run.points[k].x, run.points[k].y, (std::max)(run.points[k].z, z) });
                    if (points.size() < 2)
                        continue;
                    passes.push_back({ li, run.component, SimplifyRun3(points, tolerance) });
                    if (passes.size() > kMostMoves)
                        return Failure("The toolpath would be too long. Make the stepdown or stepover larger.");
                }
            }
        }
        if (settings.order == CutLevelOrder::DepthFirst)
        {
            std::stable_sort(passes.begin(), passes.end(), [](const Pass& a, const Pass& b) {
                return a.component < b.component || (a.component == b.component && a.level < b.level);
            });
        }
        (void)cells;

        const FeedSettings& feeds = input.operation->feeds;
        ToolpathResult result;
        MoveWriter writer(result.toolpath, w.clearance);
        size_t lastLevel = 0;
        bool down = false;
        for (const Pass& pass : passes)
        {
            const Vec3 start = pass.points.front();
            // Mixed steps over from row to row staying down, following the surface,
            // when the step is short, inside the area, and at the same level.
            Polyline link;
            bool stay = settings.direction == CutDirection::Mixed && down && pass.level == lastLevel;
            if (stay)
            {
                const Vec3 from = writer.At();
                const double length = std::hypot(start.x - from.x, start.y - from.y);
                stay = length <= 3.0 * w.pitch;
                const int samples = (std::max)(1, static_cast<int>(std::ceil(length / sample)));
                for (int k = 1; stay && k < samples; ++k)
                {
                    const double f = static_cast<double>(k) / samples;
                    const double x = from.x + (start.x - from.x) * f, y = from.y + (start.y - from.y) * f;
                    stay = inArea(x, y);
                    link.push_back({ x, y, (std::max)(refine ? exactAt(x, y) : surfaceAt(x, y), levels[pass.level]) });
                }
            }
            if (stay)
            {
                for (const Vec3& p : SimplifyRun3(link, tolerance))
                    writer.Add(MoveKind::Feed, p, feeds.cut);
                writer.Add(MoveKind::Feed, start, feeds.cut);
            }
            else
            {
                const double above = (pass.level > 0) ? levels[pass.level - 1] : w.top;
                writer.Enter(start, (std::min)((std::max)(above, start.z) + 1.0, w.clearance), feeds.plunge);
            }
            down = true;
            lastLevel = pass.level;
            for (size_t p = 1; p < pass.points.size(); ++p)
                writer.Add(MoveKind::Feed, pass.points[p], feeds.cut);
            if (writer.Count() > kMostMoves)
                return Failure("The toolpath would be too long. Make the stepdown or stepover larger.");
        }
        writer.Lift();
        if (result.toolpath.moves.empty())
            return Failure("There's nothing to cut in the area between the top and the bottom.");
        result.ok = true;
        result.refinedPoints = refined;
        result.refineTries = refineTries;
        result.refineCalls = refineCalls;
        result.refineNoAnswer = noAnswer;
        result.refineOnSurface = onSurface;
        result.refineSideContact = sideContact;
        result.refineUnsettled = unsettled;
        result.refineOffFace = offFace;
        result.refineTooFar = tooFar;
        result.refineShared = shared;
        return result;
    }
}
