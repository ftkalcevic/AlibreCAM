// DropCutter.cpp : how low a tool's tip can come down over the parts' triangles.

#include "DropCutter.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace camcore
{
    namespace
    {
        const double kLowest = std::numeric_limits<double>::lowest();
        const double kOtherFaceRange = 0.5;     // how far below the best another face is still reported

        // Where the point (x, y) is inside the triangle seen from above.
        bool InsideXY(const Vec3 p[3], double x, double y)
        {
            const auto side = [&](const Vec3& a, const Vec3& b) {
                return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
            };
            const double s0 = side(p[0], p[1]), s1 = side(p[1], p[2]), s2 = side(p[2], p[0]);
            const bool anyNegative = s0 < 0.0 || s1 < 0.0 || s2 < 0.0;
            const bool anyPositive = s0 > 0.0 || s1 > 0.0 || s2 > 0.0;
            return !(anyNegative && anyPositive);
        }
    }

    DropCutter::DropCutter(const std::vector<Vec3>& triangles, const ToolProfile& profile, double stockToLeave,
                           const std::vector<int>& faces)
        : m_profile(profile), m_s(stockToLeave), m_reach(profile.radius + stockToLeave), m_haveFaces(!faces.empty())
    {
        bool any = false;
        double xHigh = 0.0, yHigh = 0.0;
        for (size_t i = 0; i + 2 < triangles.size(); i += 3)
        {
            Triangle t;
            t.face = (i / 3 < faces.size()) ? faces[i / 3] : -1;
            for (int k = 0; k < 3; ++k)
                t.p[k] = triangles[i + k];
            t.lowX = (std::min)({ t.p[0].x, t.p[1].x, t.p[2].x });
            t.highX = (std::max)({ t.p[0].x, t.p[1].x, t.p[2].x });
            t.lowY = (std::min)({ t.p[0].y, t.p[1].y, t.p[2].y });
            t.highY = (std::max)({ t.p[0].y, t.p[1].y, t.p[2].y });
            t.highZ = (std::max)({ t.p[0].z, t.p[1].z, t.p[2].z });
            const Vec3 u = t.p[1] - t.p[0], v = t.p[2] - t.p[0];
            const double nz = u.x * v.y - u.y * v.x;
            t.flat = std::fabs(nz) > 1e-12;
            t.z0 = t.gx = t.gy = 0.0;
            if (t.flat)
            {
                const double nx = u.y * v.z - u.z * v.y, ny = u.z * v.x - u.x * v.z;
                t.gx = -nx / nz;
                t.gy = -ny / nz;
                t.z0 = t.p[0].z - t.gx * t.p[0].x - t.gy * t.p[0].y;
            }
            m_xLow = any ? (std::min)(m_xLow, t.lowX) : t.lowX;
            m_yLow = any ? (std::min)(m_yLow, t.lowY) : t.lowY;
            xHigh = any ? (std::max)(xHigh, t.highX) : t.highX;
            yHigh = any ? (std::max)(yHigh, t.highY) : t.highY;
            any = true;
            m_triangles.push_back(t);
        }
        if (!any || !(m_reach > 0.0))
            return;
        m_bucket = (std::max)(m_reach, 0.5);
        m_nx = static_cast<int>(std::floor((xHigh - m_xLow) / m_bucket)) + 1;
        m_ny = static_cast<int>(std::floor((yHigh - m_yLow) / m_bucket)) + 1;
        // Not too many buckets for a huge mesh and a tiny tool.
        while (static_cast<double>(m_nx) * m_ny > 4e6)
        {
            m_bucket *= 2.0;
            m_nx = static_cast<int>(std::floor((xHigh - m_xLow) / m_bucket)) + 1;
            m_ny = static_cast<int>(std::floor((yHigh - m_yLow) / m_bucket)) + 1;
        }
        m_buckets.assign(static_cast<size_t>(m_nx) * m_ny, {});
        for (size_t i = 0; i < m_triangles.size(); ++i)
        {
            const Triangle& t = m_triangles[i];
            const int i0 = static_cast<int>(std::floor((t.lowX - m_xLow) / m_bucket));
            const int i1 = static_cast<int>(std::floor((t.highX - m_xLow) / m_bucket));
            const int j0 = static_cast<int>(std::floor((t.lowY - m_yLow) / m_bucket));
            const int j1 = static_cast<int>(std::floor((t.highY - m_yLow) / m_bucket));
            for (int j = j0; j <= j1; ++j)
            {
                for (int k = i0; k <= i1; ++k)
                    m_buckets[static_cast<size_t>(j) * m_nx + k].push_back(static_cast<int>(i));
            }
        }
    }

    double DropCutter::Rise(double d) const
    {
        double h = m_profile.HeightForRadius((std::max)(0.0, d - m_s));
        if (h < 0.0)
            h = m_profile.length;   // past a tool shorter than its shape
        return h - m_s;
    }

    // The distance from the axis where the tool's side slopes as steeply as a
    // plane of this slope: where the plane touches it. The tool is convex, so
    // its slope only grows outwards.
    double DropCutter::ContactDistance(double slope) const
    {
        const auto steepness = [&](double d) {
            const double e = (std::max)(1e-7, m_reach * 1e-6);
            const double a = (std::max)(0.0, d - e), b = (std::min)(m_reach, d + e);
            return (Rise(b) - Rise(a)) / (b - a);
        };
        if (steepness(0.0) >= slope)
            return 0.0;
        if (steepness(m_reach) < slope)
            return m_reach;
        double low = 0.0, high = m_reach;
        for (int k = 0; k < 50; ++k)
        {
            const double mid = 0.5 * (low + high);
            if (steepness(mid) < slope)
                low = mid;
            else
                high = mid;
        }
        return 0.5 * (low + high);
    }

    // The highest the tip must be for the segment a-b: its part within reach,
    // where z less the tool's rise is highest. That's concave along the segment
    // (the rise is convex in the distance), so a golden section search finds it.
    double DropCutter::AlongEdge(const Vec3& a, const Vec3& b, double x, double y) const
    {
        const double ex = b.x - a.x, ey = b.y - a.y;
        const double fx = a.x - x, fy = a.y - y;
        const double qa = ex * ex + ey * ey;
        double t0 = 0.0, t1 = 1.0;
        if (qa < 1e-18)
        {
            // Straight up and down: its top end only.
            const double d = std::hypot(fx, fy);
            return (d <= m_reach) ? (std::max)(a.z, b.z) - Rise(d) : kLowest;
        }
        const double qb = 2.0 * (fx * ex + fy * ey), qc = fx * fx + fy * fy - m_reach * m_reach;
        const double disc = qb * qb - 4.0 * qa * qc;
        if (disc < 0.0)
            return kLowest;
        const double root = std::sqrt(disc);
        t0 = (std::max)(0.0, (-qb - root) / (2.0 * qa));
        t1 = (std::min)(1.0, (-qb + root) / (2.0 * qa));
        if (t0 > t1)
            return kLowest;
        const auto value = [&](double t) {
            const double d = (std::min)(m_reach, std::hypot(fx + ex * t, fy + ey * t));
            return a.z + (b.z - a.z) * t - Rise(d);
        };
        const double golden = 0.6180339887498949;
        double lo = t0, hi = t1;
        double m1 = hi - golden * (hi - lo), m2 = lo + golden * (hi - lo);
        double v1 = value(m1), v2 = value(m2);
        for (int k = 0; k < 40 && hi - lo > 1e-9; ++k)
        {
            if (v1 < v2)
            {
                lo = m1;
                m1 = m2;
                v1 = v2;
                m2 = lo + golden * (hi - lo);
                v2 = value(m2);
            }
            else
            {
                hi = m2;
                m2 = m1;
                v2 = v1;
                m1 = hi - golden * (hi - lo);
                v1 = value(m1);
            }
        }
        return (std::max)({ value(t0), value(t1), v1, v2 });
    }

    double DropCutter::Against(const Triangle& t, double x, double y) const
    {
        double best = kLowest;
        // Its face: the plane touches the tool where the tool slopes as steeply,
        // straight uphill from the axis, if that's on the triangle.
        if (t.flat)
        {
            const double slope = std::hypot(t.gx, t.gy);
            double cx = x, cy = y, d = 0.0;
            if (slope > 1e-12)
            {
                d = ContactDistance(slope);
                cx = x + t.gx / slope * d;
                cy = y + t.gy / slope * d;
            }
            if (InsideXY(t.p, cx, cy))
                best = (std::max)(best, t.z0 + t.gx * cx + t.gy * cy - Rise(d));
        }
        // Its edges and corners.
        for (int k = 0; k < 3; ++k)
            best = (std::max)(best, AlongEdge(t.p[k], t.p[(k + 1) % 3], x, y));
        return best;
    }

    bool DropCutter::TipAt(double x, double y, double& tip) const
    {
        Contact contact;
        if (!ContactAt(x, y, contact))
            return false;
        tip = contact.tip;
        return true;
    }

    bool DropCutter::ContactAt(double x, double y, Contact& contact) const
    {
        if (m_buckets.empty())
            return false;
        const int i0 = (std::max)(0, static_cast<int>(std::floor((x - m_reach - m_xLow) / m_bucket)));
        const int i1 = (std::min)(m_nx - 1, static_cast<int>(std::floor((x + m_reach - m_xLow) / m_bucket)));
        const int j0 = (std::max)(0, static_cast<int>(std::floor((y - m_reach - m_yLow) / m_bucket)));
        const int j1 = (std::min)(m_ny - 1, static_cast<int>(std::floor((y + m_reach - m_yLow) / m_bucket)));
        if (i0 > i1 || j0 > j1)
            return false;

        // Each triangle once, nearest the top first would be best; a quick bound
        // (its highest point less the tool's rise at its box's nearest) skips most.
        thread_local std::vector<unsigned> seen;
        thread_local unsigned tick = 0;
        if (seen.size() < m_triangles.size())
            seen.assign(m_triangles.size(), 0);
        if (++tick == 0)
        {
            std::fill(seen.begin(), seen.end(), 0);
            tick = 1;
        }
        struct Near
        {
            int index;
            double bound;
        };
        thread_local std::vector<Near> near;
        near.clear();
        for (int j = j0; j <= j1; ++j)
        {
            for (int i = i0; i <= i1; ++i)
            {
                for (int index : m_buckets[static_cast<size_t>(j) * m_nx + i])
                {
                    if (seen[static_cast<size_t>(index)] == tick)
                        continue;
                    seen[static_cast<size_t>(index)] = tick;
                    const Triangle& t = m_triangles[static_cast<size_t>(index)];
                    const double dx = (std::max)({ 0.0, t.lowX - x, x - t.highX });
                    const double dy = (std::max)({ 0.0, t.lowY - y, y - t.highY });
                    const double d = std::hypot(dx, dy);
                    if (d > m_reach)
                        continue;
                    near.push_back({ index, t.highZ - Rise(d) });
                }
            }
        }
        if (near.empty())
            return false;
        std::sort(near.begin(), near.end(), [](const Near& a, const Near& b) { return a.bound > b.bound; });
        // The best, its face, and the best on any other face.
        double best = kLowest, other = kLowest;
        int face = -1;
        // Another face matters only within the half millimetre a refined contact may
        // move; without faces, only the best does.
        for (const Near& n : near)
        {
            const double needed = m_haveFaces ? (std::max)(other, best - kOtherFaceRange) : best;
            if (n.bound <= needed)
                break;
            const Triangle& t = m_triangles[static_cast<size_t>(n.index)];
            const double value = Against(t, x, y);
            if (value == kLowest)
                continue;
            if (best != kLowest && t.face == face)
            {
                best = (std::max)(best, value);
            }
            else if (value > best)
            {
                other = best;
                best = value;
                face = t.face;
            }
            else
            {
                other = (std::max)(other, value);
            }
        }
        if (best == kLowest)
            return false;
        contact.tip = best;
        contact.face = face;
        contact.others = other;
        return true;
    }
}
