// Boundary.cpp : turns picked outlines into one closed boundary, with arcs kept as arcs.

#include "Boundary.h"

#include <algorithm>
#include <cmath>

namespace camcore
{
    namespace
    {
        const double kPi = 3.14159265358979323846;
        const double kTwoPi = 2.0 * kPi;

        // How far an angle may stray outside an arc and still count as on it.
        const double kAngleTolerance = 1e-9;

        double DistanceXY(const Vec3& a, const Vec3& b)
        {
            return std::hypot(a.x - b.x, a.y - b.y);
        }

        double AngleOf(const Vec3& point, const Vec3& centre)
        {
            return std::atan2(point.y - centre.y, point.x - centre.x);
        }

        // An angle brought into [0, 2 pi).
        double Wrap(double angle)
        {
            angle = std::fmod(angle, kTwoPi);
            return (angle < 0.0) ? angle + kTwoPi : angle;
        }

        // True if the direction at angle from the centre lies on the arc.
        bool OnArc(double angle, double startAngle, double sweep)
        {
            const double turned = (sweep >= 0.0) ? Wrap(angle - startAngle) : Wrap(startAngle - angle);
            return turned <= std::fabs(sweep) + kAngleTolerance || turned >= kTwoPi - kAngleTolerance;
        }

        // An arc about any axis as points after its start, for arcs not about Z.
        Polyline ArcPoints3D(const Vec3& start, const Vec3& end, const Vec3& centre, const Vec3& axisIn)
        {
            Vec3 axis;
            if (!Normalise(axisIn, axis))
                return { end };

            // The angle from start to end turning anticlockwise about the axis.
            const Vec3 from = start - centre;
            const Vec3 to = end - centre;
            double sweep = std::atan2(Dot(Cross(from, to), axis), Dot(from, to));
            if (sweep <= 1e-12)
                sweep += kTwoPi;

            const int count = (std::max)(1, static_cast<int>(std::ceil(sweep / (kArcDrawingStepDegrees * kPi / 180.0))));
            Polyline points;
            for (int i = 1; i < count; ++i)
            {
                // Rodrigues' rotation of the start about the axis.
                const double t = sweep * i / count;
                const Vec3 turned = from * std::cos(t) + Cross(axis, from) * std::sin(t) +
                                    axis * (Dot(axis, from) * (1.0 - std::cos(t)));
                points.push_back(centre + turned);
            }
            points.push_back(end);
            return points;
        }

        // A piece turned round. Each segment's arc moves to the vertex it now
        // starts from, turning the other way.
        Path Reversed(const Path& piece, bool closed)
        {
            const size_t n = piece.size();
            Path result(n);
            for (size_t k = 0; k < n; ++k)
            {
                const size_t from = closed ? (n - k) % n : n - 1 - k;
                result[k].point = piece[from].point;

                // The segment k -> k+1 was the original one from the vertex after it.
                const bool hasNext = closed || k + 1 < n;
                if (!hasNext)
                    continue;
                const size_t owner = closed ? (n - k - 1) % n : n - 2 - k;
                if (piece[owner].arc)
                {
                    result[k].arc = true;
                    result[k].centre = piece[owner].centre;
                    result[k].axis = piece[owner].axis * -1.0;
                }
            }
            return result;
        }
    }

    Path PathFromPoints(const Polyline& points)
    {
        Path path;
        for (const Vec3& point : points)
        {
            PathVertex vertex;
            vertex.point = point;
            path.push_back(vertex);
        }
        return path;
    }

    double ArcSweep(const Vec3& start, const Vec3& end, const Vec3& centre, bool anticlockwise)
    {
        double sweep = AngleOf(end, centre) - AngleOf(start, centre);
        if (anticlockwise)
        {
            while (sweep <= 1e-12)
                sweep += kTwoPi;
            while (sweep > kTwoPi + 1e-12)
                sweep -= kTwoPi;
        }
        else
        {
            while (sweep >= -1e-12)
                sweep -= kTwoPi;
            while (sweep < -kTwoPi - 1e-12)
                sweep += kTwoPi;
        }
        return sweep;
    }

    Polyline ArcPoints(const Vec3& start, const Vec3& end, const Vec3& centre, bool anticlockwise)
    {
        const double sweep = ArcSweep(start, end, centre, anticlockwise);
        const double radius = DistanceXY(start, centre);
        const double startAngle = AngleOf(start, centre);
        const int count =
            (std::max)(1, static_cast<int>(std::ceil(std::fabs(sweep) / (kArcDrawingStepDegrees * kPi / 180.0))));

        Polyline points;
        for (int i = 1; i < count; ++i)
        {
            const double t = static_cast<double>(i) / count;
            const double angle = startAngle + sweep * t;
            points.push_back({ centre.x + radius * std::cos(angle), centre.y + radius * std::sin(angle),
                               start.z + (end.z - start.z) * t });
        }
        points.push_back(end);
        return points;
    }

    Polyline PathPoints(const Path& path, bool closed)
    {
        Polyline points;
        if (path.empty())
            return points;

        points.push_back(path.front().point);
        const size_t segments = closed ? path.size() : path.size() - 1;
        for (size_t i = 0; i < segments; ++i)
        {
            const PathVertex& from = path[i];
            const Vec3& to = path[(i + 1) % path.size()].point;
            if (from.arc)
            {
                const Polyline arc = ArcPoints(from.point, to, from.centre, ArcIsAnticlockwise(from));
                points.insert(points.end(), arc.begin(), arc.end());
            }
            else
            {
                points.push_back(to);
            }
        }
        return points;
    }

    void PathExtent(const Path& path, const Vec3& direction, double& low, double& high)
    {
        low = high = 0.0;
        bool found = false;
        auto take = [&](const Vec3& point) {
            const double value = point.x * direction.x + point.y * direction.y;
            low = found ? (std::min)(low, value) : value;
            high = found ? (std::max)(high, value) : value;
            found = true;
        };

        for (size_t i = 0; i < path.size(); ++i)
        {
            const PathVertex& vertex = path[i];
            take(vertex.point);
            if (!vertex.arc)
                continue;

            // An arc reaches farthest along the direction, either way, where it
            // points straight along it from the centre.
            const Vec3& end = path[(i + 1) % path.size()].point;
            const double radius = DistanceXY(vertex.point, vertex.centre);
            const double startAngle = AngleOf(vertex.point, vertex.centre);
            const double sweep = ArcSweep(vertex.point, end, vertex.centre, ArcIsAnticlockwise(vertex));
            const double facing = std::atan2(direction.y, direction.x);
            for (const double angle : { facing, facing + kPi })
            {
                if (OnArc(angle, startAngle, sweep))
                    take({ vertex.centre.x + radius * std::cos(angle), vertex.centre.y + radius * std::sin(angle), 0.0 });
            }
        }
    }

    std::vector<double> PathCrossings(const Path& path, const Vec3& along, const Vec3& across, double v)
    {
        std::vector<double> crossings;
        auto alongOf = [&](const Vec3& p) { return p.x * along.x + p.y * along.y; };
        auto acrossOf = [&](const Vec3& p) { return p.x * across.x + p.y * across.y; };

        for (size_t i = 0; i < path.size(); ++i)
        {
            const PathVertex& vertex = path[i];
            const Vec3& p = vertex.point;
            const Vec3& q = path[(i + 1) % path.size()].point;

            if (!vertex.arc)
            {
                // Each line counts from its lower end up to, but not at, its upper end.
                const double pv = acrossOf(p), qv = acrossOf(q);
                if (!((pv <= v && v < qv) || (qv <= v && v < pv)))
                    continue;
                const double pu = alongOf(p), qu = alongOf(q);
                crossings.push_back(pu + (v - pv) * (qu - pu) / (qv - pv));
                continue;
            }

            // The circle meets the line where it's v across, then only the part on the arc counts.
            const double radius = DistanceXY(p, vertex.centre);
            const double cu = alongOf(vertex.centre), cv = acrossOf(vertex.centre);
            const double off = v - cv;
            if (std::fabs(off) > radius)
                continue;
            const double half = std::sqrt((std::max)(0.0, radius * radius - off * off));
            const double startAngle = AngleOf(p, vertex.centre);
            const double sweep = ArcSweep(p, q, vertex.centre, ArcIsAnticlockwise(vertex));
            for (const double u : { cu - half, cu + half })
            {
                const Vec3 at{ u * along.x + v * across.x, u * along.y + v * across.y, 0.0 };
                if (OnArc(AngleOf(at, vertex.centre), startAngle, sweep))
                    crossings.push_back(u);
            }
        }
        return crossings;
    }

    double SignedArea(const Polyline& polygon)
    {
        double twice = 0.0;
        for (size_t i = 0; i < polygon.size(); ++i)
        {
            const Vec3& a = polygon[i];
            const Vec3& b = polygon[(i + 1) % polygon.size()];
            twice += a.x * b.y - b.x * a.y;
        }
        return twice / 2.0;
    }

    double SignedArea(const Path& path)
    {
        // The polygon through the vertices, plus the sliver between each arc and its chord.
        double area = 0.0;
        for (size_t i = 0; i < path.size(); ++i)
        {
            const PathVertex& vertex = path[i];
            const Vec3& a = vertex.point;
            const Vec3& b = path[(i + 1) % path.size()].point;
            area += (a.x * b.y - b.x * a.y) / 2.0;
            if (vertex.arc)
            {
                const double radius = DistanceXY(a, vertex.centre);
                const double sweep = ArcSweep(a, b, vertex.centre, ArcIsAnticlockwise(vertex));
                area += radius * radius * (sweep - std::sin(sweep)) / 2.0;
            }
        }
        return area;
    }

    Path ClosePath(const std::vector<Path>& pieces, double level)
    {
        std::vector<Path> remaining;
        for (const Path& piece : pieces)
        {
            if (!piece.empty())
                remaining.push_back(piece);
        }

        Path chain;
        if (remaining.empty())
            return chain;

        chain = remaining.front();
        remaining.erase(remaining.begin());

        // Each time, the piece with an end nearest the chain's end comes next.
        while (!remaining.empty())
        {
            const Vec3& end = chain.back().point;
            size_t best = 0;
            bool reverse = false;
            double bestDistance = -1.0;
            for (size_t i = 0; i < remaining.size(); ++i)
            {
                const double toStart = DistanceXY(end, remaining[i].front().point);
                const double toEnd = DistanceXY(end, remaining[i].back().point);
                if (bestDistance < 0.0 || toStart < bestDistance)
                {
                    best = i;
                    reverse = false;
                    bestDistance = toStart;
                }
                if (toEnd < bestDistance)
                {
                    best = i;
                    reverse = true;
                    bestDistance = toEnd;
                }
            }

            Path next = remaining[best];
            remaining.erase(remaining.begin() + static_cast<ptrdiff_t>(best));
            if (reverse)
                next = Reversed(next, false);

            // A gap becomes a straight line, as the chain's end goes nowhere yet.
            chain.back().arc = false;
            chain.insert(chain.end(), next.begin(), next.end());
        }
        chain.back().arc = false;      // closing back to the start is a straight line

        // Flatten onto the level. Arcs not about Z become straight segments.
        Path flat;
        for (size_t i = 0; i < chain.size(); ++i)
        {
            PathVertex vertex = chain[i];
            vertex.point.z = level;
            vertex.centre.z = level;
            const double axisLength = Length(vertex.axis);
            const bool aboutZ = axisLength > 0.0 && std::fabs(vertex.axis.z) > axisLength * (1.0 - 1e-6);
            if (vertex.arc && !aboutZ && i + 1 < chain.size())
            {
                const Polyline points = ArcPoints3D(chain[i].point, chain[i + 1].point, chain[i].centre, vertex.axis);
                vertex.arc = false;
                flat.push_back(vertex);
                for (size_t p = 0; p + 1 < points.size(); ++p)
                {
                    PathVertex step;
                    step.point = { points[p].x, points[p].y, level };
                    flat.push_back(step);
                }
                continue;
            }
            if (vertex.arc)
                vertex.axis = { 0.0, 0.0, vertex.axis.z > 0.0 ? 1.0 : -1.0 };
            flat.push_back(vertex);
        }

        // Drop points that repeat the one before. The later one says how the
        // path goes on, so its arc is kept.
        Path path;
        for (const PathVertex& vertex : flat)
        {
            if (!path.empty() && DistanceXY(path.back().point, vertex.point) <= kBoundaryJoinTolerance)
            {
                path.back().arc = vertex.arc;
                path.back().centre = vertex.centre;
                path.back().axis = vertex.axis;
                continue;
            }
            path.push_back(vertex);
        }

        // Closing back to the start is implied, so a repeated start goes.
        while (path.size() > 1 && DistanceXY(path.back().point, path.front().point) <= kBoundaryJoinTolerance)
            path.pop_back();

        // Two points can enclose an area only with an arc between them.
        const bool anyArc = std::any_of(path.begin(), path.end(), [](const PathVertex& v) { return v.arc; });
        if (path.size() < 2 || (path.size() < 3 && !anyArc) || std::fabs(SignedArea(path)) < 1e-9)
            return Path();

        if (SignedArea(path) < 0.0)
            path = Reversed(path, true);
        return path;
    }

    Polyline ClosePolygon(const std::vector<Polyline>& pieces, double level)
    {
        std::vector<Path> paths;
        for (const Polyline& piece : pieces)
            paths.push_back(PathFromPoints(piece));

        Polyline polygon;
        for (const PathVertex& vertex : ClosePath(paths, level))
            polygon.push_back(vertex.point);
        return polygon;
    }
}
