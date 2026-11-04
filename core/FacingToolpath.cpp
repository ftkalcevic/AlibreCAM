// FacingToolpath.cpp : calculates a Facing operation's toolpath.

#include "FacingToolpath.h"

#include <algorithm>
#include <cmath>

namespace camcore
{
    namespace
    {
        const double kPi = 3.14159265358979323846;

        // A spiral never needs more legs than this. It's only a guard.
        const int kMostSpiralLegs = 100000;

        ToolpathResult Failure(const std::string& problem)
        {
            ToolpathResult result;
            result.problem = problem;
            return result;
        }

        // Points in the passes' own axes: u along the passes, v across them,
        // to the left of u. Both are in the frame's XY.
        struct PassAxes
        {
            Vec3 along;
            Vec3 across;

            double U(const Vec3& p) const { return p.x * along.x + p.y * along.y; }
            double V(const Vec3& p) const { return p.x * across.x + p.y * across.y; }
            Vec3 At(double u, double v, double z) const
            {
                return { u * along.x + v * across.x, u * along.y + v * across.y, z };
            }
        };

        class PathBuilder
        {
        public:
            PathBuilder(Toolpath& toolpath, const FeedSettings& feeds, double clearance, double cutZ)
                : m_toolpath(toolpath), m_feeds(feeds), m_clearance(clearance), m_cutZ(cutZ)
            {
            }

            // Across at the clearance plane to above the point, then down to cut.
            void ArriveAt(const Vec3& point)
            {
                const Vec3 above{ point.x, point.y, m_clearance };
                if (m_toolpath.moves.empty() || m_feeds.transferAtRapid)
                    Add(MoveKind::Rapid, above, 0.0);
                else
                    Add(MoveKind::Feed, above, m_feeds.transfer);
                Add(MoveKind::Feed, { point.x, point.y, m_cutZ }, m_feeds.plunge);
            }

            void Cut(const Vec3& point) { Add(MoveKind::Feed, { point.x, point.y, m_cutZ }, m_feeds.cut); }

            void Leave()
            {
                const Vec3& at = m_toolpath.moves.back().to;
                Add(MoveKind::Rapid, { at.x, at.y, m_clearance }, 0.0);
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
            double m_cutZ;
        };

        // Where the boundary crosses the line v, as its lowest and highest u.
        bool SpanAt(const Polyline& boundary, const PassAxes& axes, double v, double& low, double& high)
        {
            bool found = false;
            for (size_t i = 0; i < boundary.size(); ++i)
            {
                const Vec3& p = boundary[i];
                const Vec3& q = boundary[(i + 1) % boundary.size()];
                const double pv = axes.V(p), qv = axes.V(q);

                // Each edge counts from its lower end up to, but not at, its upper end.
                if (!((pv <= v && v < qv) || (qv <= v && v < pv)))
                    continue;

                const double pu = axes.U(p), qu = axes.U(q);
                const double u = pu + (v - pv) * (qu - pu) / (qv - pv);
                low = found ? (std::min)(low, u) : u;
                high = found ? (std::max)(high, u) : u;
                found = true;
            }
            return found;
        }

        void LinearPasses(PathBuilder& path, const Polyline& boundary, const PassAxes& axes, const FacingSettings& facing,
                          double vMin, double vMax, bool startHighV, bool startHighU, double step, double overtravel,
                          double z)
        {
            // Passes run from one side's overtravel to the other's, evenly spaced
            // no more than the step apart.
            const double low = vMin - overtravel;
            const double high = vMax + overtravel;
            const int intervals = (std::max)(1, static_cast<int>(std::ceil((high - low) / step - 1e-9)));
            const double spacing = (high - low) / intervals;
            const double stepSign = startHighV ? -1.0 : 1.0;

            // The uncut material is on the side the passes step towards. With a
            // clockwise spindle the teeth on the cutter's right move against the
            // feed, so climb keeps the material on the right. Right of +u is -v.
            const bool climbForward = (stepSign < 0.0);
            bool forward = (facing.direction == CutDirection::Conventional) ? !climbForward : climbForward;
            if (facing.direction == CutDirection::Mixed)
                forward = !startHighU;

            // Passes beyond the boundary, in the overtravel, take the span at its edge.
            const double inside = (vMax - vMin) * 1e-6;
            bool havePass = false;
            double lastU = 0.0, lastV = 0.0;
            for (int i = 0; i <= intervals; ++i)
            {
                const double v = startHighV ? high - i * spacing : low + i * spacing;
                double spanLow = 0.0, spanHigh = 0.0;
                if (!SpanAt(boundary, axes, (std::max)(vMin + inside, (std::min)(v, vMax - inside)), spanLow, spanHigh))
                    continue;

                const double from = forward ? spanLow - overtravel : spanHigh + overtravel;
                const double to = forward ? spanHigh + overtravel : spanLow - overtravel;
                const Vec3 start = axes.At(from, v, z);
                const Vec3 end = axes.At(to, v, z);

                if (facing.direction == CutDirection::Mixed)
                {
                    // Backwards and forwards, stepping over at the cut feed and square
                    // to the passes, at whichever of the two passes' ends is further out.
                    // Where the last pass ended further in, it carries on out along its
                    // line first, even though that cuts air.
                    if (!havePass)
                    {
                        path.ArriveAt(start);
                    }
                    else
                    {
                        // The last pass ended at the end this one starts from.
                        const double cornerU = forward ? (std::min)(lastU, from) : (std::max)(lastU, from);
                        if (cornerU != lastU)
                            path.Cut(axes.At(cornerU, lastV, z));
                        path.Cut(axes.At(cornerU, v, z));
                        if (cornerU != from)
                            path.Cut(start);
                    }
                    path.Cut(end);
                    havePass = true;
                    lastU = to;
                    lastV = v;
                    forward = !forward;
                }
                else
                {
                    // One way only, so each pass starts again from the clearance plane.
                    path.ArriveAt(start);
                    path.Cut(end);
                    path.Leave();
                }
            }
        }

        // A rectangular spiral in a w by h rectangle, starting at (0, 0) and
        // working inwards a step at a time, anticlockwise (first along +x) or
        // clockwise (first along +y).
        std::vector<Vec3> SpiralPoints(double w, double h, double step, bool anticlockwise)
        {
            std::vector<Vec3> points{ { 0.0, 0.0, 0.0 } };
            double left = 0.0, right = w, bottom = 0.0, top = h;

            for (int leg = 0; leg < kMostSpiralLegs; ++leg)
            {
                const int side = leg % 4;
                if (anticlockwise)
                {
                    if (side == 0) { points.push_back({ right, bottom, 0.0 }); bottom += step; if (bottom > top) break; }
                    if (side == 1) { points.push_back({ right, top, 0.0 });    right -= step;  if (right < left) break; }
                    if (side == 2) { points.push_back({ left, top, 0.0 });     top -= step;    if (top < bottom) break; }
                    if (side == 3) { points.push_back({ left, bottom, 0.0 });  left += step;   if (left > right) break; }
                }
                else
                {
                    if (side == 0) { points.push_back({ left, top, 0.0 });     left += step;   if (left > right) break; }
                    if (side == 1) { points.push_back({ right, top, 0.0 });    top -= step;    if (top < bottom) break; }
                    if (side == 2) { points.push_back({ right, bottom, 0.0 }); right -= step;  if (right < left) break; }
                    if (side == 3) { points.push_back({ left, bottom, 0.0 });  bottom += step; if (bottom > top) break; }
                }
            }
            return points;
        }

        void Spiral(PathBuilder& path, const PassAxes& axes, const FacingSettings& facing, double uMin, double uMax,
                    double vMin, double vMax, bool startHighV, bool startHighU, double step, double overtravel, double z)
        {
            const double left = uMin - overtravel, right = uMax + overtravel;
            const double bottom = vMin - overtravel, top = vMax + overtravel;

            // The spiral is made from the low corner, then mirrored to the start
            // corner. Each mirror turns it the other way, so allow for that.
            // Working inwards, the material is inside the loop. Clockwise keeps it on
            // the cutter's right, which is climb.
            const bool wantAnticlockwise = (facing.direction == CutDirection::Conventional);
            const bool mirrored = (startHighU != startHighV);
            const std::vector<Vec3> points =
                SpiralPoints(right - left, top - bottom, step, mirrored ? !wantAnticlockwise : wantAnticlockwise);

            for (size_t i = 0; i < points.size(); ++i)
            {
                const double u = startHighU ? right - points[i].x : left + points[i].x;
                const double v = startHighV ? top - points[i].y : bottom + points[i].y;
                if (i == 0)
                    path.ArriveAt(axes.At(u, v, z));
                else
                    path.Cut(axes.At(u, v, z));
            }
            path.Leave();
        }
    }

    Vec3 PassDirection(double cutAngleDegrees)
    {
        const double angle = cutAngleDegrees * kPi / 180.0;
        return { std::sin(angle), std::cos(angle), 0.0 };
    }

    ToolpathResult GenerateFacingToolpath(const FacingToolpathInput& input)
    {
        const SequenceStep* pOperation = input.operation;
        if (pOperation == nullptr || pOperation->type != StepType::Facing)
            return Failure("This isn't a facing operation.");
        if (input.tool == nullptr || !(input.tool->diameter > 0.0))
            return Failure("Choose a tool on the Tool tab.");

        const Polyline& boundary = pOperation->boundary;
        if (boundary.size() < 3)
            return Failure("There's no boundary to face.");

        const FacingSettings& facing = pOperation->facing;
        const std::vector<std::string> facingProblems = ValidateFacing(facing);
        if (!facingProblems.empty())
            return Failure(facingProblems.front());
        const std::vector<std::string> feedProblems = ValidateFeeds(pOperation->feeds);
        if (!feedProblems.empty())
            return Failure(feedProblems.front());

        // One level for now, at the boundary's own height.
        const double z = boundary.front().z;
        double clearance = 0.0;
        std::string clearanceProblem;
        if (!ClearanceHeight(pOperation->clearance, input.havePartMaxZ, input.partMaxZ, clearance, clearanceProblem))
            return Failure(clearanceProblem);
        if (!(clearance > z))
            return Failure("The clearance plane must be above the face being cut.");

        const double step = ResolveSize(facing.stepDistance, input.tool->diameter);
        const double overtravel = ResolveSize(facing.overtravel, input.tool->diameter);
        if (!(step > 0.0))
            return Failure("The step distance works out as 0.");

        PassAxes axes;
        axes.along = PassDirection(facing.cutAngle);
        axes.across = { -axes.along.y, axes.along.x, 0.0 };

        // The boundary's extent along and across the passes, and in the frame.
        double uMin = 0.0, uMax = 0.0, vMin = 0.0, vMax = 0.0, xMin = 0.0, xMax = 0.0, yMin = 0.0, yMax = 0.0;
        for (size_t i = 0; i < boundary.size(); ++i)
        {
            const Vec3& p = boundary[i];
            const double u = axes.U(p), v = axes.V(p);
            uMin = (i == 0) ? u : (std::min)(uMin, u);
            uMax = (i == 0) ? u : (std::max)(uMax, u);
            vMin = (i == 0) ? v : (std::min)(vMin, v);
            vMax = (i == 0) ? v : (std::max)(vMax, v);
            xMin = (i == 0) ? p.x : (std::min)(xMin, p.x);
            xMax = (i == 0) ? p.x : (std::max)(xMax, p.x);
            yMin = (i == 0) ? p.y : (std::min)(yMin, p.y);
            yMax = (i == 0) ? p.y : (std::max)(yMax, p.y);
        }

        // The Cut Start corner, seen from +Z with X right and Y up, decides which
        // side and which end the cut starts from.
        const bool top = (facing.start == StartCorner::TopLeft || facing.start == StartCorner::TopRight);
        const bool left = (facing.start == StartCorner::TopLeft || facing.start == StartCorner::BottomLeft);
        const Vec3 corner{ left ? xMin : xMax, top ? yMax : yMin, z };
        const bool startHighV = std::fabs(axes.V(corner) - vMax) < std::fabs(axes.V(corner) - vMin);
        const bool startHighU = std::fabs(axes.U(corner) - uMax) < std::fabs(axes.U(corner) - uMin);

        ToolpathResult result;
        PathBuilder path(result.toolpath, pOperation->feeds, clearance, z);
        if (facing.cutType == FacingCutType::Spiral)
            Spiral(path, axes, facing, uMin, uMax, vMin, vMax, startHighV, startHighU, step, overtravel, z);
        else
            LinearPasses(path, boundary, axes, facing, vMin, vMax, startHighV, startHighU, step, overtravel, z);

        if (facing.cutType == FacingCutType::Linear && facing.direction == CutDirection::Mixed &&
            !result.toolpath.moves.empty())
            path.Leave();

        result.ok = !result.toolpath.moves.empty();
        if (!result.ok)
            result.problem = "No passes fit the boundary.";
        return result;
    }
}
