// PlungeRoughToolpath.cpp : calculates a Plunge Roughing operation's toolpath.

#include "PlungeRoughToolpath.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "Boundary.h"
#include "Clearance.h"
#include "FacingToolpath.h"     // PassDirection

namespace camcore
{
    namespace
    {
        const float kNoHeight = std::numeric_limits<float>::lowest();
        const size_t kMostHeightCells = 16000000;
        const size_t kMostPlunges = 200000;

        ToolpathResult Failure(const std::string& problem)
        {
            ToolpathResult result;
            result.problem = problem;
            return result;
        }

        Vec3 LeftOf(const Vec3& direction) { return { -direction.y, direction.x, 0.0 }; }

        // The distance in XY from p to the closed polyline.
        double DistanceToOutline(const Polyline& outline, double x, double y)
        {
            double best = std::numeric_limits<double>::max();
            for (size_t i = 0; i + 1 < outline.size(); ++i)
            {
                const double ax = outline[i].x, ay = outline[i].y;
                const double ex = outline[i + 1].x - ax, ey = outline[i + 1].y - ay;
                const double lengthSquared = ex * ex + ey * ey;
                double t = 0.0;
                if (lengthSquared > 1e-18)
                    t = (std::min)(1.0, (std::max)(0.0, ((x - ax) * ex + (y - ay) * ey) / lengthSquared));
                best = (std::min)(best, std::hypot(x - (ax + ex * t), y - (ay + ey * t)));
            }
            return best;
        }

        // Where a stretch of the row, inside the outline, is at least reach from it.
        void ClearRuns(const Polyline& outline, const Vec3& along, const Vec3& across, double v, double a, double b,
                       double reach, double step, std::vector<std::pair<double, double>>& runs)
        {
            const auto clear = [&](double u) {
                const Vec3 p = along * u + across * v;
                return DistanceToOutline(outline, p.x, p.y) >= reach - 1e-9;
            };
            // The edge between a clear and a blocked sample, found more closely.
            const auto edge = [&](double blocked, double open) {
                for (int i = 0; i < 30; ++i)
                {
                    const double middle = (blocked + open) / 2.0;
                    (clear(middle) ? open : blocked) = middle;
                }
                return open;
            };

            const int samples = (std::max)(2, static_cast<int>(std::ceil((b - a) / step)) + 1);
            bool inRun = false;
            double runStart = 0.0;
            double previous = a;
            for (int i = 0; i < samples; ++i)
            {
                const double u = a + (b - a) * i / (samples - 1);
                const bool isClear = clear(u);
                if (isClear && !inRun)
                {
                    runStart = (i == 0) ? u : edge(previous, u);
                    inRun = true;
                }
                else if (!isClear && inRun)
                {
                    runs.push_back({ runStart, edge(u, previous) });
                    inRun = false;
                }
                previous = u;
            }
            if (inRun)
                runs.push_back({ runStart, b });
        }
    }

    /////////////////////////////////////////////////////////////////////////
    // The parts' heights

    void PartHeightField::Build(const std::vector<Vec3>& triangles, double xLow, double yLow, double xHigh,
                                double yHigh, double cell)
    {
        // Coarser cells if the grid would be too big.
        for (;;)
        {
            m_nx = (std::max)(1, static_cast<int>(std::ceil((xHigh - xLow) / cell)));
            m_ny = (std::max)(1, static_cast<int>(std::ceil((yHigh - yLow) / cell)));
            if (static_cast<size_t>(m_nx) * static_cast<size_t>(m_ny) <= kMostHeightCells)
                break;
            cell *= 1.5;
        }
        m_xLow = xLow;
        m_yLow = yLow;
        m_cell = cell;
        m_height.assign(static_cast<size_t>(m_nx) * m_ny, kNoHeight);

        for (size_t t = 0; t + 2 < triangles.size(); t += 3)
        {
            const Vec3& a = triangles[t];
            const Vec3& b = triangles[t + 1];
            const Vec3& c = triangles[t + 2];
            const double minX = (std::min)({ a.x, b.x, c.x }), maxX = (std::max)({ a.x, b.x, c.x });
            const double minY = (std::min)({ a.y, b.y, c.y }), maxY = (std::max)({ a.y, b.y, c.y });
            if (maxX < m_xLow || maxY < m_yLow || minX > m_xLow + m_nx * m_cell || minY > m_yLow + m_ny * m_cell)
                continue;

            // The edges, sampled at half a cell, and so the corners too.
            for (const auto& edge : { std::make_pair(a, b), std::make_pair(b, c), std::make_pair(c, a) })
            {
                const Vec3 d = edge.second - edge.first;
                const int count = (std::max)(1, static_cast<int>(std::ceil(std::hypot(d.x, d.y) / (0.5 * m_cell))));
                for (int i = 0; i <= count; ++i)
                {
                    const Vec3 p = edge.first + d * (static_cast<double>(i) / count);
                    Raise(p.x, p.y, p.z);
                }
            }

            // The cells whose centres are inside it, seen from above.
            const double area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
            if (std::fabs(area) < 1e-12)
                continue;
            const int i0 = (std::max)(0, static_cast<int>(std::floor((minX - m_xLow) / m_cell)));
            const int i1 = (std::min)(m_nx - 1, static_cast<int>(std::floor((maxX - m_xLow) / m_cell)));
            const int j0 = (std::max)(0, static_cast<int>(std::floor((minY - m_yLow) / m_cell)));
            const int j1 = (std::min)(m_ny - 1, static_cast<int>(std::floor((maxY - m_yLow) / m_cell)));
            for (int j = j0; j <= j1; ++j)
            {
                const double y = m_yLow + (j + 0.5) * m_cell;
                for (int i = i0; i <= i1; ++i)
                {
                    const double x = m_xLow + (i + 0.5) * m_cell;
                    const double wa = ((b.x - x) * (c.y - y) - (c.x - x) * (b.y - y)) / area;
                    const double wb = ((c.x - x) * (a.y - y) - (a.x - x) * (c.y - y)) / area;
                    const double wc = 1.0 - wa - wb;
                    if (wa < -1e-12 || wb < -1e-12 || wc < -1e-12)
                        continue;
                    float& h = m_height[static_cast<size_t>(j) * m_nx + i];
                    h = (std::max)(h, static_cast<float>(wa * a.z + wb * b.z + wc * c.z));
                }
            }
        }
    }

    void PartHeightField::Raise(double x, double y, double z)
    {
        const int i = static_cast<int>(std::floor((x - m_xLow) / m_cell));
        const int j = static_cast<int>(std::floor((y - m_yLow) / m_cell));
        if (i < 0 || j < 0 || i >= m_nx || j >= m_ny)
            return;
        float& h = m_height[static_cast<size_t>(j) * m_nx + i];
        h = (std::max)(h, static_cast<float>(z));
    }

    bool PartHeightField::TipHeight(const ToolProfile& profile, double stockToLeave, double x, double y,
                                    double& tip) const
    {
        // The tool grown by the stock to leave: at a distance d from its axis it's
        // the tool's shape at d less the stock to leave, lowered by it. A cell is
        // taken at its nearest to the axis.
        const double reach = profile.radius + stockToLeave;
        const double halfDiagonal = m_cell * 0.7072;
        const int i0 = (std::max)(0, static_cast<int>(std::floor((x - reach - m_xLow) / m_cell)));
        const int i1 = (std::min)(m_nx - 1, static_cast<int>(std::floor((x + reach - m_xLow) / m_cell)));
        const int j0 = (std::max)(0, static_cast<int>(std::floor((y - reach - m_yLow) / m_cell)));
        const int j1 = (std::min)(m_ny - 1, static_cast<int>(std::floor((y + reach - m_yLow) / m_cell)));
        bool found = false;
        for (int j = j0; j <= j1; ++j)
        {
            const double cy = m_yLow + (j + 0.5) * m_cell;
            for (int i = i0; i <= i1; ++i)
            {
                const float h = m_height[static_cast<size_t>(j) * m_nx + i];
                if (h == kNoHeight)
                    continue;
                const double cx = m_xLow + (i + 0.5) * m_cell;
                const double d = (std::max)(0.0, std::hypot(cx - x, cy - y) - halfDiagonal);
                if (d > reach)
                    continue;
                const double rise = profile.HeightForRadius((std::max)(0.0, d - stockToLeave));
                if (rise < 0.0)
                    continue;
                const double needed = h + stockToLeave - rise;
                tip = found ? (std::max)(tip, needed) : needed;
                found = true;
            }
        }
        return found;
    }

    /////////////////////////////////////////////////////////////////////////
    // The plunges

    std::vector<std::vector<Vec3>> PlungeRows(const Path& outline, double reach, double pitch,
                                              const PlungeRoughingSettings& settings)
    {
        std::vector<std::vector<Vec3>> rows;
        if (outline.size() < 2 || !(pitch > 0.0))
            return rows;

        const Vec3 along = PassDirection(settings.cutAngle);
        const Vec3 across = LeftOf(along);
        const Polyline polyline = PathPoints(outline, true);

        double low = 0.0, high = 0.0;
        PathExtent(outline, across, low, high);
        low += reach;
        high -= reach;
        if (high < low - 1e-9)
            return rows;

        // Rows at most a circle-packing gap apart, spread evenly from side to side.
        const double gap = pitch * std::sqrt(3.0) / 2.0;
        const int intervals = (high - low < 1e-9) ? 0 : static_cast<int>(std::ceil((high - low) / gap - 1e-9));
        const double spacing = (intervals > 0) ? (high - low) / intervals : 0.0;
        const double step = (std::min)(pitch, reach) / 16.0;

        // Start at Bottom starts on the frame's -Y side, as Pocketing's Linear Cuts do.
        const bool acrossGoesUp = std::fabs(across.y) > 1e-9 ? across.y > 0.0 : across.x < 0.0;
        const bool fromLow = (settings.start == PlungeStartSide::Bottom) == acrossGoesUp;

        for (int r = 0; r <= intervals; ++r)
        {
            const double v = (intervals == 0) ? (low + high) / 2.0 : low + r * spacing;
            std::vector<double> crossings = PathCrossings(outline, along, across, v);
            std::sort(crossings.begin(), crossings.end());

            std::vector<std::pair<double, double>> runs;
            for (size_t c = 0; c + 1 < crossings.size(); c += 2)
                ClearRuns(polyline, along, across, v, crossings[c], crossings[c + 1], reach, step, runs);

            // The grid's points in each run, alternate rows shifted half a pitch,
            // and the run's ends where the grid leaves too much.
            const double shift = (r % 2 != 0) ? pitch / 2.0 : 0.0;
            std::vector<double> us;
            for (const auto& run : runs)
            {
                std::vector<double> inRun;
                const long long first = static_cast<long long>(std::ceil((run.first - shift) / pitch - 1e-9));
                for (long long k = first; shift + k * pitch <= run.second + 1e-9; ++k)
                    inRun.push_back(shift + k * pitch);
                if (inRun.empty() || inRun.front() - run.first > pitch / 2.0)
                    inRun.insert(inRun.begin(), run.first);
                if (inRun.back() < run.second - 1e-9 && run.second - inRun.back() > pitch / 2.0)
                    inRun.push_back(run.second);
                us.insert(us.end(), inRun.begin(), inRun.end());
            }
            if (us.empty())
                continue;
            std::vector<Vec3> row;
            for (double u : us)
                row.push_back(along * u + across * v);
            rows.push_back(std::move(row));
        }

        if (!fromLow)
            std::reverse(rows.begin(), rows.end());

        // Zig runs every row forward along the angle; ZigZag turns back each row.
        for (size_t r = 0; r < rows.size(); ++r)
        {
            if (settings.direction == PlungePathDirection::ZigZag && r % 2 != 0)
                std::reverse(rows[r].begin(), rows[r].end());
        }
        return rows;
    }

    ToolpathResult GeneratePlungeRoughToolpath(const PlungeRoughToolpathInput& input)
    {
        const SequenceStep* pOperation = input.operation;
        if (pOperation == nullptr || pOperation->type != StepType::PlungeRoughing)
            return Failure("This isn't a plunge roughing operation.");
        if (input.tool == nullptr)
            return Failure("Choose a tool on the Tool tab.");
        if (input.outlines.empty())
            return Failure("There's no closed area to rough.");

        const PlungeRoughingSettings& settings = pOperation->plungeRoughing;
        for (const std::vector<std::string>& problems : { ValidatePlungeRoughing(settings), ValidateFeeds(pOperation->feeds) })
        {
            if (!problems.empty())
                return Failure(problems.front());
        }

        const Tool& tool = *input.tool;
        if (!(tool.diameter > 0.0))
            return Failure("The tool's diameter must be more than 0.");
        const double pitch = ResolveSize(settings.stepover, tool.diameter);
        if (!(pitch > 0.0))
            return Failure("The stepover must be more than 0.");

        double clearance = 0.0;
        std::string clearanceProblem;
        if (!ClearanceHeight(pOperation->clearance, input.havePartMaxZ, input.partMaxZ, clearance, clearanceProblem,
                             input.haveStockMaxZ, input.stockMaxZ))
            return Failure(clearanceProblem);

        // The top of the material, and the floor where no part is under a plunge.
        const double level = input.outlines.front().path.front().point.z;
        const double top = input.haveStockMaxZ ? input.stockMaxZ : input.havePartMaxZ ? input.partMaxZ : level;
        const double floor = input.haveStockMinZ ? input.stockMinZ : level;
        const double approach = top + settings.approach;
        if (approach > clearance + 1e-9)
        {
            return Failure("The clearance plane is below the approach height. Raise the clearance plane, or "
                           "shorten the approach.");
        }

        // The plunges, then the parts' heights under them.
        const ToolProfile profile = ProfileOf(tool);
        const double reach = profile.radius + settings.stockToLeave;
        std::vector<std::vector<Vec3>> rows;
        size_t count = 0;
        for (const ProfileChain& outline : input.outlines)
        {
            if (!outline.closed)
                return Failure("Each area's edges must join into a closed outline.");
            std::vector<std::vector<Vec3>> more = PlungeRows(outline.path, reach, pitch, settings);
            for (const auto& row : more)
                count += row.size();
            if (count > kMostPlunges)
                return Failure("There would be too many plunges. Make the stepover larger.");
            rows.insert(rows.end(), more.begin(), more.end());
        }
        if (count == 0)
            return Failure("The tool doesn't fit inside the area with the stock to leave.");

        double xLow = 0.0, xHigh = 0.0, yLow = 0.0, yHigh = 0.0;
        bool first = true;
        for (const auto& row : rows)
        {
            for (const Vec3& p : row)
            {
                xLow = first ? p.x : (std::min)(xLow, p.x);
                xHigh = first ? p.x : (std::max)(xHigh, p.x);
                yLow = first ? p.y : (std::min)(yLow, p.y);
                yHigh = first ? p.y : (std::max)(yHigh, p.y);
                first = false;
            }
        }
        const double margin = reach + 1.0;
        PartHeightField heights;
        heights.Build(input.partTriangles, xLow - margin, yLow - margin, xHigh + margin, yHigh + margin,
                      (std::min)(0.5, (std::max)(0.02, tool.diameter / 20.0)));

        const FeedSettings& feeds = pOperation->feeds;
        ToolpathResult result;
        Toolpath& path = result.toolpath;
        const auto add = [&](MoveKind kind, const Vec3& to, double feed) {
            ToolpathMove move;
            move.kind = kind;
            move.to = to;
            move.feed = feed;
            path.moves.push_back(move);
        };

        bool started = false;
        for (const auto& row : rows)
        {
            for (const Vec3& p : row)
            {
                double tip = floor;
                if (heights.TipHeight(profile, settings.stockToLeave, p.x, p.y, tip) && input.haveStockMinZ)
                    tip = (std::max)(tip, floor);
                if (tip >= top - 1e-6)
                    continue;       // nothing to cut here

                const Vec3 above{ p.x, p.y, approach };
                if (!started)
                {
                    add(MoveKind::Rapid, { p.x, p.y, clearance }, 0.0);
                    add(MoveKind::Rapid, above, 0.0);
                    started = true;
                }
                else if (feeds.transferAtRapid)
                {
                    add(MoveKind::Rapid, above, 0.0);
                }
                else
                {
                    add(MoveKind::Feed, above, feeds.transfer);
                }
                add(MoveKind::Feed, { p.x, p.y, tip }, feeds.plunge);
                add(MoveKind::Feed, above, feeds.retract);
            }
        }
        if (!started)
            return Failure("There's nothing to cut: the parts are at or above the stock's top everywhere.");
        const Vec3 last = path.moves.back().to;
        add(MoveKind::Rapid, { last.x, last.y, clearance }, 0.0);

        result.ok = true;
        return result;
    }
}
