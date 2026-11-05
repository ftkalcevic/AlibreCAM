// FacingToolpath.cpp : calculates a Facing operation's toolpath.

#include "FacingToolpath.h"

#include "PathBuilder.h"

#include <algorithm>
#include <cmath>

namespace camcore
{
    namespace
    {
        const double kPi = 3.14159265358979323846;

        // A spiral never needs more legs than this. It's only a guard.
        const int kMostSpiralLegs = 100000;

        Vec3 Plus(const Vec3& a, const Vec3& b, double scale) { return { a.x + b.x * scale, a.y + b.y * scale, a.z }; }

        // Seen from +Z, true if turning from a to b is anticlockwise.
        bool TurnsLeft(const Vec3& a, const Vec3& b) { return a.x * b.y - a.y * b.x > 0.0; }

        // The unit direction from a to b in XY, or false if they're the same point.
        bool DirectionXY(const Vec3& a, const Vec3& b, Vec3& direction)
        {
            const double dx = b.x - a.x, dy = b.y - a.y;
            const double length = std::sqrt(dx * dx + dy * dy);
            if (!(length > 1e-9))
                return false;
            direction = { dx / length, dy / length, 0.0 };
            return true;
        }

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

        // Where the boundary crosses the line v, as its lowest and highest u.
        bool SpanAt(const Path& boundary, const PassAxes& axes, double v, double& low, double& high)
        {
            const std::vector<double> crossings = PathCrossings(boundary, axes.along, axes.across, v);
            if (crossings.empty())
                return false;
            low = *std::min_element(crossings.begin(), crossings.end());
            high = *std::max_element(crossings.begin(), crossings.end());
            return true;
        }

        void LinearPasses(PathBuilder& path, const Path& boundary, const PassAxes& axes, const FacingSettings& facing,
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

            // Away from the geometry, an entry comes from the side the passes step away
            // from, outside the first pass. An exit goes the way they step, out past
            // the last pass.
            const Vec3 entryAway{ -axes.across.x * stepSign, -axes.across.y * stepSign, 0.0 };
            const Vec3 exitAway{ axes.across.x * stepSign, axes.across.y * stepSign, 0.0 };

            // Passes beyond the boundary, in the overtravel, take the span at its edge.
            const double inside = (vMax - vMin) * 1e-6;
            bool havePass = false;
            double lastU = 0.0, lastV = 0.0;
            Vec3 lastEnd, lastDirection;
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
                const Vec3 direction = forward ? axes.along : Vec3{ -axes.along.x, -axes.along.y, 0.0 };

                if (facing.direction == CutDirection::Mixed)
                {
                    // Backwards and forwards, stepping over at the cut feed and square
                    // to the passes, at whichever of the two passes' ends is further out.
                    // Where the last pass ended further in, it carries on out along its
                    // line first, even though that cuts air.
                    if (!havePass)
                    {
                        path.Enter(start, direction, entryAway);
                    }
                    else
                    {
                        // The last pass ended at the end this one starts from.
                        const double cornerU = forward ? (std::min)(lastU, from) : (std::max)(lastU, from);
                        if (cornerU != lastU)
                            path.Cut(axes.At(cornerU, lastV, z));
                        if (facing.arcStepOver)
                        {
                            // A half circle across the step, bulging out past the ends.
                            const Vec3 from = axes.At(cornerU, lastV, z);
                            const Vec3 centre = axes.At(cornerU, (lastV + v) / 2.0, z);
                            path.ArcTo(axes.At(cornerU, v, z), centre, !TurnsLeft(from - centre, lastDirection));
                        }
                        else
                        {
                            path.Cut(axes.At(cornerU, v, z));
                        }
                        if (cornerU != from)
                            path.Cut(start);
                    }
                    path.Cut(end);
                    havePass = true;
                    lastU = to;
                    lastV = v;
                    lastEnd = end;
                    lastDirection = direction;
                    forward = !forward;
                }
                else
                {
                    // One way only, so each pass is its own cut, with its own entry
                    // and exit, starting again from the clearance plane.
                    path.Enter(start, direction, entryAway);
                    path.Cut(end);
                    path.Exit(end, direction, exitAway);
                }
            }

            // Mixed is one cut a level, leaving from the end of its last pass.
            if (facing.direction == CutDirection::Mixed && havePass)
                path.Exit(lastEnd, lastDirection, exitAway);
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

            std::vector<Vec3> path3;
            for (const Vec3& point : points)
            {
                const double u = startHighU ? right - point.x : left + point.x;
                const double v = startHighV ? top - point.y : bottom + point.y;
                path3.push_back(axes.At(u, v, z));
            }

            // The first and last legs that go somewhere give the entry's and exit's
            // directions. Outside the loop is to the right going anticlockwise, and to
            // the left going clockwise.
            Vec3 first{ 1.0, 0.0, 0.0 }, last{ 1.0, 0.0, 0.0 };
            for (size_t i = 1; i < path3.size(); ++i)
            {
                if (DirectionXY(path3[i - 1], path3[i], first))
                    break;
            }
            for (size_t i = path3.size() - 1; i > 0; --i)
            {
                if (DirectionXY(path3[i - 1], path3[i], last))
                    break;
            }
            const double outside = wantAnticlockwise ? 1.0 : -1.0;
            const Vec3 firstAway{ first.y * outside, -first.x * outside, 0.0 };
            const Vec3 lastAway{ last.y * outside, -last.x * outside, 0.0 };

            path.Enter(path3.front(), first, firstAway);
            for (size_t i = 1; i < path3.size(); ++i)
                path.Cut(path3[i]);
            path.Exit(path3.back(), last, lastAway);
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

        const Path& boundary = pOperation->boundary;
        if (boundary.size() < 2)
            return Failure("There's no boundary to face.");

        const FacingSettings& facing = pOperation->facing;
        const std::vector<std::string> facingProblems = ValidateFacing(facing);
        if (!facingProblems.empty())
            return Failure(facingProblems.front());
        const std::vector<std::string> feedProblems = ValidateFeeds(pOperation->feeds);
        if (!feedProblems.empty())
            return Failure(feedProblems.front());

        const std::vector<std::string> levelProblems = ValidateCutLevels(pOperation->cutLevels);
        if (!levelProblems.empty())
            return Failure(levelProblems.front());
        const EntryExitSettings& entryExit = pOperation->entryExit;
        const std::vector<std::string> entryProblems = ValidateEntryExit(entryExit);
        if (!entryProblems.empty())
            return Failure(entryProblems.front());

        // The boundary is the cut geometry, at its own height.
        const double z = boundary.front().point.z;
        const std::vector<CutLevel> levels = CutLevelHeights(pOperation->cutLevels, z);
        double clearance = 0.0;
        std::string clearanceProblem;
        if (!ClearanceHeight(pOperation->clearance, input.havePartMaxZ, input.partMaxZ, clearance, clearanceProblem))
            return Failure(clearanceProblem);
        if (!(clearance > CutTopZ(pOperation->cutLevels, z)))
            return Failure("The clearance plane must be above the top of the cut.");
        if (entryExit.entry == EntryMode::AlongPath3D && !(clearance > levels.front().z + entryExit.alongPathHeight))
            return Failure("The along path height reaches the clearance plane.");

        const double step = ResolveSize(facing.stepDistance, input.tool->diameter);
        const double overtravel = ResolveSize(facing.overtravel, input.tool->diameter);
        if (!(step > 0.0))
            return Failure("The step distance works out as 0.");

        PassAxes axes;
        axes.along = PassDirection(facing.cutAngle);
        axes.across = { -axes.along.y, axes.along.x, 0.0 };

        // The boundary's extent along and across the passes, and in the frame,
        // counting how far its arcs reach.
        double uMin = 0.0, uMax = 0.0, vMin = 0.0, vMax = 0.0, xMin = 0.0, xMax = 0.0, yMin = 0.0, yMax = 0.0;
        PathExtent(boundary, axes.along, uMin, uMax);
        PathExtent(boundary, axes.across, vMin, vMax);
        PathExtent(boundary, { 1.0, 0.0, 0.0 }, xMin, xMax);
        PathExtent(boundary, { 0.0, 1.0, 0.0 }, yMin, yMax);

        // The Cut Start corner, seen from +Z with X right and Y up, decides which
        // side and which end the cut starts from.
        const bool top = (facing.start == StartCorner::TopLeft || facing.start == StartCorner::TopRight);
        const bool left = (facing.start == StartCorner::TopLeft || facing.start == StartCorner::BottomLeft);
        const Vec3 corner{ left ? xMin : xMax, top ? yMax : yMin, z };
        const bool startHighV = std::fabs(axes.V(corner) - vMax) < std::fabs(axes.V(corner) - vMin);
        const bool startHighU = std::fabs(axes.U(corner) - uMax) < std::fabs(axes.U(corner) - uMin);

        // The whole area is cut at each level, from the top down, lifting to the
        // clearance plane between levels. Rough and finish levels share the feeds
        // until each has its own.
        // Entry and exit are at every level, or only into the first and out of the last.
        ToolpathResult result;
        for (size_t l = 0; l < levels.size(); ++l)
        {
            const CutLevel& level = levels[l];
            PathBuilder path(result.toolpath, pOperation->feeds, clearance, level.z);
            path.SetEntryExit(&entryExit, entryExit.eachCutLevel || l == 0,
                              entryExit.eachCutLevel || l + 1 == levels.size());
            if (facing.cutType == FacingCutType::Spiral)
                Spiral(path, axes, facing, uMin, uMax, vMin, vMax, startHighV, startHighU, step, overtravel, level.z);
            else
                LinearPasses(path, boundary, axes, facing, vMin, vMax, startHighV, startHighU, step, overtravel,
                             level.z);

            if (!result.toolpath.moves.empty() && result.toolpath.moves.back().to.z != clearance)
                path.Leave();
        }

        result.ok = !result.toolpath.moves.empty();
        if (!result.ok)
            result.problem = "No passes fit the boundary.";
        return result;
    }
}
