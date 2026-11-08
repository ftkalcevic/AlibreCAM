// ArcFit.cpp : turns runs of short straight moves that follow a circle into arc moves.

#include "ArcFit.h"

#include <algorithm>
#include <cmath>

namespace camcore
{
    std::vector<std::string> ValidateArcFitting(const ArcFitting& fitting)
    {
        std::vector<std::string> problems;
        if (!(fitting.tolerance > 0.0 && fitting.tolerance <= 1.0))
            problems.push_back("The arc tolerance must be more than 0 and at most 1 mm.");
        return problems;
    }

    namespace
    {
        const double kPi = 3.14159265358979323846;
        const double kSamePlane = 1e-6;
        const double kSmallestRadius = 0.01;
        const double kLargestRadius = 1000.0;
        const size_t kFewestMoves = 3;

        struct Circle
        {
            double cu = 0.0, cv = 0.0, radius = 0.0, sweep = 0.0;
        };

        // The circle through three points in a plane, if they aren't in a line.
        bool CircleThrough(double au, double av, double bu, double bv, double cu, double cv, Circle& circle)
        {
            const double d = 2.0 * (au * (bv - cv) + bu * (cv - av) + cu * (av - bv));
            if (std::fabs(d) < 1e-12)
                return false;
            const double a2 = au * au + av * av, b2 = bu * bu + bv * bv, c2 = cu * cu + cv * cv;
            circle.cu = (a2 * (bv - cv) + b2 * (cv - av) + c2 * (av - bv)) / d;
            circle.cv = (a2 * (cu - bu) + b2 * (au - cu) + c2 * (bu - au)) / d;
            circle.radius = std::hypot(au - circle.cu, av - circle.cv);
            return true;
        }

        // The circle through the first and last points that best fits those
        // between (least squares): its centre is on the ends' perpendicular bisector,
        // at the t where 2 (a - p).c = |a|^2 - |p|^2 holds best for each point p.
        bool CircleThroughEnds(const std::vector<double>& u, const std::vector<double>& v, Circle& circle)
        {
            const size_t n = u.size() - 1;
            const double au = u[0], av = v[0], bu = u[n], bv = v[n];
            const double du = bu - au, dv = bv - av;
            const double length = std::hypot(du, dv);
            if (length < 1e-9)
            {
                // Back where it started: three points will do.
                return n >= 2 && CircleThrough(au, av, u[n / 3], v[n / 3], u[2 * n / 3], v[2 * n / 3], circle);
            }
            const double nu = -dv / length, nv = du / length;
            const double mu = 0.5 * (au + bu), mv = 0.5 * (av + bv);
            double top = 0.0, bottom = 0.0;
            for (size_t k = 1; k < n; ++k)
            {
                const double eu = au - u[k], ev = av - v[k];
                const double a = 2.0 * (eu * nu + ev * nv);
                const double b = (au * au + av * av) - (u[k] * u[k] + v[k] * v[k]) - 2.0 * (eu * mu + ev * mv);
                top += a * b;
                bottom += a * a;
            }
            if (bottom < 1e-18)
                return false;   // in a line
            const double t = top / bottom;
            circle.cu = mu + t * nu;
            circle.cv = mv + t * nv;
            circle.radius = std::hypot(au - circle.cu, av - circle.cv);
            return true;
        }

        class Fitter
        {
        public:
            Fitter(const std::vector<Vec3>& points, const ArcFitting& fitting) : m_p(points), m_fitting(fitting) {}

            // Do points first to last (indexes) lie on one circle in the plane?
            bool Fits(size_t first, size_t last, ArcPlane plane, Circle& circle) const
            {
                if (last < first + kFewestMoves || last >= m_p.size())
                    return false;
                std::vector<double> u(last - first + 1), v(last - first + 1);
                double w0 = 0.0;
                for (size_t k = first; k <= last; ++k)
                {
                    double w = 0.0;
                    ToPlane(plane, m_p[k], u[k - first], v[k - first], w);
                    if (k == first)
                        w0 = w;
                    else if (std::fabs(w - w0) > kSamePlane)
                        return false;
                }
                const size_t n = last - first;
                if (!CircleThroughEnds(u, v, circle))
                    return false;
                if (circle.radius < kSmallestRadius || circle.radius > kLargestRadius)
                    return false;

                const double tolerance = m_fitting.tolerance;
                double sweep = 0.0;
                double previous = std::atan2(v[0] - circle.cv, u[0] - circle.cu);
                for (size_t k = 0; k <= n; ++k)
                {
                    if (std::fabs(std::hypot(u[k] - circle.cu, v[k] - circle.cv) - circle.radius) > tolerance)
                        return false;
                    if (k == 0)
                        continue;
                    // The move's nearest point to the centre (the ends are checked already).
                    {
                        const double du = u[k] - u[k - 1], dv = v[k] - v[k - 1];
                        const double length2 = du * du + dv * dv;
                        double t = 0.5;
                        if (length2 > 0.0)
                            t = (std::max)(0.0, (std::min)(1.0, ((circle.cu - u[k - 1]) * du + (circle.cv - v[k - 1]) * dv) / length2));
                        const double nu = u[k - 1] + du * t, nv = v[k - 1] + dv * t;
                        if (std::fabs(std::hypot(nu - circle.cu, nv - circle.cv) - circle.radius) > tolerance)
                            return false;
                    }
                    const double angle = std::atan2(v[k] - circle.cv, u[k] - circle.cu);
                    double delta = std::remainder(angle - previous, 2.0 * kPi);
                    previous = angle;
                    if (std::fabs(delta) < 1e-12)
                        return false;   // a move that doesn't go round
                    if (k > 1 && (delta > 0.0) != (sweep > 0.0))
                        return false;   // turning back
                    sweep += delta;
                }
                if (std::fabs(sweep) >= 2.0 * kPi - 1e-6)
                    return false;       // more than a whole turn
                circle.sweep = sweep;
                return true;
            }

            // The furthest point from first that fits in the plane, or first if none:
            // doubling the stretch while it fits, then halving the gap to the first that
            // doesn't. A short stretch's circle is poorly found when its points are a
            // little rough, so until one fits, longer ones are still tried.
            size_t Furthest(size_t first, ArcPlane plane, Circle& best) const
            {
                const size_t last = m_p.size() - 1;
                Circle circle;
                size_t good = first;
                size_t bad = 0;
                for (size_t length = kFewestMoves;; length *= 2)
                {
                    if (first + kFewestMoves > last)
                        return first;
                    const size_t to = (std::min)(first + length, last);
                    if (Fits(first, to, plane, circle))
                    {
                        good = to;
                        best = circle;
                        if (to == last)
                            return good;
                    }
                    else if (good != first || length >= 32 * kFewestMoves || to == last)
                    {
                        bad = to;
                        break;
                    }
                }
                if (good == first)
                    return first;
                while (bad > good + 1)
                {
                    const size_t mid = (good + bad) / 2;
                    if (Fits(first, mid, plane, circle))
                    {
                        good = mid;
                        best = circle;
                    }
                    else
                    {
                        bad = mid;
                    }
                }
                return good;
            }

        private:
            const std::vector<Vec3>& m_p;
            const ArcFitting& m_fitting;
        };

        // Arc moves from the run's point first to last round the circle, each no
        // more than a half turn, the last ending exactly on the run's point.
        void AddArcs(const std::vector<Vec3>& points, size_t first, size_t last, ArcPlane plane, const Circle& circle,
                     double feed, std::vector<ToolpathMove>& out)
        {
            double u0, v0, w;
            ToPlane(plane, points[first], u0, v0, w);
            const double start = std::atan2(v0 - circle.cv, u0 - circle.cu);
            const int pieces = (std::max)(1, static_cast<int>(std::ceil(std::fabs(circle.sweep) / kPi - 1e-9)));
            for (int k = 1; k <= pieces; ++k)
            {
                ToolpathMove move;
                move.kind = MoveKind::Feed;
                move.feed = feed;
                move.arc = true;
                move.plane = plane;
                move.clockwise = circle.sweep < 0.0;
                move.centre = FromPlane(plane, circle.cu, circle.cv, w);
                if (k == pieces)
                {
                    move.to = points[last];
                }
                else
                {
                    const double angle = start + circle.sweep * k / pieces;
                    move.to = FromPlane(plane, circle.cu + circle.radius * std::cos(angle),
                                        circle.cv + circle.radius * std::sin(angle), w);
                }
                out.push_back(move);
            }
        }
    }

    ArcFitCounts FitArcs(Toolpath& toolpath, const ArcFitting& fitting)
    {
        ArcFitCounts counts;
        counts.movesBefore = toolpath.moves.size();
        counts.movesAfter = counts.movesBefore;
        if (!fitting.enabled || !(fitting.xy || fitting.zx || fitting.yz) || toolpath.moves.size() < 2)
            return counts;

        std::vector<ArcPlane> planes;
        for (ArcPlane plane : { ArcPlane::XY, ArcPlane::ZX, ArcPlane::YZ })
        {
            if (fitting.Allows(plane))
                planes.push_back(plane);
        }

        const std::vector<ToolpathMove>& moves = toolpath.moves;
        std::vector<ToolpathMove> out;
        out.reserve(moves.size());
        out.push_back(moves.front());
        size_t m = 1;
        while (m < moves.size())
        {
            const ToolpathMove& move = moves[m];
            if (move.kind != MoveKind::Feed || move.arc)
            {
                out.push_back(move);
                ++m;
                continue;
            }
            // A run of straight feeds at one feed, from where the last move ended.
            std::vector<Vec3> points{ moves[m - 1].to };
            size_t end = m;
            while (end < moves.size() && moves[end].kind == MoveKind::Feed && !moves[end].arc &&
                   moves[end].feed == move.feed)
            {
                points.push_back(moves[end].to);
                ++end;
            }

            const Fitter fitter(points, fitting);
            size_t k = 0;
            while (k + 1 < points.size())
            {
                size_t furthest = k;
                ArcPlane bestPlane = ArcPlane::XY;
                Circle best;
                for (ArcPlane plane : planes)
                {
                    Circle circle;
                    const size_t reach = fitter.Furthest(k, plane, circle);
                    if (reach > furthest)
                    {
                        furthest = reach;
                        bestPlane = plane;
                        best = circle;
                    }
                }
                if (furthest > k)
                {
                    const size_t before = out.size();
                    AddArcs(points, k, furthest, bestPlane, best, move.feed, out);
                    counts.arcsMade += out.size() - before;
                    counts.linesReplaced += furthest - k;
                    k = furthest;
                }
                else
                {
                    out.push_back(moves[m + k]);
                    ++k;
                }
            }
            m = end;
        }
        counts.movesAfter = out.size();
        toolpath.moves = std::move(out);
        return counts;
    }

    namespace
    {
        double DistanceToSegment(const Vec3& p, const Vec3& a, const Vec3& b)
        {
            const Vec3 d = b - a;
            const double length2 = Dot(d, d);
            const double t = (length2 > 0.0) ? (std::max)(0.0, (std::min)(1.0, Dot(p - a, d) / length2)) : 0.0;
            return Length(p - (a + d * t));
        }

        void KeepFurthest(const std::vector<Vec3>& points, size_t first, size_t last, double tolerance,
                          std::vector<char>& keep)
        {
            // An explicit stack, as runs can be long.
            std::vector<std::pair<size_t, size_t>> stack{ { first, last } };
            while (!stack.empty())
            {
                const auto [a, b] = stack.back();
                stack.pop_back();
                if (b <= a + 1)
                    continue;
                double worst = -1.0;
                size_t at = a;
                for (size_t k = a + 1; k < b; ++k)
                {
                    const double d = DistanceToSegment(points[k], points[a], points[b]);
                    if (d > worst)
                    {
                        worst = d;
                        at = k;
                    }
                }
                if (worst > tolerance)
                {
                    keep[at] = 1;
                    stack.push_back({ a, at });
                    stack.push_back({ at, b });
                }
            }
        }
    }

    size_t SimplifyStraightRuns(Toolpath& toolpath, double tolerance)
    {
        const std::vector<ToolpathMove>& moves = toolpath.moves;
        if (moves.size() < 3 || !(tolerance > 0.0))
            return 0;
        std::vector<ToolpathMove> out;
        out.reserve(moves.size());
        out.push_back(moves.front());
        size_t m = 1;
        while (m < moves.size())
        {
            const ToolpathMove& move = moves[m];
            if (move.kind != MoveKind::Feed || move.arc)
            {
                out.push_back(move);
                ++m;
                continue;
            }
            std::vector<Vec3> points{ moves[m - 1].to };
            size_t end = m;
            while (end < moves.size() && moves[end].kind == MoveKind::Feed && !moves[end].arc &&
                   moves[end].feed == move.feed)
            {
                points.push_back(moves[end].to);
                ++end;
            }
            std::vector<char> keep(points.size(), 0);
            keep.back() = 1;
            KeepFurthest(points, 0, points.size() - 1, tolerance, keep);
            for (size_t k = 1; k < points.size(); ++k)
            {
                if (keep[k])
                    out.push_back(moves[m + k - 1]);
            }
            m = end;
        }
        const size_t removed = moves.size() - out.size();
        toolpath.moves = std::move(out);
        return removed;
    }
}
