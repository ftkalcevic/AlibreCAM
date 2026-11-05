// HolePocketToolpath.cpp : calculates a Hole Pocketing operation's toolpath.

#include "HolePocketToolpath.h"

#include <algorithm>
#include <cmath>

#include "Features.h"

namespace camcore
{
    namespace
    {
        const double kPi = 3.14159265358979323846;

        ToolpathResult Failure(const std::string& problem)
        {
            ToolpathResult result;
            result.problem = problem;
            return result;
        }

        Vec3 Around(const Vec3& centre, double radius, double angle, double z)
        {
            return { centre.x + radius * std::cos(angle), centre.y + radius * std::sin(angle), z };
        }

        class HoleBuilder
        {
        public:
            HoleBuilder(Toolpath& toolpath, const FeedSettings& feeds, double clearance, bool anticlockwise)
                : m_toolpath(toolpath), m_feeds(feeds), m_clearance(clearance), m_anticlockwise(anticlockwise)
            {
            }

            const Vec3& At() const { return m_toolpath.moves.back().to; }

            void Rapid(const Vec3& to) { Add(MoveKind::Rapid, to, 0.0); }

            void Transfer(const Vec3& to)
            {
                if (m_toolpath.moves.empty() || m_feeds.transferAtRapid)
                    Add(MoveKind::Rapid, to, 0.0);
                else
                    Add(MoveKind::Feed, to, m_feeds.transfer);
            }

            void Feed(const Vec3& to, double feed) { Add(MoveKind::Feed, to, feed); }

            void Arc(const Vec3& to, const Vec3& centre, double feed)
            {
                Add(MoveKind::Feed, to, feed);
                ToolpathMove& move = m_toolpath.moves.back();
                move.arc = true;
                move.centre = { centre.x, centre.y, to.z };
                move.clockwise = !m_anticlockwise;
            }

            // Round the circle about centre from angle, descending from the
            // current Z to z by drop a turn, in arcs of at most half a turn.
            // Returns the angle it ends at.
            double Helix(const Vec3& centre, double radius, double angle, double z, double drop)
            {
                const double from = At().z;
                const double turns = (from - z) / drop;
                const int halves = (std::max)(1, static_cast<int>(std::ceil(turns * 2.0 - 1e-9)));
                const double sweep = 2.0 * kPi * turns / halves * (m_anticlockwise ? 1.0 : -1.0);
                for (int i = 1; i <= halves; ++i)
                {
                    const double a = angle + sweep * i;
                    Arc(Around(centre, radius, a, from + (z - from) * i / halves), centre, m_feeds.engage);
                }
                return angle + sweep * halves;
            }

            // Once round the circle about centre at the current Z, as two half turns.
            void Lap(const Vec3& centre, double radius, double angle)
            {
                const double half = m_anticlockwise ? kPi : -kPi;
                Arc(Around(centre, radius, angle + half, At().z), centre, m_feeds.cut);
                Arc(Around(centre, radius, angle, At().z), centre, m_feeds.cut);
            }

            // Out from one radius to another, a stepover a turn, as half circles
            // whose centres shift each half turn. Returns the angle it ends at.
            double SpiralOut(const Vec3& centre, double from, double to, double angle, double stepover)
            {
                const int halves = (std::max)(1, static_cast<int>(std::ceil((to - from) / (stepover / 2.0) - 1e-9)));
                const double grow = (to - from) / halves;
                double radius = from;
                const double z = At().z;
                for (int i = 0; i < halves; ++i)
                {
                    // From radius r1 one side to r2 the other, round the circle on that diameter.
                    const double next = radius + grow;
                    const Vec3 u{ std::cos(angle), std::sin(angle), 0.0 };
                    const Vec3 mid{ centre.x + u.x * (radius - next) / 2.0, centre.y + u.y * (radius - next) / 2.0, z };
                    angle += kPi;
                    Arc(Around(centre, next, angle, z), mid, m_feeds.cut);
                    radius = next;
                }
                return angle;
            }

        private:
            void Add(MoveKind kind, const Vec3& to, double feed)
            {
                ToolpathMove move;
                move.kind = kind;
                move.to = to;
                move.feed = feed;
                m_toolpath.moves.push_back(move);
            }

            Toolpath& m_toolpath;
            const FeedSettings& m_feeds;
            double m_clearance;
            bool m_anticlockwise;
        };
    }

    ToolpathResult GenerateHolePocketToolpath(const HolePocketToolpathInput& input)
    {
        const SequenceStep* pOperation = input.operation;
        if (pOperation == nullptr || pOperation->type != StepType::HolePocketing)
            return Failure("This isn't a hole pocketing operation.");
        if (input.tool == nullptr || !(input.tool->diameter > 0.0))
            return Failure("Choose a tool on the Tool tab.");
        if (pOperation->features.empty())
            return Failure("There are no holes. Pick circles, hole faces or points on the Features tab.");

        const HolePocketingSettings& hole = pOperation->holePocketing;
        for (const std::vector<std::string>& problems : { ValidateHolePocketing(hole), ValidateFeeds(pOperation->feeds) })
        {
            if (!problems.empty())
                return Failure(problems.front());
        }

        double clearance = 0.0;
        std::string problem;
        if (!ClearanceHeight(pOperation->clearance, input.havePartMaxZ, input.partMaxZ, clearance, problem))
            return Failure(problem);

        const double toolRadius = input.tool->diameter / 2.0;
        const double stepover = ResolveSize(hole.stepover, input.tool->diameter);
        const double stepDown = (hole.stepDown == HoleStepDown::Percent)    ? input.tool->diameter * hole.stepDownPercent / 100.0
                                : (hole.stepDown == HoleStepDown::Distance) ? hole.stepDownDistance
                                                                            : hole.depth / hole.stepDownLevels;
        const int levels = (std::max)(1, static_cast<int>(std::ceil(hole.depth / stepDown - 1e-9)));
        const bool anticlockwise = (hole.direction != CutDirection::Conventional);

        ToolpathResult result;
        HoleBuilder path(result.toolpath, pOperation->feeds, clearance, anticlockwise);
        for (size_t n = 0; n < pOperation->features.size(); ++n)
        {
            const PickedGeometry& feature = pOperation->features[n];
            const std::string which = (pOperation->features.size() > 1) ? "Hole " + std::to_string(n + 1) + ": " : "";

            // Where it is and how big.
            const Vec3 point = WorldToFrame(input.frame, FeaturePoint(input.frame, feature));
            const bool round = feature.kind == PickKind::CircleEdge || feature.kind == PickKind::CylindricalFace;
            if (hole.useArcDiameter && !round)
                return Failure(which + "a point has no diameter. Untick Use Arc Dia as Hole Dia and give one.");
            const double holeRadius = hole.useArcDiameter ? feature.radius : hole.diameter / 2.0;
            const double wall = holeRadius - toolRadius;      // the tool centre's circle at the wall
            if (wall < -1e-9)
                return Failure(which + "the tool is wider than the hole.");

            const double top = (hole.location == CutGeometryLocation::AtBottom) ? point.z + hole.depth
                               : (hole.location == CutGeometryLocation::PickTop) ? hole.topZ
                                                                                 : point.z;
            const double approach = top + hole.approachDistance;
            if (!(clearance > approach))
                return Failure(which + "the clearance plane must be above the approach distance over the hole.");

            // The helix fits inside the wall. Its drop a turn comes from the pitch.
            const double helix = (std::min)(hole.helixDiameter / 2.0, (std::max)(0.0, wall));
            const double drop = (hole.pitch == HelixPitch::Height) ? hole.pitchHeight
                                                                    : 2.0 * kPi * helix * std::tan(hole.pitchAngle * kPi / 180.0);
            const Vec3 centre{ point.x, point.y, top };

            // Across, down to the approach height, and in.
            double angle = 0.0;
            const Vec3 start = Around(centre, helix, angle, approach);
            path.Transfer({ start.x, start.y, clearance });
            path.Rapid(start);

            for (int level = 1; level <= levels; ++level)
            {
                const double z = top - hole.depth * level / levels;

                // Down the helix, or straight down if there's no room for one.
                if (helix > 1e-6 && drop > 1e-9)
                {
                    angle = path.Helix(centre, helix, angle, z, drop);
                    if (helix + 1e-6 < wall)
                        path.Lap(centre, helix, angle);     // flat, unless the wall lap does it
                }
                else
                {
                    path.Feed({ path.At().x, path.At().y, z }, pOperation->feeds.plunge);
                }

                // Out to the wall, round it, and round again to clean up.
                if (wall > helix + 1e-6)
                    angle = path.SpiralOut(centre, helix, wall, angle, stepover);
                if (wall > 1e-6)
                {
                    if (helix <= 1e-6)
                        path.Feed(Around(centre, wall, angle, z), pOperation->feeds.cut);
                    path.Lap(centre, wall, angle);
                    if (hole.cleanUpEachLevel)
                        path.Lap(centre, wall, angle);
                }

                // Back in to the helix for the next level, through what's been cut.
                if (level < levels && helix + 1e-6 < wall)
                    path.Feed(Around(centre, helix, angle, z), pOperation->feeds.cut);
            }

            // In to the centre, clear of the wall, and up.
            path.Feed({ centre.x, centre.y, path.At().z }, pOperation->feeds.cut);
            path.Rapid({ centre.x, centre.y, clearance });
        }

        result.ok = !result.toolpath.moves.empty();
        if (!result.ok)
            result.problem = "No holes to cut.";
        return result;
    }
}
