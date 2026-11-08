// WaterlineRoughToolpath.cpp : calculates a Waterline Roughing operation's toolpath.

#include "WaterlineRoughToolpath.h"

#include "AdaptiveClearing.h"

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
            std::vector<char> inPickedArea; // the area itself, before the linear extension grows it
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
            const double extension = settings.extension;    // Waterline Finishing's and Parallel Finishing's overtravel
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
            w.inPickedArea = w.inArea;
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

            // Where the tool is after moves were added to the toolpath some other way.
            void SyncTo(const Vec3& at)
            {
                m_at = at;
                m_started = true;
            }
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

        // A contour split where it leaves the cells `keep` allows, each piece simplified
        // (unless simplify is false).
        std::vector<Piece> ClipContour(Polyline loop, const WaterlineSetup& w, const std::vector<char>& keep, bool clip,
                                       bool simplify = true)
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
                piece.points = simplify ? SimplifyLoop(loop, w.cell / 4.0) : loop;
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
                    piece.points = simplify ? SimplifyRun(run, w.cell / 4.0) : run;
                    piece.closed = false;
                    pieces.push_back(std::move(piece));
                }
                run.clear();
            }
            return pieces;
        }
    }

    size_t RemoveThinFingers(Polyline& lap, bool closed,
                             const std::function<bool(const Vec3& from, const Vec3& to)>& shortcutClear)
    {
        const double kWidth = 0.2, kShortest = 0.4, kLongest = 3.0;
        size_t removed = 0;
        for (int pass = 0; pass < 1000; ++pass)
        {
            const size_t n = lap.size();
            if (n < 5)
                break;
            bool cut = false;
            const size_t starts = closed ? n : n - 2;
            for (size_t i = 0; i < starts && !cut; ++i)
            {
                // The points j coming back near i, furthest along first.
                std::vector<size_t> near;
                double path = 0.0;
                for (size_t step = 1; step + 1 < n; ++step)
                {
                    const size_t j = closed ? (i + step) % n : i + step;
                    if (!closed && j >= n)
                        break;
                    const size_t before = closed ? (j + n - 1) % n : j - 1;
                    path += std::hypot(lap[j].x - lap[before].x, lap[j].y - lap[before].y);
                    if (path > kLongest)
                        break;
                    if (path >= kShortest && std::hypot(lap[j].x - lap[i].x, lap[j].y - lap[i].y) <= kWidth)
                        near.push_back(step);
                }
                for (auto it = near.rbegin(); it != near.rend() && !cut; ++it)
                {
                    const size_t step = *it;
                    const size_t j = closed ? (i + step) % n : i + step;
                    // Thin: what it goes round, closed by the shortcut, is under the
                    // width on average (twice its area over its length).
                    double area = 0.0, length = 0.0;
                    for (size_t k = 0; k <= step; ++k)
                    {
                        const Vec3& a = lap[(i + k) % n];
                        const Vec3& b = lap[(i + (k + 1) % (step + 1)) % n];
                        area += a.x * b.y - b.x * a.y;
                        length += std::hypot(b.x - a.x, b.y - a.y);
                    }
                    if (length <= 0.0 || std::fabs(area) / length > kWidth)
                        continue;
                    // At the finger's base: just outside the cut the lap spreads apart
                    // again (otherwise this is partway up a longer finger, which stays).
                    const auto along = [&](size_t from, bool forward, double distance) {
                        size_t k = from;
                        double gone = 0.0;
                        while (gone < distance)
                        {
                            size_t next;
                            if (forward)
                            {
                                if (!closed && k + 1 >= n)
                                    break;
                                next = (k + 1) % n;
                            }
                            else
                            {
                                if (!closed && k == 0)
                                    break;
                                next = (k + n - 1) % n;
                            }
                            gone += std::hypot(lap[next].x - lap[k].x, lap[next].y - lap[k].y);
                            k = next;
                        }
                        return lap[k];
                    };
                    const Vec3 outsideI = along(i, false, 0.3), outsideJ = along(j, true, 0.3);
                    if (std::hypot(outsideI.x - outsideJ.x, outsideI.y - outsideJ.y) <= 2.0 * kWidth)
                        continue;
                    if (!shortcutClear(lap[i], lap[j]))
                        continue;
                    // Drop the points between i and j.
                    Polyline kept;
                    if (!closed || j > i)
                    {
                        kept.assign(lap.begin(), lap.begin() + static_cast<std::ptrdiff_t>(i) + 1);
                        kept.insert(kept.end(), lap.begin() + static_cast<std::ptrdiff_t>(j), lap.end());
                    }
                    else
                    {
                        kept.assign(lap.begin() + static_cast<std::ptrdiff_t>(j),
                                    lap.begin() + static_cast<std::ptrdiff_t>(i) + 1);
                    }
                    lap.swap(kept);
                    ++removed;
                    cut = true;
                }
            }
            if (!cut)
                break;
        }

        // Then tiny back steps: walking along the lap, a point that goes backwards
        // against the lap's own direction (over 0.3 mm either side) is left out, if
        // the tool keeps clear going on from the last point kept.
        const size_t n = lap.size();
        if (n >= 5)
        {
            std::vector<double> at(n, 0.0);     // path length to each point
            for (size_t k = 1; k < n; ++k)
                at[k] = at[k - 1] + std::hypot(lap[k].x - lap[k - 1].x, lap[k].y - lap[k - 1].y);
            Polyline kept;
            kept.push_back(lap[0]);
            size_t dropped = 0;
            for (size_t k = 1; k < n; ++k)
            {
                // The lap's direction here, from points 0.3 mm of path either side.
                size_t a = k, b = k;
                while (a > 0 && at[k] - at[a] < 0.3)
                    --a;
                while (b + 1 < n && at[b] - at[k] < 0.3)
                    ++b;
                const Vec3 direction{ lap[b].x - lap[a].x, lap[b].y - lap[a].y, 0.0 };
                const Vec3 step{ lap[k].x - kept.back().x, lap[k].y - kept.back().y, 0.0 };
                const bool last = (k + 1 == n);
                if (!last && Length(direction) > 1e-9 && Dot(step, direction) <= 0.0 && k + 1 < n &&
                    shortcutClear(kept.back(), lap[k + 1]))
                {
                    ++dropped;
                    continue;
                }
                kept.push_back(lap[k]);
            }
            if (dropped > 0)
            {
                lap.swap(kept);
                ++removed;
            }
        }
        return removed;
    }

    ToolpathResult GenerateWaterlineRoughToolpath(const WaterlineRoughToolpathInput& input)
    {
        if (input.progress)
            input.progress(0.0, "Working out the area and the tool's reach");
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
            std::vector<AdaptiveCut> adaptive;  // in place of the pieces, with Use Adaptive Clearing
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
            if (input.progress)
                input.progress(static_cast<double>(li) / levels.size(),
                               "Level " + std::to_string(li + 1) + " of " + std::to_string(levels.size()));
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

            // Adaptive clearing: each separate region on its own, its material the
            // picked area (the stock between this level and the one above), starting
            // from any air the linear extension reaches past it.
            if (settings.adaptive)
            {
                for (int c = 0; c < count; ++c)
                {
                    AdaptiveGrid grid;
                    grid.xLow = w.xLow + 0.5 * gridCell;
                    grid.yLow = w.yLow + 0.5 * gridCell;
                    grid.cell = gridCell;
                    grid.nx = nx;
                    grid.ny = ny;
                    grid.free.assign(cells, 0);
                    grid.material.assign(cells, 0);
                    for (size_t q = 0; q < cells; ++q)
                    {
                        grid.free[q] = (labels[q] == c) ? 1 : 0;
                        grid.material[q] = w.inPickedArea[q] ? 1 : 0;
                    }
                    AdaptiveSettings adaptive;
                    adaptive.toolRadius = w.radius;
                    adaptive.optimalLoad = pitch;
                    adaptive.climb = settings.direction != CutDirection::Conventional;
                    adaptive.ramp = settings.rampEntry;
                    adaptive.simplify = input.simplifyTolerance;
                    regions[static_cast<size_t>(base + c)].adaptive = AdaptiveClear(std::move(grid), adaptive).cuts;
                }
                previousLabels = labels;
                previousBase = base;
                continue;
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
            if (region.pieces.empty() && region.adaptive.empty())
                continue;
            const double z = levels[region.level];
            const double above = (region.parent >= 0) ? levels[regions[static_cast<size_t>(region.parent)].level]
                                                      : (region.level > 0 ? levels[region.level - 1] : w.top);
            // At rapid to the approach distance above the level above (the top of the
            // material still to cut here), then at the plunge feed.
            const double enter = (std::min)(above + settings.approachDistance, w.clearance);
            if (settings.adaptive)
            {
                if (region.adaptive.empty())
                    continue;
                writer.Lift();
                AdaptiveWriting writing;
                writing.z = z;
                writing.top = above;
                writing.enter = enter;
                writing.clearance = w.clearance;
                writing.feeds = feeds;
                writing.climb = settings.direction != CutDirection::Conventional;
                writing.rampAngle = settings.rampEntry.rampAngle;
                WriteAdaptiveCuts(region.adaptive, writing, result.toolpath);
                if (!result.toolpath.moves.empty())
                    writer.SyncTo(result.toolpath.moves.back().to);
                continue;
            }
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

            // Into the material between the level above and this one: down a helix
            // round the start where there's room, else a ramp along the piece itself,
            // else straight down.
            const RampEntry& ramp = settings.rampEntry;
            const bool climb = settings.direction != CutDirection::Conventional;
            const double slope = std::tan((std::max)(0.1, ramp.rampAngle) * 3.14159265358979323846 / 180.0);
            const auto enterPiece = [&](const Piece& piece, const Vec3& start) {
                const double depth = above - z;
                const auto comeDownAbove = [&](const Vec3& p) {
                    writer.Lift();
                    writer.Add(MoveKind::Rapid, { p.x, p.y, w.clearance }, 0.0);
                    if (enter < w.clearance - 1e-9 && enter > above + 1e-9)
                        writer.Add(MoveKind::Rapid, { p.x, p.y, enter }, 0.0);
                    writer.Add(MoveKind::Feed, { p.x, p.y, above }, feeds.plunge);
                    writer.SyncTo(writer.At());     // started, as Enter would mark it
                };
                if (depth > 1e-9 && ramp.kind == EntryKind::Helix)
                {
                    // The helix's circle passes through the start, its centre that far
                    // in from it (up the room's slope), where there's room round it;
                    // smaller where there isn't.
                    const auto roomAt = [&](double x, double y) {
                        size_t q = 0;
                        return w.CellAt({ x, y, z }, q) ? static_cast<double>(field[q]) : 0.0;
                    };
                    double gx = roomAt(start.x + gridCell, start.y) - roomAt(start.x - gridCell, start.y);
                    double gy = roomAt(start.x, start.y + gridCell) - roomAt(start.x, start.y - gridCell);
                    const double g = std::hypot(gx, gy);
                    gx = (g > 1e-12) ? gx / g : 1.0;
                    gy = (g > 1e-12) ? gy / g : 0.0;
                    const double smallest = (std::max)(0.1 * w.radius, 2.0 * gridCell);
                    for (double radius = ramp.helixPercent / 100.0 * w.radius; radius >= smallest; radius *= 0.5)
                    {
                        const Vec3 centre{ start.x + gx * radius, start.y + gy * radius, z };
                        if (roomAt(centre.x, centre.y) < radius - 0.5 * gridCell)     // the start is on it
                            continue;
                        const Vec3 first{ start.x, start.y, above };
                        comeDownAbove(first);
                        AppendHelix(result.toolpath, first, centre, z, ramp.rampAngle, feeds.engage, climb);
                        writer.SyncTo(result.toolpath.moves.back().to);
                        ++result.helixEntries;
                        return;
                    }
                }
                if (depth > 1e-9 && ramp.kind != EntryKind::Plunge)
                {
                    // Back along the piece from the start (round a loop, or to and fro
                    // along an open run), as far as the ramp needs; then down it.
                    const std::vector<Vec3>& pts = piece.points;
                    double total = 0.0;
                    for (size_t k = 1; k < pts.size(); ++k)
                        total += std::hypot(pts[k].x - pts[k - 1].x, pts[k].y - pts[k - 1].y);
                    if (piece.closed && pts.size() > 1)
                        total += std::hypot(pts.front().x - pts.back().x, pts.front().y - pts.back().y);
                    const double needed = depth / slope;
                    if (total >= 2.0 * gridCell)
                    {
                        std::vector<Vec3> track{ start };
                        double along = 0.0;
                        const size_t n = pts.size();
                        long long index = 0;
                        int way = 1;        // an open run: forward first, then back
                        while (along < needed - 1e-9 && track.size() < 1000000)
                        {
                            Vec3 next;
                            if (piece.closed)
                            {
                                index = (index - 1 + static_cast<long long>(n)) % static_cast<long long>(n);
                                next = pts[static_cast<size_t>(index)];
                            }
                            else
                            {
                                if (index + way < 0 || index + way >= static_cast<long long>(n))
                                    way = -way;
                                index += way;
                                next = pts[static_cast<size_t>(index)];
                            }
                            const Vec3& last = track.back();
                            const double step = std::hypot(next.x - last.x, next.y - last.y);
                            if (along + step >= needed)
                            {
                                const double f = (needed - along) / step;
                                track.push_back({ last.x + (next.x - last.x) * f, last.y + (next.y - last.y) * f, z });
                                along = needed;
                                break;
                            }
                            track.push_back(next);
                            along += step;
                        }
                        std::reverse(track.begin(), track.end());
                        comeDownAbove(track.front());
                        double done = 0.0;
                        for (size_t k = 1; k < track.size(); ++k)
                        {
                            done += std::hypot(track[k].x - track[k - 1].x, track[k].y - track[k - 1].y);
                            const double height = above - depth * (std::min)(1.0, done / needed);
                            writer.Add(MoveKind::Feed, { track[k].x, track[k].y, height }, feeds.engage);
                        }
                        writer.Add(MoveKind::Feed, start, feeds.engage);    // exactly at the level
                        ++result.rampEntries;
                        return;
                    }
                }
                writer.Enter(start, enter, feeds.plunge);
                ++result.plungeEntries;
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
                    enterPiece(piece, start);
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

    namespace
    {
        // How many points were moved onto the true surface, and why others weren't.
        struct RefineCounts
        {
            size_t refined = 0, tries = 0, calls = 0, noAnswer = 0, onSurface = 0, sideContact = 0, unsettled = 0,
                   offFace = 0, tooFar = 0, shared = 0;
            void Add(const RefineCounts& o)
            {
                refined += o.refined;
                tries += o.tries;
                calls += o.calls;
                noAnswer += o.noAnswer;
                onSurface += o.onSurface;
                sideContact += o.sideContact;
                unsettled += o.unsettled;
                offFace += o.offFace;
                tooFar += o.tooFar;
                shared += o.shared;
            }
            void Into(ToolpathResult& result) const
            {
                result.refinedPoints = refined;
                result.refineTries = tries;
                result.refineCalls = calls;
                result.refineNoAnswer = noAnswer;
                result.refineOnSurface = onSurface;
                result.refineSideContact = sideContact;
                result.refineUnsettled = unsettled;
                result.refineOffFace = offFace;
                result.refineTooFar = tooFar;
                result.refineShared = shared;
            }
        };

        // The lowest the tool's tip may go with its axis through a point: the tool
        // dropped exactly onto the parts' triangles, and for a ball mill, moved onto
        // the face's true surface (its centre straight up or down until it's the
        // ball's radius plus the stock to leave from the surface, by Newton's
        // method; kept only if it settles on the face near the triangles' answer,
        // and never below another face's). Spheres, cylinders, cones and tori are
        // worked out here, on any thread; other faces are asked of Alibre
        // (closestPoint), on the calling thread only.
        class ExactHeights
        {
        public:
            ExactHeights(const WaterlineRoughToolpathInput& input, double stockToLeave, double floorZ)
                : m_input(input)
                , m_cutter(input.partTriangles, ProfileOf(*input.tool), stockToLeave, input.partTriangleFaces)
                , m_s(stockToLeave)
                , m_floor(floorZ)
                , m_ball(input.tool->diameter / 2.0)
            {
                m_refine = input.tool->type == ToolType::BallMill && !input.partTriangleFaces.empty() &&
                           (static_cast<bool>(input.closestPoint) || !input.faceSurfaces.empty());
            }

            const DropCutter& Cutter() const { return m_cutter; }
            bool Refines() const { return m_refine; }
            double Floor() const { return m_floor; }

            bool Analytic(int face) const
            {
                return face >= 0 && static_cast<size_t>(face) < m_input.faceSurfaces.size() &&
                       m_input.faceSurfaces[static_cast<size_t>(face)].kind != SurfaceKind::None;
            }

            // Whether refining this contact means asking Alibre (so the calling thread only).
            bool AsksAlibre(const DropCutter::Contact& contact) const
            {
                return m_refine && contact.face >= 0 && !Analytic(contact.face) && m_input.closestPoint;
            }

            // The triangles' answer only.
            double TriangleHeight(double x, double y) const
            {
                double tip = 0.0;
                return m_cutter.TipAt(x, y, tip) ? (std::max)(tip, m_floor) : m_floor;
            }

            // Refines one contact, starting with the centre moved by guess from the
            // triangles' answer. Each step squares the error, so once the next would
            // be under 0.01 microns it stops without asking again. False, keeping the
            // triangles' answer, if it can't be done.
            bool RefineOnce(double x, double y, const DropCutter::Contact& contact, double guess, double& tip,
                            RefineCounts& n) const
            {
                if (!m_refine || contact.face < 0)
                    return false;
                const bool here = Analytic(contact.face);
                if (!here && !m_input.closestPoint)
                    return false;
                ++n.tries;
                const double gap = m_ball + m_s;
                double centreZ = contact.tip + m_ball + guess;
                bool settled = false;
                Vec3 closest;
                bool onFace = false;
                double normalZ = 1.0;
                for (int k = 0; k < 8 && !settled; ++k)
                {
                    const Vec3 centre{ x, y, centreZ };
                    bool answered = false;
                    if (here)
                    {
                        answered =
                            ClosestPointOn(m_input.faceSurfaces[static_cast<size_t>(contact.face)], centre, closest);
                    }
                    else
                    {
                        ++n.calls;
                        answered = m_input.closestPoint(contact.face, centre, closest, onFace);
                    }
                    if (!answered)
                    {
                        ++n.noAnswer;
                        return false;
                    }
                    const Vec3 d = centre - closest;
                    const double distance = Length(d);
                    if (distance < 1e-9)
                    {
                        ++n.onSurface;
                        return false;
                    }
                    const double nz = d.z / distance;
                    normalZ = nz;
                    const double error = distance - gap;
                    if (std::fabs(error) < 1e-5)
                    {
                        settled = true;
                        break;
                    }
                    if (nz < 0.05)
                    {
                        ++n.sideContact;
                        return false;   // a contact on the ball's side: leave it to the triangles
                    }
                    const double step = error / nz;
                    centreZ -= step;
                    // How far off the next answer would be, from the distance's curvature.
                    const double next = (1.0 - nz * nz) / (2.0 * gap * nz * nz * nz) * step * step;
                    settled = next < 1e-5;
                }
                // A worked out point is on the face if it's near the face's own facets.
                if (settled && here)
                    onFace = m_cutter.NearFace(
                        contact.face, closest, m_input.faceSurfaces[static_cast<size_t>(contact.face)].onFaceTolerance);
                if (!settled || !onFace)
                {
                    ++(settled ? n.offFace : n.unsettled);
                    return false;
                }
                tip = centreZ - m_ball;
                // Measured across the surface (along its normal), as on a steep side a
                // small move across is a big one up or down.
                if (std::fabs(tip - contact.tip) * std::fabs(normalZ) > 0.5)
                {
                    ++n.tooFar;
                    return false;
                }
                ++n.refined;
                return true;
            }

            // The height at (x, y), refined when it can be. False if that needs
            // Alibre and allowAlibre is false (on another thread).
            bool Height(double x, double y, RefineCounts& n, bool allowAlibre, double& height) const
            {
                DropCutter::Contact contact;
                if (!m_cutter.ContactAt(x, y, contact))
                {
                    height = m_floor;
                    return true;
                }
                if (!allowAlibre && AsksAlibre(contact))
                    return false;
                double tip = 0.0;
                if (RefineOnce(x, y, contact, 0.0, tip, n))
                    height = (std::max)({ tip, contact.others, m_floor });
                else
                    height = (std::max)(contact.tip, m_floor);
                return true;
            }

        private:
            const WaterlineRoughToolpathInput& m_input;
            DropCutter m_cutter;
            double m_s;
            double m_floor;
            double m_ball;
            bool m_refine = false;
        };
        // A lap with fewer points: Douglas-Peucker, a straight move from a to b taking
        // the place of the points between if they're all within `sideways` of it, or
        // within `furthest` of it and spanOk(a, b) says so.
        void SimplifySpan(const Polyline& p, size_t first, size_t last, double sideways, double furthest,
                          const std::function<bool(const Vec3&, const Vec3&)>& spanOk, std::vector<char>& keep)
        {
            if (last <= first + 1)
                return;
            double worst = -1.0;
            size_t at = first;
            for (size_t i = first + 1; i < last; ++i)
            {
                const double d = SegmentDistance(p[i], p[first], p[last]);
                if (d > worst)
                {
                    worst = d;
                    at = i;
                }
            }
            if (worst <= sideways || (worst <= furthest && spanOk(p[first], p[last])))
                return;
            if (worst <= 1e-12)
                at = (first + last) / 2;
            keep[at] = 1;
            SimplifySpan(p, first, at, sideways, furthest, spanOk, keep);
            SimplifySpan(p, at, last, sideways, furthest, spanOk, keep);
        }

        Polyline SimplifyByHeight(const Polyline& points, bool closed, double sideways, double furthest,
                                  const std::function<bool(const Vec3&, const Vec3&)>& spanOk)
        {
            if (points.size() < 3)
                return points;
            Polyline work = points;
            size_t middle = 0;
            if (closed)
            {
                // Split at the point furthest from the start, and close the loop.
                double best = -1.0;
                for (size_t i = 1; i < points.size(); ++i)
                {
                    const double d = std::hypot(points[i].x - points[0].x, points[i].y - points[0].y);
                    if (d > best)
                    {
                        best = d;
                        middle = i;
                    }
                }
                work.push_back(points[0]);
            }
            std::vector<char> keep(work.size(), 0);
            keep.front() = keep.back() = 1;
            if (closed)
            {
                keep[middle] = 1;
                SimplifySpan(work, 0, middle, sideways, furthest, spanOk, keep);
                SimplifySpan(work, middle, work.size() - 1, sideways, furthest, spanOk, keep);
            }
            else
            {
                SimplifySpan(work, 0, work.size() - 1, sideways, furthest, spanOk, keep);
            }
            Polyline result;
            const size_t count = closed ? work.size() - 1 : work.size();
            for (size_t i = 0; i < count; ++i)
            {
                if (keep[i])
                    result.push_back(work[i]);
            }
            return result;
        }

        // Glitches left in the laps (not the links between them): turns of more than
        // 120 degrees between moves at least 0.02 long, S-bends within 1 mm with a
        // turn of more than 90 degrees, and wobbles: a turn of more than 10 degrees
        // one way then the next the other, the move between them under 0.5 mm.
        void CountLapGlitches(const std::vector<std::vector<Piece>>& levelLaps, size_t& turnBacks, size_t& zigzags,
                              size_t& wobbles)
        {
            turnBacks = zigzags = wobbles = 0;
            const double tenDegrees = std::sin(10.0 * 3.14159265358979323846 / 180.0);
            for (const std::vector<Piece>& pieces : levelLaps)
            {
                for (const Piece& piece : pieces)
                {
                    const Polyline& p = piece.points;
                    const size_t n = p.size();
                    if (n < 3)
                        continue;
                    if (piece.closed)
                    {
                        // A closed sliver (under 0.2 mm wide on average, as where the
                        // area's corner cuts a lap off) turns right round at its tips.
                        double area = 0.0, perimeter = 0.0;
                        for (size_t k = 0; k < n; ++k)
                        {
                            const Vec3& a = p[k];
                            const Vec3& b = p[(k + 1) % n];
                            area += a.x * b.y - b.x * a.y;
                            perimeter += std::hypot(b.x - a.x, b.y - a.y);
                        }
                        if (std::fabs(area) / 2.0 < 0.1 * perimeter)
                            continue;
                    }
                    const size_t segments = piece.closed ? n : n - 1;
                    const auto segment = [&](size_t k) { return p[(k + 1) % n] - p[k]; };
                    for (size_t k = 0; k + 1 < segments || (piece.closed && k < segments); ++k)
                    {
                        const Vec3 u = segment(k), v = segment((k + 1) % segments);
                        const double lu = Length(u), lv = Length(v);
                        if (lu >= 0.02 && lv >= 0.02 && Dot(u, v) < -0.5 * lu * lv)
                            ++turnBacks;
                        if (k + 2 >= segments && !piece.closed)
                            continue;
                        const Vec3 x = segment((k + 2) % segments);
                        const double c0 = u.x * v.y - u.y * v.x, c1 = v.x * x.y - v.y * x.x;
                        if (lu + lv + Length(x) < 1.0 && c0 * c1 < 0.0 && std::fabs(c0) > 1e-4 &&
                            std::fabs(c1) > 1e-4 && (std::min)(Dot(u, v), Dot(v, x)) < 0.0)
                            ++zigzags;
                        const double lx = Length(x);
                        if (lu > 1e-9 && lv > 1e-9 && lx > 1e-9 && lv < 0.5 && c0 * c1 < 0.0 &&
                            std::fabs(c0) > tenDegrees * lu * lv && std::fabs(c1) > tenDegrees * lv * lx)
                            ++wobbles;
                    }
                }
            }
        }

        // Moves every point of the laps sideways, along the lap's normal (laps have
        // the part on their right), to where the tip's exact height is the level:
        // first on the triangles by a bracketed root search, on all cores, then for
        // a ball onto the true surface by secant steps, faces worked out here on all
        // cores and those asked of Alibre afterwards on this thread. A point whose
        // level can't be bracketed near it stays where the grid put it.
        void MoveLapsOntoLevels(const WaterlineRoughToolpathInput& input, const WaterlineSetup& w,
                                const std::vector<double>& levels, std::vector<std::vector<Piece>>& levelLaps,
                                RefineCounts& totals, size_t& smoothed, size_t& fingers, size_t& pointsBefore,
                                size_t& pointsAfter)
        {
            const ExactHeights heights(input, w.s, w.bottom - 1.0);
            const auto record = [&](std::vector<std::vector<Polyline>>& stage) {
                stage.assign(levelLaps.size(), {});
                for (size_t li = 0; li < levelLaps.size(); ++li)
                {
                    for (const Piece& piece : levelLaps[li])
                        stage[li].push_back(piece.points);
                }
            };
            if (input.lapStages != nullptr)
            {
                input.lapStages->levels = levels;
                record(input.lapStages->grid);
                input.lapStages->closed.assign(levelLaps.size(), {});
                for (size_t li = 0; li < levelLaps.size(); ++li)
                {
                    for (const Piece& piece : levelLaps[li])
                        input.lapStages->closed[li].push_back(piece.closed);
                }
            }
            struct Target
            {
                Vec3* point;
                Vec3 normal;
                double z;           // the height the tip's lowest must be at
                double s = 0.0;     // how far along the normal it moves
                bool pending = false;
                bool moved = false;
                Vec3 local;             // square to the lap from its next points, a fallback
                bool between = false;   // it may go to the middle of its neighbours
                bool middle = false;    // it does
                size_t before = 0, after = 0;
            };
            std::vector<Target> targets;
            struct LapSpan
            {
                size_t first, count;
                bool closed;
            };
            std::vector<LapSpan> spans;
            for (size_t li = 0; li < levelLaps.size(); ++li)
            {
                for (Piece& piece : levelLaps[li])
                {
                    Polyline& points = piece.points;
                    const size_t n = points.size();
                    if (n < 2)
                        continue;
                    const size_t first = targets.size();
                    spans.push_back({ first, n, piece.closed });
                    // Square to the lap, from points `span` either side (the lap's left,
                    // away from the part); far enough apart to smooth the grid's jaggedness.
                    const auto across = [&](size_t k, size_t span) {
                        const size_t b = (k >= span) ? k - span : (piece.closed ? (k + n - span % n) % n : 0);
                        const size_t a = (k + span < n) ? k + span : (piece.closed ? (k + span) % n : n - 1);
                        const Vec3 tangent{ points[a].x - points[b].x, points[a].y - points[b].y, 0.0 };
                        Vec3 normal;
                        if (!Normalise(Vec3{ -tangent.y, tangent.x, 0.0 }, normal))
                            normal = Vec3{ 0.0, 0.0, 0.0 };
                        return normal;
                    };
                    for (size_t k = 0; k < n; ++k)
                    {
                        const Vec3 local = across(k, 1);
                        Vec3 normal = (n >= 7) ? across(k, 3) : local;
                        if (normal.x == 0.0 && normal.y == 0.0)
                            normal = local;     // stays put if that's none either
                        Target t{ &points[k], normal, levels[li] + kHeightTolerance };
                        t.local = local;
                        // Every second point (not an open lap's last) takes the middle of its neighbours'.
                        t.between = (k % 2 == 1) && (k + 1 < n || piece.closed) && n >= 3;
                        t.before = first + (k > 0 ? k - 1 : n - 1);
                        t.after = first + (k + 1 < n ? k + 1 : 0);
                        targets.push_back(t);
                    }
                }
            }
            if (targets.empty())
                return;

            // The grid's laps are within about a cell of the true ones.
            const double reach = w.cell;
            const auto triangles = [&](const Target& t, double s) {
                return heights.TriangleHeight(t.point->x + t.normal.x * s, t.point->y + t.normal.y * s) - t.z;
            };
            // On the triangles: the height falls going out from the part, so look for
            // a change of sign within a few cells, then close in (Illinois).
            const auto onTriangles = [&](Target& t) {
                double low = -reach, high = reach;
                double fLow = triangles(t, low), fHigh = triangles(t, high);
                for (int widen = 0; widen < 3 && !(fLow > 0.0 && fHigh <= 0.0); ++widen)
                {
                    low *= 2.0;
                    high *= 2.0;
                    fLow = triangles(t, low);
                    fHigh = triangles(t, high);
                }
                if (!(fLow > 0.0 && fHigh <= 0.0))
                    return false;
                int side = 0;
                double found = 0.5 * (low + high);
                for (int k = 0; k < 40 && high - low > 1e-5; ++k)
                {
                    const double mid = (low * fHigh - high * fLow) / (fHigh - fLow);
                    const double f = triangles(t, mid);
                    found = mid;
                    if (std::fabs(f) < 1e-4)
                        break;      // close enough: this point, not the bracket's middle
                    if (f > 0.0)
                    {
                        low = mid;
                        fLow = f;
                        if (side == -1)
                            fHigh *= 0.5;
                        side = -1;
                    }
                    else
                    {
                        high = mid;
                        fHigh = f;
                        if (side == 1)
                            fLow *= 0.5;
                        side = 1;
                    }
                    found = 0.5 * (low + high);
                }
                t.s = found;
                return true;
            };
            // Onto the true surface: secant steps on the exact height, the slope from
            // the triangles. False if that needs Alibre and this isn't its thread.
            // The first step takes its slope from the triangles; after that, secant
            // steps on the exact heights themselves, which are smooth, so it settles
            // quickly even where the facets are coarse and their slope rough. It keeps
            // the best point found.
            const auto onSurface = [&](Target& t, bool allowAlibre, RefineCounts& n) {
                const double start = t.s, d = w.cell / 4.0;
                const auto clamp = [&](double s) {
                    return (std::max)(start - 8.0 * reach, (std::min)(start + 8.0 * reach, s));
                };
                const auto exactError = [&](double s, double& error) {
                    double height = 0.0;
                    if (!heights.Height(t.point->x + t.normal.x * s, t.point->y + t.normal.y * s, n, allowAlibre,
                                        height))
                        return false;
                    error = height - t.z;
                    return true;
                };
                double s0 = t.s, e0 = 0.0;
                if (!exactError(s0, e0))
                    return false;
                if (std::fabs(e0) < 1e-4)
                    return true;
                const double slope = (triangles(t, s0 - d) - triangles(t, s0 + d)) / (2.0 * d);
                if (slope < 0.05)
                    return true;    // flat here: the level doesn't say where across
                double best = s0, bestError = std::fabs(e0);
                double s1 = clamp(s0 + e0 / slope);
                for (int k = 0; k < 6; ++k)
                {
                    double e1 = 0.0;
                    if (!exactError(s1, e1))
                    {
                        t.s = best;     // Alibre is needed from here: carry on later from the best
                        return false;
                    }
                    if (std::fabs(e1) < bestError)
                    {
                        best = s1;
                        bestError = std::fabs(e1);
                    }
                    if (std::fabs(e1) < 1e-4 || std::fabs(e1 - e0) < 1e-12)
                        break;
                    const double s2 = clamp(s1 - e1 * (s1 - s0) / (e1 - e0));
                    s0 = s1;
                    e0 = e1;
                    s1 = s2;
                }
                t.s = best;
                return true;
            };

            // A point moved in full: on the triangles, then the true surface where
            // that's worked out here; those Alibre is asked about are left pending.
            const auto moveFully = [&](Target& t, RefineCounts& n) {
                if (t.normal.x == 0.0 && t.normal.y == 0.0)
                    return;
                // Square to the lap (smoothed over a few points), so the point moves
                // across it and keeps its place along it. Failing that, straight downhill
                // (the exact height's slope), then square to the lap from its next points.
                bool found = onTriangles(t);
                if (!found)
                {
                    const double d = w.cell / 2.0, x = t.point->x, y = t.point->y;
                    const double gx =
                        (heights.TriangleHeight(x + d, y) - heights.TriangleHeight(x - d, y)) / (2.0 * d);
                    const double gy =
                        (heights.TriangleHeight(x, y + d) - heights.TriangleHeight(x, y - d)) / (2.0 * d);
                    const double slope = std::hypot(gx, gy);
                    if (slope >= 0.05)
                    {
                        t.normal = Vec3{ -gx / slope, -gy / slope, 0.0 };
                        found = onTriangles(t);
                    }
                }
                if (!found && (t.local.x != 0.0 || t.local.y != 0.0))
                {
                    t.normal = t.local;
                    found = onTriangles(t);
                }
                if (!found)
                    return;
                t.moved = true;
                if (heights.Refines() && !onSurface(t, false, n))
                    t.pending = true;
            };
            // Over the targets on all cores, a chunk at a time.
            const auto inParallel = [&](const char* stage, const auto& work) {
                const size_t chunk = 16;
                const size_t chunks = (targets.size() + chunk - 1) / chunk;
                std::vector<RefineCounts> counts(chunks);
                std::vector<size_t> order;
                const size_t slices = 10;
                for (size_t slice = 0; slice < slices; ++slice)
                {
                    order.clear();
                    for (size_t c = chunks * slice / slices; c < chunks * (slice + 1) / slices; ++c)
                        order.push_back(c);
                    std::for_each(std::execution::par, order.begin(), order.end(), [&](size_t c) {
                        for (size_t k = c * chunk; k < (std::min)(targets.size(), (c + 1) * chunk); ++k)
                            work(targets[k], counts[c]);
                    });
                    if (input.progress)
                        input.progress(static_cast<double>(slice + 1) / slices, stage);
                }
                for (const RefineCounts& c : counts)
                    totals.Add(c);
            };
            // Those Alibre is asked about, on this thread.
            const auto askPending = [&]() {
                size_t asked = 0, pendingCount = 0;
                for (const Target& t : targets)
                    pendingCount += t.pending ? 1 : 0;
                for (Target& t : targets)
                {
                    if (!t.pending)
                        continue;
                    t.pending = false;
                    onSurface(t, true, totals);
                    if (input.progress && ++asked % 200 == 0)
                        input.progress(static_cast<double>(asked) / pendingCount,
                                       "Asking Alibre for points on the true surface");
                }
            };

            // Every second point in full, then each between takes the middle of its
            // neighbours' moves where they agree (the move changes smoothly along a lap).
            inParallel("Moving the laps onto the parts", [&](Target& t, RefineCounts& n) {
                if (!t.between)
                    moveFully(t, n);
            });
            askPending();
            inParallel("Filling in between", [&](Target& t, RefineCounts& n) {
                if (!t.between)
                    return;
                const Target& before = targets[t.before];
                const Target& after = targets[t.after];
                if (before.moved && after.moved && std::fabs(before.s - after.s) < w.cell / 2.0)
                {
                    t.middle = true;
                    t.moved = true;
                    ++n.shared;
                }
                else
                {
                    moveFully(t, n);
                }
            });
            askPending();
            // Where each point ends up: its own move, or for a point between, the
            // middle of its neighbours' (the grid's points are jagged by up to a cell,
            // so their moves aren't smooth even though the true lap is). A point that
            // would leave the area stays where the grid put it.
            const auto final = [&](const Target& t) {
                return Vec3{ t.point->x + t.normal.x * t.s, t.point->y + t.normal.y * t.s, t.point->z };
            };
            const auto inArea = [&](const Vec3& p) {
                size_t q = 0;
                return w.CellAt(p, q) && w.inArea[q] != 0;
            };
            std::vector<Vec3> ends(targets.size());
            for (size_t k = 0; k < targets.size(); ++k)
            {
                const Target& t = targets[k];
                if (t.middle)
                    ends[k] = (final(targets[t.before]) + final(targets[t.after])) * 0.5;
                else
                    ends[k] = t.moved ? final(t) : *t.point;
                if (!inArea(ends[k]))
                    ends[k] = *t.point;
            }

            // Then each lap is cleaned up, on all cores, a lap at a time:
            // - A point that turns back on its neighbours (more than 135 degrees), or
            //   is more than a cell off the line between them, goes to their middle: a
            //   closest point Alibre found on the wrong part of a surface, or a step
            //   along a poor slope, would otherwise leave a spike or a little loop.
            // - A point that turns more than 45 degrees goes to its neighbours' middle
            //   if the tool's height there is within 0.01 mm (the moves' own
            //   tolerance) of where it was moved to (on the facets): on a shallow
            //   slope a tiny difference in height, as between Alibre's answers on a
            //   spline and its facets, moves a point a long way sideways, which leaves
            //   little hooks; on a steep side the height check keeps it put.
            // - An open lap's end that turns back onto the lap goes onto its neighbour.
            const std::vector<Vec3> moved = ends;
            std::vector<size_t> counts(spans.size(), 0);
            std::vector<size_t> spanOrder(spans.size());
            for (size_t k = 0; k < spans.size(); ++k)
                spanOrder[k] = k;
            std::for_each(std::execution::par, spanOrder.begin(), spanOrder.end(), [&](size_t si) {
                const LapSpan& span = spans[si];
                if (span.count < 3)
                    return;
                const auto at = [&](size_t k) -> Vec3& { return ends[span.first + k]; };
                for (int pass = 0; pass < 20; ++pass)
                {
                    bool changed = false;
                    const size_t from = span.closed ? 0 : 1, to = span.closed ? span.count : span.count - 1;
                    for (size_t k = from; k < to; ++k)
                    {
                        const Vec3& before = at(k > 0 ? k - 1 : span.count - 1);
                        const Vec3& after = at(k + 1 < span.count ? k + 1 : 0);
                        Vec3& p = at(k);
                        const Vec3 u = p - before, v = after - p;
                        const double lu = Length(u), lv = Length(v);
                        if (lu < 1e-6 || lv < 1e-6)
                            continue;
                        const double turn = Dot(u, v) / (lu * lv);    // the cosine of the turn
                        const Vec3 middle = (before + after) * 0.5;
                        bool move = turn < -0.7;
                        if (!move)
                        {
                            const Vec3 chord = after - before;
                            const double length = Length(chord);
                            if (length > 1e-9)
                            {
                                // Past one of its neighbours along their chord (it overtook
                                // one), or well off it (an outlier).
                                const double along = Dot(p - before, chord) / (length * length);
                                const double t = (std::max)(0.0, (std::min)(1.0, along));
                                move = along < 0.0 || along > 1.0 ||
                                       Length(p - (before + chord * t)) > (std::min)(w.cell, 0.05);
                            }
                        }
                        if (!move && turn < 0.7 && Length(p - middle) > 0.001)
                        {
                            const Vec3& was = moved[span.first + k];
                            move = std::fabs(heights.TriangleHeight(middle.x, middle.y) -
                                             heights.TriangleHeight(was.x, was.y)) <= 0.01;
                        }
                        if (move && inArea(middle))
                        {
                            p = middle;
                            ++counts[si];
                            changed = true;
                        }
                    }
                    if (!span.closed && span.count >= 3)
                    {
                        // The ends: back onto the lap if they turn back on it.
                        for (int end = 0; end < 2; ++end)
                        {
                            const size_t e = end == 0 ? 0 : span.count - 1;
                            const size_t n1 = end == 0 ? 1 : span.count - 2;
                            const size_t n2 = end == 0 ? 2 : span.count - 3;
                            const Vec3 u = at(n1) - at(n2), v = at(e) - at(n1);
                            if (Length(u) > 1e-6 && Length(v) > 1e-6 && Dot(u, v) < 0.0)
                            {
                                at(e) = at(n1);
                                ++counts[si];
                                changed = true;
                            }
                        }
                    }
                    if (!changed)
                        break;
                }
            });
            for (size_t c : counts)
                smoothed += c;
            if (input.lapStages != nullptr)
            {
                for (size_t k = 0; k < targets.size(); ++k)
                    *targets[k].point = moved[k];
                record(input.lapStages->moved);
            }
            for (size_t k = 0; k < targets.size(); ++k)
                *targets[k].point = ends[k];
            if (input.lapStages != nullptr)
                record(input.lapStages->cleaned);

            // Thin fingers and small loops in the level line itself (the ball just
            // grazing a slight ridge) are cut across where the tool keeps clear of
            // the part along the shortcut: its lowest tip no higher than the level
            // and the moves' 0.01 mm.
            struct PieceRef
            {
                Piece* piece;
                double z;
            };
            std::vector<PieceRef> pieces;
            for (size_t li = 0; li < levelLaps.size(); ++li)
            {
                for (Piece& piece : levelLaps[li])
                    pieces.push_back({ &piece, levels[li] + kHeightTolerance });
            }
            std::vector<size_t> cutOff(pieces.size(), 0);
            std::vector<size_t> pieceOrder(pieces.size());
            for (size_t k = 0; k < pieceOrder.size(); ++k)
                pieceOrder[k] = k;
            std::for_each(std::execution::par, pieceOrder.begin(), pieceOrder.end(), [&](size_t k) {
                const double z = pieces[k].z;
                RefineCounts ignored;
                const auto clear = [&](const Vec3& from, const Vec3& to) {
                    const double length = std::hypot(to.x - from.x, to.y - from.y);
                    const int samples = (std::max)(1, static_cast<int>(std::ceil(length / 0.02)));
                    for (int s = 0; s <= samples; ++s)
                    {
                        const double f = static_cast<double>(s) / samples;
                        const double x = from.x + (to.x - from.x) * f, y = from.y + (to.y - from.y) * f;
                        double height = 0.0;
                        if (!heights.Height(x, y, ignored, false, height))
                            height = heights.TriangleHeight(x, y);
                        if (height > z + 0.01)
                            return false;
                    }
                    return true;
                };
                cutOff[k] = RemoveThinFingers(pieces[k].piece->points, pieces[k].piece->closed, clear);
            });
            for (size_t c : cutOff)
                fingers += c;

            // Simplified by height: a straight move may replace points where it stays
            // within 0.01 mm sideways of them (as on steep walls), or where the tool's
            // lowest tip along it stays within 0.01 mm of the level, up or down, and it's
            // within 0.5 mm sideways (so on shallow ground the lap runs smoothly
            // instead of following tiny sideways wanderings of the level line).
            const double sideways = (std::min)(w.cell / 4.0, 0.01);
            std::vector<size_t> pointCounts(pieces.size() * 2, 0);
            std::for_each(std::execution::par, pieceOrder.begin(), pieceOrder.end(), [&](size_t k) {
                Piece& piece = *pieces[k].piece;
                const double z = pieces[k].z;
                RefineCounts ignored;
                const auto inBand = [&](const Vec3& from, const Vec3& to) {
                    const double length = std::hypot(to.x - from.x, to.y - from.y);
                    const int samples = (std::max)(1, static_cast<int>(std::ceil(length / 0.05)));
                    for (int s = 1; s < samples; ++s)
                    {
                        const double f = static_cast<double>(s) / samples;
                        const double x = from.x + (to.x - from.x) * f, y = from.y + (to.y - from.y) * f;
                        double height = 0.0;
                        if (!heights.Height(x, y, ignored, false, height))
                            height = heights.TriangleHeight(x, y);
                        if (std::fabs(height - z) > 0.01)
                            return false;
                    }
                    return true;
                };
                pointCounts[2 * k] = piece.points.size();
                piece.points = SimplifyByHeight(piece.points, piece.closed, sideways, 0.5, inBand);
                pointCounts[2 * k + 1] = piece.points.size();
            });
            for (size_t k = 0; k < pieces.size(); ++k)
            {
                pointsBefore += pointCounts[2 * k];
                pointsAfter += pointCounts[2 * k + 1];
            }
        }
    }

    ToolpathResult GenerateWaterlineFinishToolpath(const WaterlineRoughToolpathInput& input)
    {
        if (input.progress)
            input.progress(0.0, "Working out the area and the tool's reach");
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

        // The levels: the stepdown's (and Clear Flats'), or with Optimise Stepover,
        // each chosen to suit the slope: from each level the next stepdown level is
        // tried, and the step halved (to no less than 0.01 mm) until the laps there are
        // no further across than the stepover from where the tool was blocked at the
        // level above. Steep parts keep the whole stepdown; shallow ones get more.
        std::vector<double> levels;
        if (!settings.optimiseStepover)
        {
            levels = w.levels;
        }
        else
        {
            // How far across the laps at z are from the cells blocked at `above`.
            double distanceLevel = std::numeric_limits<double>::quiet_NaN();
            std::vector<float> fromAbove;
            bool anyAbove = false;
            const auto gapAt = [&](double above, double z) {
                if (!(above == distanceLevel))
                {
                    std::vector<char> blocked(cells);
                    for (size_t q = 0; q < cells; ++q)
                        blocked[q] = w.Blocked(q, above) ? 1 : 0;
                    fromAbove = DistanceFrom(blocked, nx, ny, w.cell, anyAbove);
                    distanceLevel = above;
                }
                double gap = 0.0;
                if (anyAbove)
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
                return gap;
            };
            bool first = true;
            double from = w.top;
            for (double target : w.levels)
            {
                if (first)
                {
                    levels.push_back(target);
                    from = target;
                    first = false;
                    continue;
                }
                while (from - target > 1e-9 && levels.size() < 5000)
                {
                    double z = target;
                    while (from - z > 0.02 + 1e-9 && gapAt(from, z) > w.pitch)
                        z = from - (from - z) / 2.0;
                    levels.push_back(z);
                    from = z;
                }
            }
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

        // Each level's laps from the grid, each point then moved sideways onto the
        // level exactly (as Parallel Finishing's rows are), and only then simplified.
        std::vector<std::vector<Piece>> levelLaps(levels.size());
        for (size_t li = 0; li < levels.size(); ++li)
        {
            if (input.progress)
                input.progress(static_cast<double>(li) / levels.size(),
                               "Level " + std::to_string(li + 1) + " of " + std::to_string(levels.size()));
            for (Polyline& loop :
                 IsoContours(lowness, nx, ny, w.xLow, w.yLow, w.cell, -(levels[li] + kHeightTolerance), levels[li]))
            {
                for (Piece& piece : ClipContour(loop, w, w.inArea, true, false))
                    levelLaps[li].push_back(std::move(piece));
            }
        }
        RefineCounts totals;
        size_t smoothed = 0, fingers = 0, pointsBefore = 0, pointsAfter = 0;
        MoveLapsOntoLevels(input, w, levels, levelLaps, totals, smoothed, fingers, pointsBefore, pointsAfter);
        size_t turnBacks = 0, zigzags = 0, wobbles = 0;
        CountLapGlitches(levelLaps, turnBacks, zigzags, wobbles);
        if (input.lapStages != nullptr)
        {
            input.lapStages->simplified.assign(levelLaps.size(), {});
            for (size_t li = 0; li < levelLaps.size(); ++li)
            {
                for (const Piece& piece : levelLaps[li])
                    input.lapStages->simplified[li].push_back(piece.points);
            }
        }

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
            for (Piece& piece : levelLaps[li])
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
                // At rapid to the approach distance above the lap's start, then at the plunge feed.
                writer.Enter(start, (std::min)(start.z + settings.approachDistance, w.clearance), feeds.plunge);
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
        totals.Into(result);
        result.lapPointsSmoothed = smoothed;
        result.lapFingersRemoved = fingers;
        result.lapTurnBacks = turnBacks;
        result.lapZigzags = zigzags;
        result.lapWobbles = wobbles;
        result.lapPointsBefore = pointsBefore;
        result.lapPointsAfter = pointsAfter;
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
        // Progress, from this thread only; parallel work reports between slices.
        const auto Report = [&](double fraction, const char* stage) {
            if (input.progress)
                input.progress(fraction, stage);
        };
        const auto ParallelSlices = [&](size_t count, const char* stage, const auto& work) {
            const size_t slices = 40;
            std::vector<size_t> indexes;
            for (size_t slice = 0; slice < slices; ++slice)
            {
                const size_t begin = count * slice / slices, end = count * (slice + 1) / slices;
                indexes.clear();
                for (size_t k = begin; k < end; ++k)
                    indexes.push_back(k);
                std::for_each(std::execution::par, indexes.begin(), indexes.end(), [&](size_t k) { work(k); });
                Report(static_cast<double>(slice + 1) / slices, stage);
            }
        };

        Report(0.0, "Working out the area");
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
        // exactly for its shape (not the grid, which would make the rows step), and
        // for a ball, moved onto the true surface.
        const ExactHeights heights(input, w.s, floorZ);
        const DropCutter& cutter = heights.Cutter();
        const bool refine = heights.Refines();
        const auto surfaceAt = [&](double x, double y) { return heights.TriangleHeight(x, y); };
        const auto analytic = [&](int face) { return heights.Analytic(face); };
        RefineCounts totals;
        const auto refineOnce = [&](double x, double y, const DropCutter::Contact& contact, double guess, double& tip,
                                    RefineCounts& n) { return heights.RefineOnce(x, y, contact, guess, tip, n); };
        // The height at (x, y), refined when it can be: this thread only.
        const auto exactAt = [&](double x, double y) {
            double height = floorZ;
            heights.Height(x, y, totals, true, height);
            return height;
        };
        const double tolerance = (std::min)(w.cell / 4.0, input.simplifyTolerance);
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
            // The triangles' contacts on all cores.
            std::vector<DropCutter::Contact> contacts(samples.size());
            std::vector<char> found(samples.size(), 0);
            ParallelSlices(samples.size(), "Dropping the tool on the parts", [&](size_t k) {
                found[k] = cutter.ContactAt(samples[k]->x, samples[k]->y, contacts[k]) ? 1 : 0;
            });

            // Along each run, every second point (and the last) is refined, starting
            // from the one before's correction. The point between two refined on
            // the same face takes the middle of their heights. Points on faces
            // worked out here go on all cores, run by run; those Alibre is asked
            // about follow on this thread.
            std::vector<char> done(samples.size(), 0), pending(samples.size(), 0);
            std::vector<double> correction(samples.size(), 0.0);
            std::vector<RefineCounts> runCounts(runSpans.size());
            const auto refinePoint = [&](size_t k, double guess, RefineCounts& n) {
                Vec3& p = *samples[k];
                if (!found[k])
                {
                    p.z = floorZ;
                    return;
                }
                double tip = 0.0;
                if (refineOnce(p.x, p.y, contacts[k], guess, tip, n))
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
            const auto askAlibre = [&](size_t k) {
                return found[k] && contacts[k].face >= 0 && !analytic(contacts[k].face);
            };
            const auto isMain = [](size_t first, size_t last, size_t k) {
                return (k - first) % 2 == 0 || k + 1 == last;
            };
            // The guess for point k: the correction of the refined point before it on the same face.
            const auto guessFor = [&](size_t first, size_t k, size_t back) {
                if (k < first + back)
                    return 0.0;
                const size_t before = k - back;
                return (found[k] && done[before] && contacts[before].face == contacts[k].face) ? correction[before]
                                                                                                : 0.0;
            };

            // 1. The main points worked out here.
            ParallelSlices(runSpans.size(), "Moving points onto the true surface", [&](size_t r) {
                const auto [first, last] = runSpans[r];
                for (size_t k = first; k < last; ++k)
                {
                    if (!isMain(first, last, k))
                        continue;
                    if (askAlibre(k))
                    {
                        pending[k] = 1;
                        continue;
                    }
                    refinePoint(k, guessFor(first, k, (k + 1 == last && (k - first) % 2 == 1) ? 1 : 2),
                                runCounts[r]);
                }
            });
            // 2. The main points Alibre is asked about, on this thread.
            const auto askPending = [&](const char* stage) {
                size_t count = 0, total = 0;
                for (char flag : pending)
                    total += flag ? 1 : 0;
                for (size_t r = 0; r < runSpans.size(); ++r)
                {
                    const auto [first, last] = runSpans[r];
                    for (size_t k = first; k < last; ++k)
                    {
                        if (!pending[k])
                            continue;
                        pending[k] = 0;
                        const size_t back = isMain(first, last, k) ? ((k + 1 == last && (k - first) % 2 == 1) ? 1 : 2)
                                                                   : 1;
                        refinePoint(k, guessFor(first, k, back), totals);
                        if (++count % 500 == 0)
                            Report(static_cast<double>(count) / total, stage);
                    }
                }
            };
            askPending("Asking Alibre for points on the true surface");
            // 3. The points between, worked out here where they can be.
            ParallelSlices(runSpans.size(), "Filling in between the refined points", [&](size_t r) {
                const auto [first, last] = runSpans[r];
                RefineCounts& n = runCounts[r];
                for (size_t k = first + 1; k + 1 < last; k += 2)
                {
                    // Within one face the true heights are smooth, so the middle of
                    // two refined ones is off by only the curvature times the spacing
                    // squared over 8; the triangles' answer has kinks, so it isn't
                    // used, except as a check that nothing odd lies between.
                    const int face = found[k] ? contacts[k].face : -1;
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
                            ++n.shared;
                            continue;
                        }
                    }
                    if (askAlibre(k))
                        pending[k] = 1;
                    else
                        refinePoint(k, guessFor(first, k, 1), n);
                }
            });
            // 4. The points between that Alibre is asked about.
            askPending("Asking Alibre for points between");
            for (const RefineCounts& n : runCounts)
                totals.Add(n);
        }
        else
        {
            ParallelSlices(samples.size(), "Dropping the tool on the parts",
                           [&](size_t k) { samples[k]->z = surfaceAt(samples[k]->x, samples[k]->y); });
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
                // At rapid to the approach distance above the row's start, then at the plunge
                // feed (not from above the stock's top through what roughing cleared).
                writer.Enter(start, (std::min)(start.z + settings.approachDistance, w.clearance), feeds.plunge);
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
        totals.Into(result);
        Report(1.0, "Writing the moves");
        return result;
    }
}
