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

        // The squared distance transform of one line: for each place, the least of
        // (place - q)^2 + f[q] (Felzenszwalb and Huttenlocher).
        void DistanceLine(const std::vector<double>& f, std::vector<double>& d, std::vector<int>& v,
                          std::vector<double>& z)
        {
            const int n = static_cast<int>(f.size());
            int k = 0;
            v[0] = 0;
            z[0] = -std::numeric_limits<double>::infinity();
            z[1] = std::numeric_limits<double>::infinity();
            for (int q = 1; q < n; ++q)
            {
                double s = 0.0;
                for (;;)
                {
                    const int p = v[static_cast<size_t>(k)];
                    s = ((f[static_cast<size_t>(q)] + static_cast<double>(q) * q) -
                         (f[static_cast<size_t>(p)] + static_cast<double>(p) * p)) /
                        (2.0 * static_cast<double>(q - p));
                    if (s > z[static_cast<size_t>(k)] || k == 0)
                        break;
                    --k;
                }
                if (s <= z[static_cast<size_t>(k)])
                    s = z[static_cast<size_t>(k)];
                ++k;
                v[static_cast<size_t>(k)] = q;
                z[static_cast<size_t>(k)] = s;
                z[static_cast<size_t>(k) + 1] = std::numeric_limits<double>::infinity();
            }
            k = 0;
            for (int q = 0; q < n; ++q)
            {
                while (z[static_cast<size_t>(k) + 1] < static_cast<double>(q))
                    ++k;
                const int p = v[static_cast<size_t>(k)];
                d[static_cast<size_t>(q)] = static_cast<double>(q - p) * (q - p) + f[static_cast<size_t>(p)];
            }
        }
    }

    /////////////////////////////////////////////////////////////////////////
    // The plunge area

    void PlungeArea::Reset(double xLow, double yLow, double xHigh, double yHigh, double cell)
    {
        for (;;)
        {
            m_nx = (std::max)(1, static_cast<int>(std::ceil((xHigh - xLow) / cell))) + 4;
            m_ny = (std::max)(1, static_cast<int>(std::ceil((yHigh - yLow) / cell))) + 4;
            if (static_cast<size_t>(m_nx) * static_cast<size_t>(m_ny) <= kMostHeightCells)
                break;
            cell *= 1.5;
        }
        m_cell = cell;
        m_xLow = xLow - 2.0 * cell;
        m_yLow = yLow - 2.0 * cell;
        m_count = 0;
        m_in.assign(static_cast<size_t>(m_nx) * m_ny, 0);
        m_distance.clear();
    }

    void PlungeArea::AddLoop(const Path& loop)
    {
        // Each row of cell centres between pairs of the loop's crossings.
        const Vec3 along{ 1.0, 0.0, 0.0 };
        const Vec3 across{ 0.0, 1.0, 0.0 };
        for (int j = 0; j < m_ny; ++j)
        {
            std::vector<double> crossings = PathCrossings(loop, along, across, Y(j));
            std::sort(crossings.begin(), crossings.end());
            for (size_t c = 0; c + 1 < crossings.size(); c += 2)
            {
                const int i0 = (std::max)(0, static_cast<int>(std::ceil((crossings[c] - m_xLow) / m_cell - 0.5)));
                const int i1 =
                    (std::min)(m_nx - 1, static_cast<int>(std::floor((crossings[c + 1] - m_xLow) / m_cell - 0.5)));
                for (int i = i0; i <= i1; ++i)
                    m_in[static_cast<size_t>(j) * m_nx + i] = 1;
            }
        }
    }

    void PlungeArea::AddFace(const std::vector<Vec3>& triangles)
    {
        if (triangles.size() < 3)
            return;

        // The face's own cells, in a box round it with a ring of empty cells.
        double minX = triangles[0].x, maxX = minX, minY = triangles[0].y, maxY = minY;
        for (const Vec3& p : triangles)
        {
            minX = (std::min)(minX, p.x);
            maxX = (std::max)(maxX, p.x);
            minY = (std::min)(minY, p.y);
            maxY = (std::max)(maxY, p.y);
        }
        const int bi0 = (std::max)(0, static_cast<int>(std::floor((minX - m_xLow) / m_cell)) - 1);
        const int bi1 = (std::min)(m_nx - 1, static_cast<int>(std::floor((maxX - m_xLow) / m_cell)) + 1);
        const int bj0 = (std::max)(0, static_cast<int>(std::floor((minY - m_yLow) / m_cell)) - 1);
        const int bj1 = (std::min)(m_ny - 1, static_cast<int>(std::floor((maxY - m_yLow) / m_cell)) + 1);
        if (bi0 > bi1 || bj0 > bj1)
            return;
        const int w = bi1 - bi0 + 1;
        const int h = bj1 - bj0 + 1;
        std::vector<unsigned char> face(static_cast<size_t>(w) * h, 0);

        for (size_t t = 0; t + 2 < triangles.size(); t += 3)
        {
            const Vec3& a = triangles[t];
            const Vec3& b = triangles[t + 1];
            const Vec3& c = triangles[t + 2];
            const double area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
            if (std::fabs(area) < 1e-12)
                continue;   // edge on from above: it covers nothing
            const double tx0 = (std::min)({ a.x, b.x, c.x }), tx1 = (std::max)({ a.x, b.x, c.x });
            const double ty0 = (std::min)({ a.y, b.y, c.y }), ty1 = (std::max)({ a.y, b.y, c.y });
            const int i0 = (std::max)(bi0, static_cast<int>(std::floor((tx0 - m_xLow) / m_cell)));
            const int i1 = (std::min)(bi1, static_cast<int>(std::floor((tx1 - m_xLow) / m_cell)));
            const int j0 = (std::max)(bj0, static_cast<int>(std::floor((ty0 - m_yLow) / m_cell)));
            const int j1 = (std::min)(bj1, static_cast<int>(std::floor((ty1 - m_yLow) / m_cell)));
            for (int j = j0; j <= j1; ++j)
            {
                for (int i = i0; i <= i1; ++i)
                {
                    const double x = X(i), y = Y(j);
                    const double wa = ((b.x - x) * (c.y - y) - (c.x - x) * (b.y - y)) / area;
                    const double wb = ((c.x - x) * (a.y - y) - (a.x - x) * (c.y - y)) / area;
                    const double wc = 1.0 - wa - wb;
                    if (wa >= -1e-9 && wb >= -1e-9 && wc >= -1e-9)
                        face[static_cast<size_t>(j - bj0) * w + (i - bi0)] = 1;
                }
            }
        }

        // Its holes filled: whatever empty cell can't be reached from the box's edge.
        std::vector<unsigned char> outside(face.size(), 0);
        std::vector<int> stack;
        const auto push = [&](int x, int y) {
            const size_t q = static_cast<size_t>(y) * w + x;
            if (face[q] == 0 && outside[q] == 0)
            {
                outside[q] = 1;
                stack.push_back(static_cast<int>(q));
            }
        };
        for (int x = 0; x < w; ++x)
        {
            push(x, 0);
            push(x, h - 1);
        }
        for (int y = 0; y < h; ++y)
        {
            push(0, y);
            push(w - 1, y);
        }
        while (!stack.empty())
        {
            const int q = stack.back();
            stack.pop_back();
            const int x = q % w, y = q / w;
            if (x > 0)
                push(x - 1, y);
            if (x + 1 < w)
                push(x + 1, y);
            if (y > 0)
                push(x, y - 1);
            if (y + 1 < h)
                push(x, y + 1);
        }
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                if (outside[static_cast<size_t>(y) * w + x] == 0)
                    m_in[static_cast<size_t>(bj0 + y) * m_nx + (bi0 + x)] = 1;
            }
        }
    }

    void PlungeArea::Finish()
    {
        // The squared distance to the nearest outside cell, columns then rows.
        const double big = 1e12;     // more than any squared distance in cells
        std::vector<double> grid(m_in.size());
        m_count = 0;
        for (size_t q = 0; q < m_in.size(); ++q)
        {
            grid[q] = m_in[q] ? big : 0.0;
            m_count += m_in[q] ? 1 : 0;
        }
        const int longest = (std::max)(m_nx, m_ny);
        std::vector<double> f(static_cast<size_t>(longest)), d(static_cast<size_t>(longest)),
            z(static_cast<size_t>(longest) + 1);
        std::vector<int> v(static_cast<size_t>(longest));
        f.resize(static_cast<size_t>(m_ny));
        d.resize(static_cast<size_t>(m_ny));
        for (int i = 0; i < m_nx; ++i)
        {
            for (int j = 0; j < m_ny; ++j)
                f[static_cast<size_t>(j)] = grid[static_cast<size_t>(j) * m_nx + i];
            DistanceLine(f, d, v, z);
            for (int j = 0; j < m_ny; ++j)
                grid[static_cast<size_t>(j) * m_nx + i] = d[static_cast<size_t>(j)];
        }
        f.resize(static_cast<size_t>(m_nx));
        d.resize(static_cast<size_t>(m_nx));
        for (int j = 0; j < m_ny; ++j)
        {
            for (int i = 0; i < m_nx; ++i)
                f[static_cast<size_t>(i)] = grid[static_cast<size_t>(j) * m_nx + i];
            DistanceLine(f, d, v, z);
            for (int i = 0; i < m_nx; ++i)
                grid[static_cast<size_t>(j) * m_nx + i] = d[static_cast<size_t>(i)];
        }
        m_distance.resize(grid.size());
        for (size_t q = 0; q < grid.size(); ++q)
            m_distance[q] = static_cast<float>(std::sqrt(grid[q]) * m_cell);
    }

    double PlungeArea::Clearance(double x, double y) const
    {
        const int i = static_cast<int>(std::floor((x - m_xLow) / m_cell));
        const int j = static_cast<int>(std::floor((y - m_yLow) / m_cell));
        if (i < 0 || j < 0 || i >= m_nx || j >= m_ny || m_distance.empty())
            return -1.0;
        const size_t q = static_cast<size_t>(j) * m_nx + i;
        if (!m_in[q])
            return -1.0;
        // The edge is half a cell short of the outside cell's centre, and the
        // point may be anywhere in its own cell.
        return m_distance[q] - (0.5 + 0.7072) * m_cell;
    }

    bool PlungeArea::Extent(const Vec3& direction, double clearance, double& low, double& high) const
    {
        bool found = false;
        for (int j = 0; j < m_ny; ++j)
        {
            for (int i = 0; i < m_nx; ++i)
            {
                const size_t q = static_cast<size_t>(j) * m_nx + i;
                if (!m_in[q] || m_distance.empty() || m_distance[q] - (0.5 + 0.7072) * m_cell < clearance)
                    continue;
                const double value = X(i) * direction.x + Y(j) * direction.y;
                low = found ? (std::min)(low, value) : value;
                high = found ? (std::max)(high, value) : value;
                found = true;
            }
        }
        return found;
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

    std::vector<std::vector<Vec3>> PlungeRows(const PlungeArea& area, double reach, double pitch,
                                              double toolDiameter, const PlungeRoughingSettings& settings)
    {
        std::vector<std::vector<Vec3>> rows;
        if (area.Empty() || !(pitch > 0.0))
            return rows;

        const Vec3 along = PassDirection(settings.cutAngle);
        const Vec3 across = LeftOf(along);

        // Plunges a pitch of at least the tool apart mustn't overlap anywhere.
        const bool apart = pitch >= toolDiameter - 1e-9;

        // The rows span the cells far enough inside; each row runs the area's length.
        double low = 0.0, high = 0.0, uLow = 0.0, uHigh = 0.0;
        if (!area.Extent(across, reach, low, high) || !area.Extent(along, reach, uLow, uHigh))
            return rows;
        const double uMiddle = (uLow + uHigh) / 2.0;   // the grid is centred along the rows
        uLow -= area.Cell();
        uHigh += area.Cell();

        // Where each row is across the area.
        const double gap = pitch * std::sqrt(3.0) / 2.0;
        std::vector<double> vs;
        if (apart)
        {
            // Exactly the packing gap apart, centred.
            const int count = static_cast<int>(std::floor((high - low) / gap + 1e-9)) + 1;
            const double first = (low + high) / 2.0 - (count - 1) * gap / 2.0;
            for (int r = 0; r < count; ++r)
                vs.push_back(first + r * gap);
        }
        else
        {
            // Spread evenly from side to side, at most the gap apart.
            const int intervals = (high - low < 1e-9) ? 0 : static_cast<int>(std::ceil((high - low) / gap - 1e-9));
            const double spacing = (intervals > 0) ? (high - low) / intervals : 0.0;
            for (int r = 0; r <= intervals; ++r)
                vs.push_back((intervals == 0) ? (low + high) / 2.0 : low + r * spacing);
        }
        const double step = (std::min)(area.Cell() / 2.0, pitch / 8.0);

        // Each row's stretches where the tool fits, and the grid's points in them.
        std::vector<std::vector<std::pair<double, double>>> runsOf(vs.size());
        std::vector<std::vector<double>> us(vs.size());
        for (size_t r = 0; r < vs.size(); ++r)
        {
            const double v = vs[r];
            std::vector<std::pair<double, double>>& runs = runsOf[r];
            const int samples = (std::max)(2, static_cast<int>(std::ceil((uHigh - uLow) / step)) + 1);
            bool inRun = false;
            double runStart = 0.0, last = 0.0;
            for (int i = 0; i < samples; ++i)
            {
                const double u = uLow + (uHigh - uLow) * i / (samples - 1);
                const Vec3 p = along * u + across * v;
                const bool clear = area.Clearance(p.x, p.y) >= reach - 1e-9;
                if (clear && !inRun)
                {
                    runStart = u;
                    inRun = true;
                }
                else if (!clear && inRun)
                {
                    runs.push_back({ runStart, last });
                    inRun = false;
                }
                if (clear)
                    last = u;
            }
            if (inRun)
                runs.push_back({ runStart, last });

            // Alternate rows shifted half a pitch.
            const double base = uMiddle + ((r % 2 != 0) ? pitch / 2.0 : 0.0);
            for (const auto& run : runs)
            {
                const long long first = static_cast<long long>(std::ceil((run.first - base) / pitch - 1e-9));
                for (long long k = first; base + k * pitch <= run.second + 1e-9; ++k)
                    us[r].push_back(base + k * pitch);
            }
        }

        // The rows' ends, where the grid leaves room.
        const auto farEnough = [&](size_t r, double u) {
            if (!apart)
                return true;
            const Vec3 p = along * u + across * vs[r];
            for (size_t other = (r > 0 ? r - 1 : 0); other <= r + 1 && other < vs.size(); ++other)
            {
                for (double w : us[other])
                {
                    const Vec3 q = along * w + across * vs[other];
                    if (std::hypot(p.x - q.x, p.y - q.y) < toolDiameter - 1e-9)
                        return false;
                }
            }
            return true;
        };
        const double endGap = apart ? toolDiameter : pitch / 2.0;
        for (size_t r = 0; r < vs.size(); ++r)
        {
            for (const auto& run : runsOf[r])
            {
                // The grid's points in this run, if any.
                double nearestLow = run.second + pitch, nearestHigh = run.first - pitch;
                for (double u : us[r])
                {
                    if (u >= run.first - 1e-9 && u <= run.second + 1e-9)
                    {
                        nearestLow = (std::min)(nearestLow, u);
                        nearestHigh = (std::max)(nearestHigh, u);
                    }
                }
                const bool none = nearestLow > run.second;
                if ((none || nearestLow - run.first >= endGap - 1e-9) && farEnough(r, run.first))
                {
                    us[r].push_back(run.first);
                    if (none)
                        nearestHigh = run.first;
                }
                if (run.second - nearestHigh >= endGap - 1e-9 && run.second > run.first + 1e-9 &&
                    farEnough(r, run.second))
                    us[r].push_back(run.second);
            }
            std::sort(us[r].begin(), us[r].end());
        }

        for (size_t r = 0; r < vs.size(); ++r)
        {
            if (us[r].empty())
                continue;
            std::vector<Vec3> row;
            for (double u : us[r])
                row.push_back(along * u + across * vs[r]);
            rows.push_back(std::move(row));
        }

        // Start at Bottom starts on the frame's -Y side, as Pocketing's Linear Cuts do.
        const bool acrossGoesUp = std::fabs(across.y) > 1e-9 ? across.y > 0.0 : across.x < 0.0;
        const bool fromLow = (settings.start == PlungeStartSide::Bottom) == acrossGoesUp;
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
        if (input.outlines.empty() && input.faces.empty())
            return Failure("There's no area to rough. Pick faces, or edges making closed loops.");

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

        // The top of the material, and the floor where no part is under a plunge:
        // without a stock, the highest of the picked geometry.
        double level = 0.0;
        bool haveLevel = false;
        for (const ProfileChain& outline : input.outlines)
        {
            for (const PathVertex& vertex : outline.path)
            {
                level = haveLevel ? (std::max)(level, vertex.point.z) : vertex.point.z;
                haveLevel = true;
            }
        }
        for (const std::vector<Vec3>& face : input.faces)
        {
            for (const Vec3& point : face)
            {
                level = haveLevel ? (std::max)(level, point.z) : point.z;
                haveLevel = true;
            }
        }
        const double top = input.haveStockMaxZ ? input.stockMaxZ : input.havePartMaxZ ? input.partMaxZ : level;
        const double floor = input.haveStockMinZ ? input.stockMinZ : level;
        const double approach = top + settings.approach;
        if (approach > clearance + 1e-9)
        {
            return Failure("The clearance plane is below the approach height. Raise the clearance plane, or "
                           "shorten the approach.");
        }

        // The area: everything picked, projected onto the frame's XY plane and merged.
        const ToolProfile profile = ProfileOf(tool);
        const double reach = profile.radius + settings.stockToLeave;
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
        PlungeArea area;
        area.Reset(boxLowX, boxLowY, boxHighX, boxHighY, (std::min)(0.25, (std::max)(0.02, tool.diameter / 40.0)));
        for (const ProfileChain& outline : input.outlines)
            area.AddLoop(outline.path);
        for (const std::vector<Vec3>& face : input.faces)
            area.AddFace(face);
        area.Finish();
        if (area.Empty())
            return Failure("The picked geometry covers no area seen from above.");

        // The plunges, then the parts' heights under them.
        const std::vector<std::vector<Vec3>> rows = PlungeRows(area, reach, pitch, tool.diameter, settings);
        size_t count = 0;
        for (const auto& row : rows)
            count += row.size();
        if (count > kMostPlunges)
            return Failure("There would be too many plunges. Make the stepover larger.");
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
