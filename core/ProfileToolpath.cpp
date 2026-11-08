// ProfileToolpath.cpp : calculates a Profiling operation's toolpath.

#include "ProfileToolpath.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "AdaptiveClearing.h"
#include "FacingToolpath.h"
#include "PathBuilder.h"
#include "PocketToolpath.h"

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

    ProfileChains ChainProfiles(const std::vector<std::vector<Path>>& pieces, double level)
    {
        ProfileChains result;

        // Every piece, labelled with its feature.
        struct Piece
        {
            const Path* pPath;
            size_t feature;
        };
        std::vector<Piece> all;
        for (size_t f = 0; f < pieces.size(); ++f)
        {
            for (const Path& path : pieces[f])
            {
                if (path.size() >= 2)
                    all.push_back({ &path, f });
            }
        }
        if (all.empty())
        {
            result.problem = "There's no profile to follow.";
            return result;
        }

        // Group pieces whose ends meet, through any chain of them.
        std::vector<size_t> group(all.size());
        for (size_t i = 0; i < all.size(); ++i)
            group[i] = i;
        auto root = [&](size_t i) {
            while (group[i] != i)
                i = group[i] = group[group[i]];
            return i;
        };
        for (size_t i = 0; i < all.size(); ++i)
        {
            for (size_t j = i + 1; j < all.size(); ++j)
            {
                bool meet = false;
                for (const Vec3& a : { all[i].pPath->front().point, all[i].pPath->back().point })
                {
                    for (const Vec3& b : { all[j].pPath->front().point, all[j].pPath->back().point })
                        meet = meet || DistanceXY(a, b) <= kProfileJoinTolerance;
                }
                if (meet)
                    group[root(j)] = root(i);
            }
        }

        // Each group is a profile, in the order its earliest piece comes.
        std::vector<size_t> order;
        for (size_t i = 0; i < all.size(); ++i)
        {
            if (std::find(order.begin(), order.end(), root(i)) == order.end())
                order.push_back(root(i));
        }
        for (const size_t g : order)
        {
            std::vector<std::vector<Path>> own(pieces.size());
            for (size_t i = 0; i < all.size(); ++i)
            {
                if (root(i) == g)
                    own[all[i].feature].push_back(*all[i].pPath);
            }
            ProfileChain chain = ChainProfile(own, level);
            if (!chain.ok)
            {
                result.problem = chain.problem;
                result.chains.clear();
                return result;
            }
            result.chains.push_back(std::move(chain));
        }
        result.ok = true;
        return result;
    }

    AreaGeometry GatherAreas(const std::vector<std::vector<Path>>& outlines, const std::vector<bool>& isFace)
    {
        AreaGeometry result;

        // The highest point of everything is the level it's all put at.
        bool found = false;
        for (const std::vector<Path>& outline : outlines)
        {
            for (const Path& piece : outline)
            {
                for (const PathVertex& vertex : piece)
                {
                    result.level = found ? (std::max)(result.level, vertex.point.z) : vertex.point.z;
                    found = true;
                }
            }
        }
        if (!found)
        {
            result.problem = "The features have no outline.";
            return result;
        }

        struct Item
        {
            Path path;
            size_t feature;
            bool face;
        };
        std::vector<Item> items;
        for (size_t f = 0; f < outlines.size(); ++f)
        {
            for (const Path& piece : outlines[f])
            {
                Path flat = FlattenPath(piece, result.level);
                if (flat.size() >= 2 && LengthXY(flat) > kProfileJoinTolerance)
                    items.push_back({ std::move(flat), f, f < isFace.size() && isFace[f] });
            }
        }
        if (items.empty())
        {
            result.problem = "Seen from above, the features have no length.";
            return result;
        }

        // A face's pieces stay together and apart from everything else. Edges
        // join any edge whose end they meet.
        std::vector<size_t> group(items.size());
        for (size_t i = 0; i < items.size(); ++i)
            group[i] = i;
        auto root = [&](size_t i) {
            while (group[i] != i)
                i = group[i] = group[group[i]];
            return i;
        };
        for (size_t i = 0; i < items.size(); ++i)
        {
            for (size_t j = i + 1; j < items.size(); ++j)
            {
                bool together = false;
                if (items[i].face || items[j].face)
                {
                    together = items[i].face && items[j].face && items[i].feature == items[j].feature;
                }
                else
                {
                    for (const Vec3& a : { items[i].path.front().point, items[i].path.back().point })
                    {
                        for (const Vec3& b : { items[j].path.front().point, items[j].path.back().point })
                            together = together || DistanceXY(a, b) <= kProfileJoinTolerance;
                    }
                }
                if (together)
                    group[root(j)] = root(i);
            }
        }

        // In the order of each group's first feature.
        std::vector<size_t> order;
        for (size_t i = 0; i < items.size(); ++i)
        {
            if (std::find(order.begin(), order.end(), root(i)) == order.end())
                order.push_back(root(i));
        }
        for (const size_t g : order)
        {
            std::vector<std::vector<Path>> area(outlines.size());
            for (size_t i = 0; i < items.size(); ++i)
            {
                if (root(i) == g)
                    area[items[i].feature].push_back(items[i].path);
            }
            result.areas.push_back(std::move(area));
        }
        result.ok = true;
        return result;
    }

    std::vector<Path> AreaPieces(const std::vector<std::vector<Path>>& area)
    {
        std::vector<Path> pieces;
        for (const std::vector<Path>& feature : area)
            pieces.insert(pieces.end(), feature.begin(), feature.end());
        return pieces;
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

    namespace
    {
        // What a chamfer changes in a profiling toolpath: the tool centre's
        // distance from the profile, the levels, and the top of the cut.
        struct ProfileOverride
        {
            double offset = 0.0;
            std::vector<CutLevel> levels;
            double cutTop = 0.0;
        };

        ToolpathResult ProfileToolpath(const ProfileToolpathInput& input, const ProfileOverride* pOverride);
    }

    ToolpathResult GenerateProfileToolpath(const ProfileToolpathInput& input)
    {
        if (input.operation == nullptr || input.operation->type != StepType::Profiling)
            return Failure("This isn't a profiling operation.");
        return ProfileToolpath(input, nullptr);
    }

    double ChamferDepth(const Tool& tool, const ChamferSettings& chamfer)
    {
        const double half = tool.includedAngle / 2.0 * kPi / 180.0;
        return chamfer.width / std::tan(half);
    }

    double ChamferOffset(const Tool& tool, const ChamferSettings& chamfer, double stockToLeave)
    {
        const double half = tool.includedAngle / 2.0 * kPi / 180.0;
        return tool.tipDiameter / 2.0 + chamfer.tipClearance * std::tan(half) + stockToLeave;
    }

    ToolpathResult GenerateChamferToolpath(const ProfileToolpathInput& input)
    {
        const SequenceStep* pOperation = input.operation;
        if (pOperation == nullptr || pOperation->type != StepType::Chamfer)
            return Failure("This isn't a chamfer operation.");
        if (input.tool == nullptr)
            return Failure("Choose a tool on the Tool tab.");
        if (input.tool->type != ToolType::VMill && input.tool->type != ToolType::ChamferMill)
            return Failure("A chamfer needs a V-mill or a chamfer mill, not a " + ToolTypeTitle(input.tool->type) +
                           ".");
        if (!(input.tool->includedAngle > 0.0 && input.tool->includedAngle < 180.0))
            return Failure("The tool's included angle must be more than 0 and less than 180 degrees.");
        const std::vector<std::string> problems = ValidateChamfer(pOperation->chamfer);
        if (!problems.empty())
            return Failure(problems.front());
        if (input.profiles.empty() || input.profiles.front().path.empty())
            return Failure("There's no profile to follow.");

        // The chamfer's depth is cut in equal passes, none deeper than Max
        // Depth/Cut. Each pass's tip is the tooltip clearance below that pass's
        // depth: the tool centre is outside the edge, so the clearance is in air.
        const ChamferSettings& chamfer = pOperation->chamfer;
        const double top = input.profiles.front().path.front().point.z;
        const double depth = ChamferDepth(*input.tool, chamfer);
        const int passes = (std::max)(1, static_cast<int>(std::ceil(depth / chamfer.maxDepthPerCut - 1e-9)));

        ProfileOverride cut;
        cut.offset = ChamferOffset(*input.tool, chamfer, pOperation->profiling.stockToLeave);
        cut.cutTop = top;
        for (int k = 1; k <= passes; ++k)
        {
            CutLevel level;
            level.z = top - depth * k / passes - chamfer.tipClearance;
            level.finish = k == passes;
            cut.levels.push_back(level);
        }
        if (!(cut.offset >= 0.0))
            return Failure("The stock to leave puts the tool past the edge.");
        return ProfileToolpath(input, &cut);
    }

    namespace
    {
    ToolpathResult ProfileToolpath(const ProfileToolpathInput& input, const ProfileOverride* pOverride)
    {
        const SequenceStep* pOperation = input.operation;
        if (input.tool == nullptr || !(input.tool->diameter > 0.0))
            return Failure("Choose a tool on the Tool tab.");
        if (input.profiles.empty())
            return Failure("There's no profile to follow.");
        for (const ProfileChain& profile : input.profiles)
        {
            if (!profile.ok || profile.path.size() < 2)
                return Failure(profile.problem.empty() ? "There's no profile to follow." : profile.problem);
        }

        const ProfilingSettings& settings = pOperation->profiling;
        // A chamfer has no Cut Levels tab, or tabs.
        for (const std::vector<std::string>& problems :
             { ValidateProfiling(settings), ValidateFeeds(pOperation->feeds),
               pOverride != nullptr ? std::vector<std::string>() : ValidateCutLevels(pOperation->cutLevels),
               ValidateEntryExit(pOperation->entryExit) })
        {
            if (!problems.empty())
                return Failure(problems.front());
        }
        const bool useTabs = settings.useTabs && pOverride == nullptr;

        // The profiles are the cut geometry, all at one height.
        const double z = input.profiles.front().path.front().point.z;
        const std::vector<CutLevel> levels =
            (pOverride != nullptr) ? pOverride->levels : CutLevelHeights(pOperation->cutLevels, z);
        const double cutTop = (pOverride != nullptr) ? pOverride->cutTop : CutTopZ(pOperation->cutLevels, z);
        const EntryExitSettings& entryExit = pOperation->entryExit;
        double clearance = 0.0;
        std::string clearanceProblem;
        if (!ClearanceHeight(pOperation->clearance, input.havePartMaxZ, input.partMaxZ, clearance, clearanceProblem,
                             input.haveStockMaxZ, input.stockMaxZ))
            return Failure(clearanceProblem);
        if (!(clearance > cutTop))
            return Failure("The clearance plane must be above the top of the cut.");
        if (entryExit.entry == EntryMode::AlongPath3D && !(clearance > levels.front().z + entryExit.alongPathHeight))
            return Failure("The along path height reaches the clearance plane.");

        // The passes' extra widths, from the widest out in to the one beside the profile.
        const double radius = input.tool->diameter / 2.0;
        std::vector<double> extras{ 0.0 };
        if (settings.cutWidth > 0.0)
        {
            const int steps = (std::max)(1, static_cast<int>(std::ceil(settings.cutWidth / settings.stepPerCut - 1e-9)));
            extras.clear();
            for (int i = 0; i <= steps; ++i)
                extras.push_back(settings.cutWidth * (steps - i) / steps);
        }
        const double tabTop = levels.back().z + settings.tabHeight;

        // Each profile's side, direction and passes, worked out once.
        struct Profile
        {
            double side = 1.0;
            bool forward = true;
            std::vector<std::vector<CutRun>> passes;
        };
        std::vector<Profile> profiles;
        for (size_t n = 0; n < input.profiles.size(); ++n)
        {
            const ProfileChain& chain = input.profiles[n];
            Profile profile;

            // Which side the tool runs on, as +1 for the profile's left and -1 for its right.
            const bool closed = chain.closed;
            profile.side = (settings.startSide == ProfileSide::Left) ? 1.0 : -1.0;
            if (closed && settings.useClosedSide)
            {
                // An anticlockwise profile has its outside on the right.
                const bool anticlockwise = SignedArea(chain.path) > 0.0;
                const bool outside = settings.closedSide == ClosedProfileSide::Outside;
                profile.side = (outside == anticlockwise) ? -1.0 : 1.0;
            }

            // Climb keeps the profile on the cutter's right, so with the tool on the
            // profile's left it runs the profile's way.
            profile.forward = (settings.direction == CutDirection::Mixed)   ? true
                              : (settings.direction == CutDirection::Climb) ? profile.side > 0.0
                                                                            : profile.side < 0.0;

            const std::vector<Seg> segs = ToSegs(chain.path, closed);
            std::vector<Vec3> tabCentres;
            if (useTabs)
            {
                for (const double at : TabPositions(RunLength(segs), closed, settings))
                    tabCentres.push_back(PointAtLength(segs, at));
            }

            for (const double extra : extras)
            {
                const double offset =
                    ((pOverride != nullptr) ? pOverride->offset : radius + settings.stockToLeave) + extra;
                std::vector<CutRun> pass;
                for (const ProfileRun& run : OffsetProfile(chain.path, closed, profile.side * offset))
                {
                    CutRun cut;
                    cut.segs = ToSegs(run.path, run.closed);
                    cut.closed = run.closed;
                    cut.offset = offset;
                    if (cut.segs.empty())
                        continue;
                    if (!profile.forward)
                    {
                        std::reverse(cut.segs.begin(), cut.segs.end());
                        for (Seg& seg : cut.segs)
                            seg = Reverse(seg);
                    }
                    cut.lifted = LiftedSpans(cut, tabCentres, settings.tabLength / 2.0 + radius);
                    pass.push_back(std::move(cut));
                }
                if (!profile.forward)
                    std::reverse(pass.begin(), pass.end());
                if (!pass.empty())
                    profile.passes.push_back(std::move(pass));
            }
            if (profile.passes.empty())
            {
                const std::string which =
                    (input.profiles.size() > 1) ? "Profile " + std::to_string(n + 1) + ": " : std::string();
                return Failure(which + "the tool doesn't fit anywhere along the profile.");
            }
            profiles.push_back(std::move(profile));
        }

        // Every pass at every level, top down. Entry and exit are at every level,
        // or only into the first and out of the last.
        ToolpathResult result;
        auto cut = [&](const Profile& profile, size_t l) {
            const CutLevel& level = levels[l];
            PathBuilder path(result.toolpath, pOperation->feeds, clearance, level.z);
            path.SetEntryExit(&entryExit, entryExit.eachCutLevel || l == 0,
                              entryExit.eachCutLevel || l + 1 == levels.size());
            const bool tabs = useTabs && level.z < tabTop - 1e-9;
            for (const std::vector<CutRun>& pass : profile.passes)
            {
                for (const CutRun& run : pass)
                    CutAlong(path, run, level.z, tabTop, tabs, profile.side, profile.forward);
            }
        };
        if (pOverride == nullptr && pOperation->cutLevels.order == CutLevelOrder::LevelFirst)
        {
            for (size_t l = 0; l < levels.size(); ++l)
            {
                for (const Profile& profile : profiles)
                    cut(profile, l);
            }
        }
        else
        {
            for (const Profile& profile : profiles)
            {
                for (size_t l = 0; l < levels.size(); ++l)
                    cut(profile, l);
            }
        }

        result.ok = !result.toolpath.moves.empty();
        if (!result.ok)
            result.problem = "No passes fit the profile.";
        return result;
    }
    }

    /////////////////////////////////////////////////////////////////////////
    // Pocketing

    namespace
    {
        // A loop or row to cut, and the side of it that's open to the pocket.
        struct PocketPiece
        {
            std::vector<Seg> segs;
            Vec3 away;              // for the entry and exit motions
            bool awayFromLoop = false;  // away is left of the loop's own way (true), or fixed
        };

        std::vector<Seg> ReverseSegs(const std::vector<Seg>& segs)
        {
            std::vector<Seg> reversed;
            for (auto it = segs.rbegin(); it != segs.rend(); ++it)
                reversed.push_back(Reverse(*it));
            return reversed;
        }

        bool LoopIsAnticlockwise(const std::vector<Seg>& segs)
        {
            return SignedArea(ToPath(segs, true)) > 0.0;
        }

        // A closed loop started again at the point on it nearest to near.
        std::vector<Seg> StartNear(const std::vector<Seg>& segs, const Vec3& near)
        {
            size_t best = 0;
            double bestT = 0.0, bestDistance = -1.0;
            for (size_t i = 0; i < segs.size(); ++i)
            {
                double t = 0.0;
                const double d = NearestOn(segs[i], near, t);
                if (bestDistance < 0.0 || d < bestDistance)
                {
                    bestDistance = d;
                    best = i;
                    bestT = t;
                }
            }

            std::vector<Seg> rotated;
            const bool split = bestT > 1e-9 && bestT < 1.0 - 1e-9;
            if (split)
                rotated.push_back(SubSeg(segs[best], bestT, 1.0));
            const size_t first = (bestT >= 1.0 - 1e-9) ? best + 1 : (split ? best + 1 : best);
            for (size_t k = 0; k < segs.size(); ++k)
            {
                const size_t i = (first + k) % segs.size();
                if (split && i == best)
                    break;
                rotated.push_back(segs[i]);
            }
            if (split)
                rotated.push_back(SubSeg(segs[best], 0.0, bestT));
            return rotated;
        }

        // The point a share of the way round a loop, by length.
        Vec3 PointRound(const std::vector<Seg>& segs, double share)
        {
            return PointAtLength(segs, RunLength(segs) * share);
        }

        // Every offset of the outline from the wall inwards, a stepover apart,
        // until nothing fits.
        std::vector<std::vector<ProfileRun>> PocketRings(const Path& outline, double wall, double stepover)
        {
            std::vector<std::vector<ProfileRun>> rings;
            for (int k = 0; k < 100000; ++k)
            {
                std::vector<ProfileRun> ring = OffsetProfile(outline, true, wall + k * stepover);
                std::vector<ProfileRun> kept;
                for (ProfileRun& run : ring)
                {
                    if (RunLength(ToSegs(run.path, run.closed)) > 1e-6)
                        kept.push_back(std::move(run));
                }
                if (kept.empty())
                    break;
                rings.push_back(std::move(kept));
            }
            return rings;
        }

        // Feeding straight from a to b keeps clear of the wall, and isn't far.
        bool SafeLink(const std::vector<Seg>& outline, double wall, const Vec3& a, const Vec3& b, double furthest)
        {
            const double length = DistanceXY(a, b);
            if (length > furthest)
                return false;
            const int samples = 8;
            for (int i = 0; i <= samples; ++i)
            {
                const double t = static_cast<double>(i) / samples;
                const Vec3 p{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, 0.0 };
                if (DistanceToSegs(outline, p) < wall - 1e-3)
                    return false;
            }
            return true;
        }

        // Cuts the pieces at one level, staying down between them when it's safe.
        void CutPieces(PathBuilder& path, const std::vector<PocketPiece>& pieces, double z,
                       const std::vector<Seg>& outline, double wall, double furthest)
        {
            auto awayAt = [](const PocketPiece& piece, const Vec3& tangent) {
                return piece.awayFromLoop ? LeftOf(tangent) * (piece.away.x) : piece.away;
            };

            bool down = false;
            Vec3 at, lastTangent, lastAway;
            for (const PocketPiece& piece : pieces)
            {
                if (piece.segs.empty())
                    continue;
                const Seg& first = piece.segs.front();
                const Vec3 start{ first.a.x, first.a.y, z };
                const Vec3 tangent = TangentAt(first, 0.0);

                if (down && SafeLink(outline, wall, at, start, furthest))
                {
                    if (DistanceXY(at, start) > kTiny)
                        path.Cut(start);
                }
                else
                {
                    if (down)
                        path.Exit(at, lastTangent, lastAway);
                    path.Enter(start, tangent, awayAt(piece, tangent));
                }

                for (const Seg& s : piece.segs)
                {
                    if (s.arc)
                        path.ArcTo(s.b, s.c, !s.ccw);
                    else
                        path.Cut(s.b);
                }
                const Seg& last = piece.segs.back();
                at = { last.b.x, last.b.y, z };
                lastTangent = TangentAt(last, 1.0);
                lastAway = awayAt(piece, lastTangent);
                down = true;
            }
            if (down)
                path.Exit(at, lastTangent, lastAway);
        }

        // A loop to cut, turned the way it's cut and started near the last end.
        // Its open side is towards the pocket: left of an anticlockwise loop,
        // which is right when it's cut clockwise.
        PocketPiece LoopPiece(const ProfileRun& run, bool anticlockwise, const Vec3* pNear)
        {
            PocketPiece piece;
            std::vector<Seg> segs = ToSegs(run.path, run.closed);
            if (run.closed)
            {
                if (LoopIsAnticlockwise(segs) != anticlockwise)
                    segs = ReverseSegs(segs);
                if (pNear != nullptr)
                    segs = StartNear(segs, *pNear);
            }
            else if (!anticlockwise)
            {
                // Pieces of an outline's offset run its way, which is anticlockwise.
                segs = ReverseSegs(segs);
            }
            piece.segs = std::move(segs);
            piece.awayFromLoop = true;
            piece.away = { anticlockwise ? 1.0 : -1.0, 0.0, 0.0 };     // which side of the tangent
            return piece;
        }

        // The lap round the wall, after the rows or the spiral.
        void AddLaps(std::vector<PocketPiece>& pieces, const std::vector<ProfileRun>& wallRing, bool anticlockwise)
        {
            for (const ProfileRun& run : wallRing)
            {
                const Vec3* pNear = nullptr;
                Vec3 end;
                if (!pieces.empty() && !pieces.back().segs.empty())
                {
                    end = pieces.back().segs.back().b;
                    pNear = &end;
                }
                pieces.push_back(LoopPiece(run, anticlockwise, pNear));
            }
        }

        std::vector<PocketPiece> OffsetPieces(const std::vector<std::vector<ProfileRun>>& rings, bool fromInside,
                                              CutDirection direction)
        {
            std::vector<PocketPiece> pieces;
            bool anticlockwise = (direction != CutDirection::Conventional);
            for (size_t n = 0; n < rings.size(); ++n)
            {
                const std::vector<ProfileRun>& ring = rings[fromInside ? rings.size() - 1 - n : n];
                for (const ProfileRun& run : ring)
                {
                    Vec3 end;
                    const Vec3* pNear = nullptr;
                    if (!pieces.empty())
                    {
                        end = pieces.back().segs.back().b;
                        pNear = &end;
                    }
                    pieces.push_back(LoopPiece(run, anticlockwise, pNear));
                    if (direction == CutDirection::Mixed)
                        anticlockwise = !anticlockwise;
                }
            }
            return pieces;
        }

        std::vector<PocketPiece> LinearPieces(const std::vector<ProfileRun>& wallRing, const PocketingSettings& settings,
                                              double stepover, std::string& problem)
        {
            std::vector<PocketPiece> pieces;
            for (const ProfileRun& run : wallRing)
            {
                if (!run.closed)
                {
                    problem = "The pocket is too narrow in places for linear cuts.";
                    return pieces;
                }
            }

            const Vec3 along = PassDirection(settings.cutAngle);
            const Vec3 across = LeftOf(along);

            // The rows run across the wall's extent, a stepover apart at most. The
            // first and last would only touch the wall, which the lap cuts.
            double low = 0.0, high = 0.0;
            bool found = false;
            for (const ProfileRun& run : wallRing)
            {
                double a = 0.0, b = 0.0;
                PathExtent(run.path, across, a, b);
                low = found ? (std::min)(low, a) : a;
                high = found ? (std::max)(high, b) : b;
                found = true;
            }
            const int intervals = (std::max)(1, static_cast<int>(std::ceil((high - low) / stepover - 1e-9)));
            const double spacing = (high - low) / intervals;

            // Start at Bottom starts on the frame's -Y side. Rows along Y start on -X.
            const bool acrossGoesUp = std::fabs(across.y) > 1e-9 ? across.y > 0.0 : across.x < 0.0;
            const bool fromLow = (settings.linearStart == PocketLinearStart::Bottom) == acrossGoesUp;
            const double stepSign = fromLow ? 1.0 : -1.0;

            // As for Facing: the uncut side is the way the rows step, and climb keeps it on the right.
            const CutDirection direction = PocketDirection(settings);
            const bool climbForward = (stepSign < 0.0);
            bool forward = (direction == CutDirection::Conventional) ? !climbForward : climbForward;
            const Vec3 cutSide = across * -stepSign;

            for (int i = 1; i < intervals; ++i)
            {
                const double v = fromLow ? low + i * spacing : high - i * spacing;
                std::vector<double> crossings;
                for (const ProfileRun& run : wallRing)
                {
                    const std::vector<double> more = PathCrossings(run.path, along, across, v);
                    crossings.insert(crossings.end(), more.begin(), more.end());
                }
                std::sort(crossings.begin(), crossings.end());

                // Inside between each pair of crossings, cut in the row's direction.
                std::vector<std::pair<double, double>> spans;
                for (size_t k = 0; k + 1 < crossings.size(); k += 2)
                {
                    if (crossings[k + 1] - crossings[k] > 1e-6)
                        spans.push_back({ crossings[k], crossings[k + 1] });
                }
                if (!forward)
                    std::reverse(spans.begin(), spans.end());
                for (const auto& span : spans)
                {
                    const double from = forward ? span.first : span.second;
                    const double to = forward ? span.second : span.first;
                    Seg row;
                    row.a = { from * along.x + v * across.x, from * along.y + v * across.y, 0.0 };
                    row.b = { to * along.x + v * across.x, to * along.y + v * across.y, 0.0 };
                    PocketPiece piece;
                    piece.segs.push_back(row);
                    piece.away = cutSide;
                    pieces.push_back(piece);
                }
                if (direction == CutDirection::Mixed)
                    forward = !forward;
            }

            // Then round the wall, climbing for Mixed.
            AddLaps(pieces, wallRing, direction != CutDirection::Conventional);
            return pieces;
        }

        std::vector<PocketPiece> SpiralPieces(const std::vector<std::vector<ProfileRun>>& rings, bool fromInside,
                                              CutDirection direction, double stepover)
        {
            const bool anticlockwise = (direction != CutDirection::Conventional);

            // The spiral goes through the loops while each is one closed loop. Past
            // that, the rest are cut as offset loops.
            size_t spiralRings = 0;
            while (spiralRings < rings.size() && rings[spiralRings].size() == 1 && rings[spiralRings][0].closed)
                ++spiralRings;

            std::vector<PocketPiece> pieces;
            if (spiralRings >= 2)
            {
                // Built from the wall in, turned the opposite way if it'll be run backwards from the middle.
                const bool buildAnticlockwise = fromInside ? !anticlockwise : anticlockwise;
                std::vector<std::vector<Seg>> loops;
                for (size_t k = 0; k < spiralRings; ++k)
                {
                    std::vector<Seg> segs = ToSegs(rings[k][0].path, true);
                    if (LoopIsAnticlockwise(segs) != buildAnticlockwise)
                        segs = ReverseSegs(segs);
                    if (!loops.empty())
                        segs = StartNear(segs, loops.back().front().a);
                    loops.push_back(std::move(segs));
                }

                // Each turn blends from one loop to the next.
                Polyline points;
                const double spacing = (std::max)(0.25, stepover / 4.0);
                for (size_t k = 0; k + 1 < loops.size(); ++k)
                {
                    const int samples = (std::max)(24, (std::min)(4000, static_cast<int>(std::ceil(RunLength(loops[k]) / spacing))));
                    for (int i = 0; i < samples; ++i)
                    {
                        const double t = static_cast<double>(i) / samples;
                        const Vec3 outer = PointRound(loops[k], t);
                        const Vec3 inner = PointRound(loops[k + 1], t);
                        points.push_back({ outer.x + (inner.x - outer.x) * t, outer.y + (inner.y - outer.y) * t, 0.0 });
                    }
                }
                points.push_back(loops.back().front().a);
                if (fromInside)
                    std::reverse(points.begin(), points.end());

                PocketPiece spiral;
                for (size_t i = 0; i + 1 < points.size(); ++i)
                {
                    Seg s;
                    s.a = points[i];
                    s.b = points[i + 1];
                    if (DistanceXY(s.a, s.b) > kTiny)
                        spiral.segs.push_back(s);
                }
                spiral.awayFromLoop = true;
                spiral.away = { anticlockwise ? 1.0 : -1.0, 0.0, 0.0 };

                // The innermost loop is cut whole: first from the middle, last from the wall.
                const ProfileRun& innermost = rings[spiralRings - 1][0];
                const Vec3 middle = loops.back().front().a;
                PocketPiece centre = LoopPiece(innermost, anticlockwise, &middle);
                if (fromInside)
                {
                    pieces.push_back(centre);
                    pieces.push_back(spiral);
                }
                else
                {
                    pieces.push_back(spiral);
                    pieces.push_back(centre);
                }
            }

            // Whatever the spiral can't reach, as loops, then the lap round the wall.
            std::vector<std::vector<ProfileRun>> rest(rings.begin() + (std::min)(spiralRings, rings.size()), rings.end());
            if (spiralRings < 2)
                rest.assign(rings.begin() + (rings.empty() ? 0 : 1), rings.end());
            // From the middle they come first, before the spiral works out.
            const std::vector<PocketPiece> loops =
                OffsetPieces(rest, fromInside, anticlockwise ? CutDirection::Climb : CutDirection::Conventional);
            pieces.insert(fromInside ? pieces.begin() : pieces.end(), loops.begin(), loops.end());
            if (!rings.empty())
                AddLaps(pieces, rings.front(), anticlockwise);
            return pieces;
        }
    }

    ToolpathResult GeneratePocketToolpath(const PocketToolpathInput& input)
    {
        const SequenceStep* pOperation = input.operation;
        if (pOperation == nullptr || pOperation->type != StepType::Pocketing)
            return Failure("This isn't a pocketing operation.");
        if (input.tool == nullptr || !(input.tool->diameter > 0.0))
            return Failure("Choose a tool on the Tool tab.");
        if (input.outlines.empty())
            return Failure("There's no pocket outline.");
        for (const ProfileChain& outline : input.outlines)
        {
            if (!outline.ok || outline.path.size() < 2)
                return Failure(outline.problem.empty() ? "There's no pocket outline." : outline.problem);
            if (!outline.closed)
                return Failure("Each pocket's edges must join into a closed outline.");
        }

        const PocketingSettings& settings = pOperation->pocketing;
        if (settings.cutType == PocketCutType::Radial)
            return Failure("Radial cuts aren't built yet.");
        for (const std::vector<std::string>& problems :
             { ValidatePocketing(settings), ValidateFeeds(pOperation->feeds), ValidateCutLevels(pOperation->cutLevels),
               ValidateEntryExit(pOperation->entryExit) })
        {
            if (!problems.empty())
                return Failure(problems.front());
        }

        // The outlines are the cut geometry, all at one height.
        const double z = input.outlines.front().path.front().point.z;
        const std::vector<CutLevel> levels = CutLevelHeights(pOperation->cutLevels, z);
        const EntryExitSettings& entryExit = pOperation->entryExit;
        double clearance = 0.0;
        std::string problem;
        if (!ClearanceHeight(pOperation->clearance, input.havePartMaxZ, input.partMaxZ, clearance, problem,
                             input.haveStockMaxZ, input.stockMaxZ))
            return Failure(problem);
        if (!(clearance > CutTopZ(pOperation->cutLevels, z)))
            return Failure("The clearance plane must be above the top of the cut.");
        if (entryExit.entry == EntryMode::AlongPath3D && !(clearance > levels.front().z + entryExit.alongPathHeight))
            return Failure("The along path height reaches the clearance plane.");

        const double wall = input.tool->diameter / 2.0 + settings.stockToLeave;
        const double stepover = ResolveSize(settings.stepover, input.tool->diameter);
        if (!(wall > 0.0))
            return Failure("The stock to leave can't be less than minus the tool's radius.");

        // Each pocket's pattern, worked out once.
        struct Pocket
        {
            std::vector<Seg> outline;
            std::vector<PocketPiece> pieces;
            std::vector<AdaptiveCut> adaptive;      // with Use Adaptive Clearing, then a lap of the wall
        };
        std::vector<Pocket> pockets;
        const CutDirection direction = PocketDirection(settings);
        const bool fromInside = settings.start == PocketStart::Inside;
        for (size_t n = 0; n < input.outlines.size(); ++n)
        {
            // Inside an anticlockwise outline is its left.
            Path outline = input.outlines[n].path;
            if (SignedArea(outline) < 0.0)
                outline = ReversePath(outline, true);

            const std::vector<std::vector<ProfileRun>> rings = PocketRings(outline, wall, stepover);
            const std::string which =
                (input.outlines.size() > 1) ? "Pocket " + std::to_string(n + 1) + ": " : std::string();
            if (rings.empty())
                return Failure(which + "the tool doesn't fit in the pocket.");

            Pocket pocket;
            pocket.outline = ToSegs(outline, true);
            if (settings.adaptive)
            {
                // Adaptive clearing inside the wall, then the wall's own loop, exactly.
                double xLow = 0.0, xHigh = 0.0, yLow = 0.0, yHigh = 0.0;
                PathExtent(outline, { 1.0, 0.0, 0.0 }, xLow, xHigh);
                PathExtent(outline, { 0.0, 1.0, 0.0 }, yLow, yHigh);
                const double cell = AdaptiveCell(input.tool->diameter, stepover, xHigh - xLow, yHigh - yLow);
                AdaptiveSettings adaptive;
                adaptive.toolRadius = input.tool->diameter / 2.0;
                adaptive.optimalLoad = stepover;
                adaptive.climb = settings.direction != CutDirection::Conventional;
                adaptive.ramp = settings.rampEntry;
                adaptive.wallLaps = false;
                adaptive.simplify = input.simplifyTolerance;
                pocket.adaptive =
                    AdaptiveClear(AdaptiveGridFromOutlines({ outline }, cell, wall, adaptive.toolRadius), adaptive).cuts;
                pocket.pieces = OffsetPieces({ rings.front() }, true,
                                             adaptive.climb ? CutDirection::Climb : CutDirection::Conventional);
                pockets.push_back(std::move(pocket));
                continue;
            }
            switch (settings.cutType)
            {
            case PocketCutType::Linear:
                pocket.pieces = LinearPieces(rings.front(), settings, stepover, problem);
                if (!problem.empty())
                    return Failure(which + problem);
                break;
            case PocketCutType::Spiral:
                pocket.pieces = SpiralPieces(rings, fromInside, direction, stepover);
                break;
            case PocketCutType::Offset:
            default:
                pocket.pieces = OffsetPieces(rings, fromInside, direction);
                break;
            }
            pockets.push_back(std::move(pocket));
        }

        // Every level repeats each pocket's pattern, top down, with entries and
        // exits at every level or only into the first and out of the last.
        const double furthest = 2.0 * stepover + 1e-6;
        ToolpathResult result;
        auto cut = [&](const Pocket& pocket, size_t l) {
            if (settings.adaptive)
            {
                // Down from 2 mm above the level above (the cut's top for the first).
                const double top = (l == 0) ? CutTopZ(pOperation->cutLevels, z) : levels[l - 1].z;
                AdaptiveWriting writing;
                writing.z = levels[l].z;
                writing.top = top;
                writing.enter = (std::min)(top + 2.0, clearance);
                writing.clearance = clearance;
                writing.feeds = pOperation->feeds;
                writing.climb = settings.direction != CutDirection::Conventional;
                writing.rampAngle = settings.rampEntry.rampAngle;
                WriteAdaptiveCuts(pocket.adaptive, writing, result.toolpath);
            }
            PathBuilder path(result.toolpath, pOperation->feeds, clearance, levels[l].z);
            path.SetEntryExit(&entryExit, entryExit.eachCutLevel || l == 0,
                              entryExit.eachCutLevel || l + 1 == levels.size());
            CutPieces(path, pocket.pieces, levels[l].z, pocket.outline, wall, furthest);
        };
        if (pOperation->cutLevels.order == CutLevelOrder::LevelFirst)
        {
            for (size_t l = 0; l < levels.size(); ++l)
            {
                for (const Pocket& pocket : pockets)
                    cut(pocket, l);
            }
        }
        else
        {
            for (const Pocket& pocket : pockets)
            {
                for (size_t l = 0; l < levels.size(); ++l)
                    cut(pocket, l);
            }
        }

        result.ok = !result.toolpath.moves.empty();
        if (!result.ok)
            result.problem = "Nothing fits in the pocket.";
        return result;
    }
}
