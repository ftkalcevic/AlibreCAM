// ProfileToolpath.cpp : calculates a Profiling operation's toolpath.

#include "ProfileToolpath.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "PathBuilder.h"

namespace camcore
{
    namespace
    {
        const double kPi = 3.14159265358979323846;
        const double kTwoPi = 2.0 * kPi;

        // Lengths shorter than this are nothing.
        const double kTiny = 1e-9;

        // Offset pieces this much nearer the profile than the offset still count as clear of it.
        const double kClearTolerance = 1e-4;

        ToolpathResult Failure(const std::string& problem)
        {
            ToolpathResult result;
            result.problem = problem;
            return result;
        }

        double CrossXY(const Vec3& a, const Vec3& b) { return a.x * b.y - a.y * b.x; }
        double DistanceXY(const Vec3& a, const Vec3& b) { return std::hypot(a.x - b.x, a.y - b.y); }

        Vec3 UnitXY(const Vec3& v)
        {
            const double length = std::hypot(v.x, v.y);
            return (length > kTiny) ? Vec3{ v.x / length, v.y / length, 0.0 } : Vec3{ 1.0, 0.0, 0.0 };
        }

        Vec3 LeftOf(const Vec3& direction) { return { -direction.y, direction.x, 0.0 }; }

        double Wrap(double angle)
        {
            angle = std::fmod(angle, kTwoPi);
            return (angle < 0.0) ? angle + kTwoPi : angle;
        }

        // A straight line or an arc in XY, from a to b.
        struct Seg
        {
            Vec3 a, b;
            bool arc = false;
            Vec3 c;
            bool ccw = true;
        };

        double Radius(const Seg& s) { return DistanceXY(s.a, s.c); }
        double Sweep(const Seg& s) { return ArcSweep(s.a, s.b, s.c, s.ccw); }
        double StartAngle(const Seg& s) { return std::atan2(s.a.y - s.c.y, s.a.x - s.c.x); }

        double SegLength(const Seg& s)
        {
            return s.arc ? std::fabs(Sweep(s)) * Radius(s) : DistanceXY(s.a, s.b);
        }

        Vec3 PointAt(const Seg& s, double t)
        {
            if (!s.arc)
                return { s.a.x + (s.b.x - s.a.x) * t, s.a.y + (s.b.y - s.a.y) * t, s.a.z };
            if (t <= 0.0)
                return s.a;
            if (t >= 1.0)
                return s.b;
            const double angle = StartAngle(s) + Sweep(s) * t;
            const double r = Radius(s);
            return { s.c.x + r * std::cos(angle), s.c.y + r * std::sin(angle), s.a.z };
        }

        Vec3 TangentAt(const Seg& s, double t)
        {
            if (!s.arc)
                return UnitXY(s.b - s.a);
            const Vec3 radial = UnitXY(PointAt(s, t) - s.c);
            return s.ccw ? Vec3{ -radial.y, radial.x, 0.0 } : Vec3{ radial.y, -radial.x, 0.0 };
        }

        Seg Reverse(const Seg& s)
        {
            Seg r = s;
            std::swap(r.a, r.b);
            r.ccw = !s.ccw;
            return r;
        }

        Seg SubSeg(const Seg& s, double t0, double t1)
        {
            Seg r = s;
            r.a = PointAt(s, t0);
            r.b = PointAt(s, t1);
            return r;
        }

        // Where point p lies along an arc, as a share of it. False if it's off the arc.
        bool ArcFraction(const Seg& s, const Vec3& p, double& t)
        {
            const double sweep = Sweep(s);
            const double angle = std::atan2(p.y - s.c.y, p.x - s.c.x);
            const double turned = (sweep >= 0.0) ? Wrap(angle - StartAngle(s)) : Wrap(StartAngle(s) - angle);
            const double tolerance = 1e-9;
            if (turned <= std::fabs(sweep) + tolerance)
            {
                t = (std::min)(1.0, turned / std::fabs(sweep));
                return true;
            }
            if (turned >= kTwoPi - tolerance)
            {
                t = 0.0;
                return true;
            }
            return false;
        }

        // The distance from p to the segment, and where along it the nearest point is.
        double NearestOn(const Seg& s, const Vec3& p, double& t)
        {
            if (!s.arc)
            {
                const Vec3 d = s.b - s.a;
                const double lengthSquared = d.x * d.x + d.y * d.y;
                t = (lengthSquared > 0.0) ? ((p.x - s.a.x) * d.x + (p.y - s.a.y) * d.y) / lengthSquared : 0.0;
                t = (std::max)(0.0, (std::min)(1.0, t));
                return DistanceXY(p, PointAt(s, t));
            }

            const Vec3 radial = UnitXY(p - s.c);
            const Vec3 onCircle{ s.c.x + radial.x * Radius(s), s.c.y + radial.y * Radius(s), 0.0 };
            if (DistanceXY(p, s.c) > kTiny && ArcFraction(s, onCircle, t))
                return std::fabs(DistanceXY(p, s.c) - Radius(s));

            const double toStart = DistanceXY(p, s.a), toEnd = DistanceXY(p, s.b);
            t = (toStart <= toEnd) ? 0.0 : 1.0;
            return (std::min)(toStart, toEnd);
        }

        double DistanceToSegs(const std::vector<Seg>& segs, const Vec3& p)
        {
            double best = -1.0;
            for (const Seg& s : segs)
            {
                double t = 0.0;
                const double d = NearestOn(s, p, t);
                if (best < 0.0 || d < best)
                    best = d;
            }
            return best;
        }

        // Where two segments cross, as shares of each.
        void Intersect(const Seg& s, const Seg& u, std::vector<std::pair<double, double>>& found)
        {
            const double e = 1e-9;
            if (!s.arc && !u.arc)
            {
                const Vec3 r = s.b - s.a, q = u.b - u.a;
                const double denominator = CrossXY(r, q);
                if (std::fabs(denominator) < 1e-15)
                    return;
                const Vec3 w = u.a - s.a;
                const double t = CrossXY(w, q) / denominator;
                const double v = CrossXY(w, r) / denominator;
                if (t >= -e && t <= 1.0 + e && v >= -e && v <= 1.0 + e)
                    found.push_back({ t, v });
                return;
            }

            if (s.arc && !u.arc)
            {
                std::vector<std::pair<double, double>> swapped;
                Intersect(u, s, swapped);
                for (const auto& pair : swapped)
                    found.push_back({ pair.second, pair.first });
                return;
            }

            if (!s.arc && u.arc)
            {
                // Along the line, where it's the arc's radius from its centre.
                const Vec3 r = s.b - s.a;
                const Vec3 f = s.a - u.c;
                const double a = r.x * r.x + r.y * r.y;
                const double b = 2.0 * (f.x * r.x + f.y * r.y);
                const double c = f.x * f.x + f.y * f.y - Radius(u) * Radius(u);
                const double discriminant = b * b - 4.0 * a * c;
                if (a < 1e-18 || discriminant < 0.0)
                    return;
                const double root = std::sqrt(discriminant);
                for (const double t : { (-b - root) / (2.0 * a), (-b + root) / (2.0 * a) })
                {
                    double v = 0.0;
                    if (t >= -e && t <= 1.0 + e && ArcFraction(u, PointAt(s, t), v))
                        found.push_back({ t, v });
                    if (root < 1e-12)
                        break;
                }
                return;
            }

            // Two arcs: where their circles meet, on both.
            const double r1 = Radius(s), r2 = Radius(u);
            const double d = DistanceXY(s.c, u.c);
            if (d < 1e-12 || d > r1 + r2 + 1e-9 || d < std::fabs(r1 - r2) - 1e-9)
                return;
            const double along = (r1 * r1 - r2 * r2 + d * d) / (2.0 * d);
            const double half = std::sqrt((std::max)(0.0, r1 * r1 - along * along));
            const Vec3 axis = UnitXY(u.c - s.c);
            const Vec3 middle{ s.c.x + axis.x * along, s.c.y + axis.y * along, 0.0 };
            for (const double sign : { -1.0, 1.0 })
            {
                const Vec3 p{ middle.x - axis.y * half * sign, middle.y + axis.x * half * sign, 0.0 };
                double t = 0.0, v = 0.0;
                if (ArcFraction(s, p, t) && ArcFraction(u, p, v))
                    found.push_back({ t, v });
                if (half < 1e-12)
                    break;
            }
        }

        std::vector<Seg> ToSegs(const Path& path, bool closed)
        {
            std::vector<Seg> segs;
            if (path.size() < 2)
                return segs;
            const size_t count = closed ? path.size() : path.size() - 1;
            for (size_t i = 0; i < count; ++i)
            {
                Seg s;
                s.a = path[i].point;
                s.b = path[(i + 1) % path.size()].point;
                if (path[i].arc)
                {
                    s.arc = true;
                    s.c = path[i].centre;
                    s.ccw = ArcIsAnticlockwise(path[i]);
                }
                if (s.arc || DistanceXY(s.a, s.b) > kTiny)
                    segs.push_back(s);
            }
            return segs;
        }

        Path ToPath(const std::vector<Seg>& segs, bool closed)
        {
            Path path;
            for (const Seg& s : segs)
            {
                PathVertex vertex;
                vertex.point = s.a;
                if (s.arc)
                {
                    vertex.arc = true;
                    vertex.centre = s.c;
                    vertex.axis = { 0.0, 0.0, s.ccw ? 1.0 : -1.0 };
                }
                path.push_back(vertex);
            }
            if (!closed && !segs.empty())
            {
                PathVertex end;
                end.point = segs.back().b;
                path.push_back(end);
            }
            return path;
        }

        // A segment moved sideways by distance, positive to its left. An arc that
        // shrinks to nothing has no offset.
        bool OffsetSeg(const Seg& s, double distance, Seg& offset)
        {
            offset = s;
            if (!s.arc)
            {
                const Vec3 left = LeftOf(UnitXY(s.b - s.a));
                offset.a = s.a + left * distance;
                offset.b = s.b + left * distance;
                return true;
            }

            // Left of an anticlockwise arc is towards its centre.
            const double radius = s.ccw ? Radius(s) - distance : Radius(s) + distance;
            if (!(radius > 1e-9))
                return false;
            offset.a = s.c + UnitXY(s.a - s.c) * radius;
            offset.b = s.c + UnitXY(s.b - s.c) * radius;
            offset.a.z = s.a.z;
            offset.b.z = s.b.z;
            return true;
        }

        // The offset with the joints filled in: arcs round outside corners, and
        // straight lines elsewhere, which are trimmed away later as they come too close.
        std::vector<Seg> RawOffset(const std::vector<Seg>& segs, bool closed, double distance)
        {
            std::vector<Seg> raw;
            std::vector<size_t> source;     // the profile segment each offset came from
            for (size_t i = 0; i < segs.size(); ++i)
            {
                Seg offset;
                if (OffsetSeg(segs[i], distance, offset))
                {
                    raw.push_back(offset);
                    source.push_back(i);
                }
            }
            if (raw.empty())
                return raw;

            std::vector<Seg> joined;
            auto join = [&](size_t from, size_t to) {
                const Seg& a = raw[from];
                const Seg& b = raw[to];
                if (DistanceXY(a.b, b.a) < kTiny)
                    return;

                // Next to each other on the profile, they meet at a corner.
                const size_t i = source[from], j = source[to];
                const bool adjacent = (j == i + 1) || (closed && i + 1 == segs.size() && j == 0);
                if (adjacent)
                {
                    const Vec3 corner = segs[i].b;
                    const double turn = CrossXY(TangentAt(segs[i], 1.0), TangentAt(segs[j], 0.0));
                    const bool outside = (distance > 0.0 && turn < 0.0) || (distance < 0.0 && turn > 0.0);
                    if (outside && std::fabs(DistanceXY(a.b, corner) - std::fabs(distance)) < 1e-6 &&
                        std::fabs(DistanceXY(b.a, corner) - std::fabs(distance)) < 1e-6)
                    {
                        Seg round;
                        round.a = a.b;
                        round.b = b.a;
                        round.arc = true;
                        round.c = corner;
                        round.ccw = turn > 0.0;
                        joined.push_back(round);
                        return;
                    }
                }
                Seg line;
                line.a = a.b;
                line.b = b.a;
                joined.push_back(line);
            };

            for (size_t k = 0; k < raw.size(); ++k)
            {
                joined.push_back(raw[k]);
                if (k + 1 < raw.size())
                    join(k, k + 1);
                else if (closed)
                    join(k, 0);
            }
            return joined;
        }
    }

    ProfileChain ChainProfile(const std::vector<std::vector<Path>>& pieces, double level)
    {
        ProfileChain chain;
        auto refuse = [&](const std::string& problem) {
            chain = ProfileChain();
            chain.problem = problem;
            return chain;
        };

        struct Piece
        {
            Path path;
            size_t feature;
        };
        std::vector<Piece> all;
        for (size_t f = 0; f < pieces.size(); ++f)
        {
            for (const Path& path : pieces[f])
            {
                if (path.size() >= 2)
                    all.push_back({ path, f });
            }
        }
        if (all.empty())
            return refuse("There's no profile to follow.");

        auto end = [&](size_t piece, int which) -> const Vec3& {
            return (which == 0) ? all[piece].path.front().point : all[piece].path.back().point;
        };

        // A closed edge is a profile on its own.
        for (size_t i = 0; i < all.size(); ++i)
        {
            if (DistanceXY(end(i, 0), end(i, 1)) > kProfileJoinTolerance)
                continue;
            if (all.size() > 1)
                return refuse("A closed edge can't join other edges in one profile.");
            chain.path = all[i].path;
            chain.path.pop_back();
            chain.closed = true;
        }

        if (!chain.closed)
        {
            // Each end joins at most one other end.
            const size_t n = all.size();
            std::vector<std::pair<long, int>> match(2 * n, { -1, 0 });
            for (size_t i = 0; i < n; ++i)
            {
                for (int e = 0; e < 2; ++e)
                {
                    int found = 0;
                    for (size_t j = 0; j < n; ++j)
                    {
                        for (int f = 0; f < 2; ++f)
                        {
                            if (j == i || DistanceXY(end(i, e), end(j, f)) > kProfileJoinTolerance)
                                continue;
                            match[2 * i + e] = { static_cast<long>(j), f };
                            ++found;
                        }
                    }
                    if (found > 1)
                        return refuse("Three or more edges meet at one point, so the profile branches.");
                }
            }

            std::vector<std::pair<size_t, int>> loose;
            for (size_t i = 0; i < n; ++i)
            {
                for (int e = 0; e < 2; ++e)
                {
                    if (match[2 * i + e].first < 0)
                        loose.push_back({ i, e });
                }
            }

            size_t start = 0;
            bool forward = true;
            if (loose.empty())
            {
                // Closed: from the first feature towards its neighbour that's earlier in the list.
                chain.closed = true;
                const size_t ahead = static_cast<size_t>(match[1].first);
                const size_t behind = static_cast<size_t>(match[0].first);
                forward = all[ahead].feature <= all[behind].feature;
            }
            else if (loose.size() == 2)
            {
                // Open: from the loose end of the earliest feature in the list.
                const auto& first = (all[loose[1].first].feature < all[loose[0].first].feature) ? loose[1] : loose[0];
                start = first.first;
                forward = (first.second == 0);
            }
            else
            {
                return refuse("The edges don't join end to end into one profile.");
            }

            std::vector<bool> used(n, false);
            size_t current = start;
            bool currentForward = forward;
            for (size_t step = 0; step < n; ++step)
            {
                used[current] = true;
                const Path piece = currentForward ? all[current].path : ReversePath(all[current].path, false);
                if (chain.path.empty())
                {
                    chain.path = piece;
                }
                else
                {
                    // The join keeps one point, which goes on as the new piece does.
                    PathVertex& joint = chain.path.back();
                    joint.arc = piece.front().arc;
                    joint.centre = piece.front().centre;
                    joint.axis = piece.front().axis;
                    chain.path.insert(chain.path.end(), piece.begin() + 1, piece.end());
                }

                const std::pair<long, int> next = match[2 * current + (currentForward ? 1 : 0)];
                if (next.first < 0 || used[static_cast<size_t>(next.first)])
                    break;
                current = static_cast<size_t>(next.first);
                currentForward = (next.second == 0);
            }

            if (std::find(used.begin(), used.end(), false) != used.end())
                return refuse("The edges don't all join into one profile.");
            if (chain.closed)
                chain.path.pop_back();      // back at the start
        }

        for (PathVertex& vertex : chain.path)
        {
            vertex.point.z = level;
            vertex.centre.z = level;
        }
        chain.ok = chain.path.size() >= (chain.closed ? 2u : 2u);
        if (!chain.ok)
            return refuse("There's no profile to follow.");
        return chain;
    }

    std::vector<ProfileRun> OffsetProfile(const Path& profile, bool closed, double distance)
    {
        const std::vector<Seg> segs = ToSegs(profile, closed);
        std::vector<ProfileRun> runs;
        if (segs.empty())
            return runs;
        const std::vector<Seg> raw = RawOffset(segs, closed, distance);

        // Split everything where it crosses itself.
        std::vector<std::vector<double>> cuts(raw.size());
        for (size_t i = 0; i < raw.size(); ++i)
        {
            for (size_t j = i + 1; j < raw.size(); ++j)
            {
                std::vector<std::pair<double, double>> found;
                Intersect(raw[i], raw[j], found);
                const bool next = (j == i + 1);
                const bool wraps = closed && i == 0 && j + 1 == raw.size();
                for (const auto& pair : found)
                {
                    // Neighbours meet at their shared end, which isn't a crossing.
                    if (next && pair.first > 1.0 - 1e-7 && pair.second < 1e-7)
                        continue;
                    if (wraps && pair.first < 1e-7 && pair.second > 1.0 - 1e-7)
                        continue;
                    if (pair.first > 1e-7 && pair.first < 1.0 - 1e-7)
                        cuts[i].push_back(pair.first);
                    if (pair.second > 1e-7 && pair.second < 1.0 - 1e-7)
                        cuts[j].push_back(pair.second);
                }
            }
        }

        // And wherever it crosses the edge of the band the offset's distance round
        // the profile: each segment's offsets either side, and a circle round each
        // vertex. Then each piece is wholly clear of the profile or wholly too
        // close, so its middle says which.
        std::vector<Seg> edges;
        const double reach = std::fabs(distance);
        if (reach > kTiny)
        {
            for (const Seg& s : segs)
            {
                for (const double side : { reach, -reach })
                {
                    Seg edge;
                    if (OffsetSeg(s, side, edge))
                        edges.push_back(edge);
                }
                for (const Vec3& corner : { s.a, s.b })
                {
                    for (const double from : { 0.0, kPi })
                    {
                        Seg half;
                        half.arc = true;
                        half.c = corner;
                        half.a = { corner.x + reach * std::cos(from), corner.y + reach * std::sin(from), corner.z };
                        half.b = { corner.x - reach * std::cos(from), corner.y - reach * std::sin(from), corner.z };
                        edges.push_back(half);
                    }
                }
            }
        }
        for (size_t i = 0; i < raw.size(); ++i)
        {
            for (const Seg& edge : edges)
            {
                std::vector<std::pair<double, double>> found;
                Intersect(raw[i], edge, found);
                for (const auto& pair : found)
                {
                    if (pair.first > 1e-7 && pair.first < 1.0 - 1e-7)
                        cuts[i].push_back(pair.first);
                }
            }
        }

        // Keep the pieces clear of the profile, joining them where they meet.
        const double clear = std::fabs(distance) - kClearTolerance;
        std::vector<std::vector<Seg>> kept;
        bool allKept = true;
        for (size_t i = 0; i < raw.size(); ++i)
        {
            std::vector<double> ts = cuts[i];
            ts.push_back(0.0);
            ts.push_back(1.0);
            std::sort(ts.begin(), ts.end());
            for (size_t k = 0; k + 1 < ts.size(); ++k)
            {
                if (ts[k + 1] - ts[k] < 1e-9)
                    continue;
                const Seg piece = SubSeg(raw[i], ts[k], ts[k + 1]);
                if (SegLength(piece) < kTiny)
                    continue;
                if (DistanceToSegs(segs, PointAt(piece, 0.5)) < clear)
                {
                    allKept = false;
                    continue;
                }
                if (!kept.empty() && !kept.back().empty() && DistanceXY(kept.back().back().b, piece.a) < 1e-6)
                    kept.back().push_back(piece);
                else
                    kept.push_back({ piece });
            }
        }

        // A closed profile's last run may carry on into its first.
        if (closed && kept.size() >= 2 && DistanceXY(kept.back().back().b, kept.front().front().a) < 1e-6)
        {
            kept.back().insert(kept.back().end(), kept.front().begin(), kept.front().end());
            kept.front() = std::move(kept.back());
            kept.pop_back();
        }

        for (const std::vector<Seg>& run : kept)
        {
            ProfileRun result;
            result.closed = closed && kept.size() == 1 && DistanceXY(run.back().b, run.front().a) < 1e-6;
            (void)allKept;
            result.path = ToPath(run, result.closed);
            runs.push_back(std::move(result));
        }
        return runs;
    }

    namespace
    {
        // A run to cut, as segments in the order it's cut.
        struct CutRun
        {
            std::vector<Seg> segs;
            bool closed = false;
            double offset = 0.0;            // its distance from the profile
            std::vector<std::pair<double, double>> lifted;     // tab spans, along it
        };

        double RunLength(const std::vector<Seg>& segs)
        {
            double total = 0.0;
            for (const Seg& s : segs)
                total += SegLength(s);
            return total;
        }

        Vec3 PointAtLength(const std::vector<Seg>& segs, double length)
        {
            for (const Seg& s : segs)
            {
                const double l = SegLength(s);
                if (length <= l || &s == &segs.back())
                    return PointAt(s, (l > 0.0) ? (std::min)(1.0, length / l) : 0.0);
                length -= l;
            }
            return segs.empty() ? Vec3() : segs.back().b;
        }

        // Where along the profile the tabs are centred.
        std::vector<double> TabPositions(double length, bool closed, const ProfilingSettings& settings)
        {
            std::vector<double> positions;
            if (!(length > 0.0))
                return positions;

            if (settings.tabSpacing == TabSpacing::Count)
            {
                const int count = (std::max)(1, settings.tabCount);
                for (int i = 0; i < count; ++i)
                    positions.push_back(closed ? (i + 0.5) * length / count : (i + 1) * length / (count + 1));
                return positions;
            }

            // One every distance, evenly round a closed profile, or centred along an open one.
            if (closed)
            {
                const int count = (std::max)(1, static_cast<int>(std::lround(length / settings.tabDistance)));
                for (int i = 0; i < count; ++i)
                    positions.push_back((i + 0.5) * length / count);
            }
            else
            {
                const int count = (std::max)(1, static_cast<int>(std::floor(length / settings.tabDistance)));
                const double first = (length - (count - 1) * settings.tabDistance) / 2.0;
                for (int i = 0; i < count; ++i)
                    positions.push_back(first + i * settings.tabDistance);
            }
            return positions;
        }

        // The spans of a run the tool lifts over, for tabs centred on these profile points.
        std::vector<std::pair<double, double>> LiftedSpans(const CutRun& run, const std::vector<Vec3>& centres,
                                                           double halfSpan)
        {
            std::vector<std::pair<double, double>> spans;
            const double total = RunLength(run.segs);
            for (const Vec3& centre : centres)
            {
                // Where the run passes the tab, if it does there.
                double best = -1.0, at = 0.0, before = 0.0;
                for (const Seg& s : run.segs)
                {
                    double t = 0.0;
                    const double d = NearestOn(s, centre, t);
                    if (best < 0.0 || d < best)
                    {
                        best = d;
                        at = before + t * SegLength(s);
                    }
                    before += SegLength(s);
                }
                if (best < 0.0 || best > std::fabs(run.offset) + 0.01)
                    continue;

                const double from = at - halfSpan, to = at + halfSpan;
                if (run.closed && from < 0.0)
                {
                    spans.push_back({ from + total, total });
                    spans.push_back({ 0.0, to });
                }
                else if (run.closed && to > total)
                {
                    spans.push_back({ from, total });
                    spans.push_back({ 0.0, to - total });
                }
                else
                {
                    spans.push_back({ (std::max)(0.0, from), (std::min)(total, to) });
                }
            }
            return spans;
        }

        bool Lifted(const std::vector<std::pair<double, double>>& spans, double at)
        {
            for (const auto& span : spans)
            {
                if (at > span.first && at < span.second)
                    return true;
            }
            return false;
        }

        // One run at one level: in, along it (over any tabs), and out.
        void CutAlong(PathBuilder& path, const CutRun& run, double z, double tabTop, bool tabs, double side,
                      bool forward)
        {
            // Away from the profile is the side the tool runs on.
            auto away = [&](const Vec3& travel) {
                const Vec3 along = forward ? travel : travel * -1.0;
                return LeftOf(along) * side;
            };

            const std::vector<std::pair<double, double>> none;
            const std::vector<std::pair<double, double>>& spans = tabs ? run.lifted : none;

            const Seg& first = run.segs.front();
            const Vec3 start{ first.a.x, first.a.y, z };
            path.SetCutZ(Lifted(spans, 1e-9) ? tabTop : z);
            path.Enter(start, TangentAt(first, 0.0), away(TangentAt(first, 0.0)));

            double before = 0.0;
            for (const Seg& s : run.segs)
            {
                const double length = SegLength(s);

                // Split where it goes on or off a tab.
                std::vector<double> marks{ 0.0, 1.0 };
                for (const auto& span : spans)
                {
                    for (const double edge : { span.first, span.second })
                    {
                        if (edge > before && edge < before + length)
                            marks.push_back((edge - before) / length);
                    }
                }
                std::sort(marks.begin(), marks.end());

                for (size_t k = 0; k + 1 < marks.size(); ++k)
                {
                    if (marks[k + 1] - marks[k] < 1e-12)
                        continue;
                    const Seg piece = SubSeg(s, marks[k], marks[k + 1]);
                    const double middle = before + length * (marks[k] + marks[k + 1]) / 2.0;
                    path.MoveToZ(Lifted(spans, middle) ? tabTop : z);
                    if (piece.arc)
                        path.ArcTo(piece.b, piece.c, !piece.ccw);
                    else
                        path.Cut(piece.b);
                }
                before += length;
            }

            const Seg& last = run.segs.back();
            path.Exit(last.b, TangentAt(last, 1.0), away(TangentAt(last, 1.0)));
            path.SetCutZ(z);
        }
    }

    ToolpathResult GenerateProfileToolpath(const ProfileToolpathInput& input)
    {
        const SequenceStep* pOperation = input.operation;
        if (pOperation == nullptr || pOperation->type != StepType::Profiling)
            return Failure("This isn't a profiling operation.");
        if (input.tool == nullptr || !(input.tool->diameter > 0.0))
            return Failure("Choose a tool on the Tool tab.");
        if (!input.profile.ok || input.profile.path.size() < 2)
            return Failure(input.profile.problem.empty() ? "There's no profile to follow." : input.profile.problem);

        const ProfilingSettings& settings = pOperation->profiling;
        for (const std::vector<std::string>& problems :
             { ValidateProfiling(settings), ValidateFeeds(pOperation->feeds), ValidateCutLevels(pOperation->cutLevels),
               ValidateEntryExit(pOperation->entryExit) })
        {
            if (!problems.empty())
                return Failure(problems.front());
        }

        // The profile is the cut geometry, at its own height.
        const double z = input.profile.path.front().point.z;
        const std::vector<CutLevel> levels = CutLevelHeights(pOperation->cutLevels, z);
        const EntryExitSettings& entryExit = pOperation->entryExit;
        double clearance = 0.0;
        std::string clearanceProblem;
        if (!ClearanceHeight(pOperation->clearance, input.havePartMaxZ, input.partMaxZ, clearance, clearanceProblem))
            return Failure(clearanceProblem);
        if (!(clearance > CutTopZ(pOperation->cutLevels, z)))
            return Failure("The clearance plane must be above the top of the cut.");
        if (entryExit.entry == EntryMode::AlongPath3D && !(clearance > levels.front().z + entryExit.alongPathHeight))
            return Failure("The along path height reaches the clearance plane.");

        // Which side the tool runs on, as +1 for the profile's left and -1 for its right.
        const bool closed = input.profile.closed;
        double side = (settings.startSide == ProfileSide::Left) ? 1.0 : -1.0;
        if (closed && settings.useClosedSide)
        {
            // An anticlockwise profile has its outside on the right.
            const bool anticlockwise = SignedArea(input.profile.path) > 0.0;
            const bool outside = settings.closedSide == ClosedProfileSide::Outside;
            side = (outside == anticlockwise) ? -1.0 : 1.0;
        }

        // Climb keeps the profile on the cutter's right, so with the tool on the
        // profile's left it runs the profile's way.
        const bool forward = (settings.direction == CutDirection::Mixed)   ? true
                             : (settings.direction == CutDirection::Climb) ? side > 0.0
                                                                           : side < 0.0;

        // The passes, from the widest out in to the one beside the profile.
        const double radius = input.tool->diameter / 2.0;
        std::vector<double> extras{ 0.0 };
        if (settings.cutWidth > 0.0)
        {
            const int steps = (std::max)(1, static_cast<int>(std::ceil(settings.cutWidth / settings.stepPerCut - 1e-9)));
            extras.clear();
            for (int i = 0; i <= steps; ++i)
                extras.push_back(settings.cutWidth * (steps - i) / steps);
        }

        const std::vector<Seg> profile = ToSegs(input.profile.path, closed);
        std::vector<Vec3> tabCentres;
        if (settings.useTabs)
        {
            for (const double at : TabPositions(RunLength(profile), closed, settings))
                tabCentres.push_back(PointAtLength(profile, at));
        }
        const double tabTop = levels.back().z + settings.tabHeight;

        std::vector<std::vector<CutRun>> passes;
        for (const double extra : extras)
        {
            const double offset = radius + settings.stockToLeave + extra;
            std::vector<CutRun> pass;
            for (const ProfileRun& run : OffsetProfile(input.profile.path, closed, side * offset))
            {
                CutRun cut;
                cut.segs = ToSegs(run.path, run.closed);
                cut.closed = run.closed;
                cut.offset = offset;
                if (cut.segs.empty())
                    continue;
                if (!forward)
                {
                    std::reverse(cut.segs.begin(), cut.segs.end());
                    for (Seg& s : cut.segs)
                        s = Reverse(s);
                }
                cut.lifted = LiftedSpans(cut, tabCentres, settings.tabLength / 2.0 + radius);
                pass.push_back(std::move(cut));
            }
            if (!forward)
                std::reverse(pass.begin(), pass.end());
            if (!pass.empty())
                passes.push_back(std::move(pass));
        }
        if (passes.empty())
            return Failure("The tool doesn't fit anywhere along the profile.");

        // Every pass at every level, top down. Entry and exit are at every level,
        // or only into the first and out of the last.
        ToolpathResult result;
        for (size_t l = 0; l < levels.size(); ++l)
        {
            const CutLevel& level = levels[l];
            PathBuilder path(result.toolpath, pOperation->feeds, clearance, level.z);
            path.SetEntryExit(&entryExit, entryExit.eachCutLevel || l == 0,
                              entryExit.eachCutLevel || l + 1 == levels.size());
            const bool tabs = settings.useTabs && level.z < tabTop - 1e-9;
            for (const std::vector<CutRun>& pass : passes)
            {
                for (const CutRun& run : pass)
                    CutAlong(path, run, level.z, tabTop, tabs, side, forward);
            }
        }

        result.ok = !result.toolpath.moves.empty();
        if (!result.ok)
            result.problem = "No passes fit the profile.";
        return result;
    }
}
