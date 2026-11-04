// Origin.cpp : CAM origins, and the frames they define.

#include "Origin.h"

#include <algorithm>
#include <cmath>

namespace camcore
{
    namespace
    {
        // Shorter than this has no usable direction. Model sizes are in mm, so
        // this is far below anything Alibre models.
        const double kTinyLength = 1e-9;

        // A projected X shorter than this fraction of the original is too close
        // to the tool axis to give a reliable direction (about 3 degrees).
        const double kMinimumProjection = 0.05;

        // Assembly X is used for a calculated X unless it's within about 6
        // degrees of the tool axis.
        const double kWorldAxisProjection = 0.1;

        const double kEmptyPlaneHalfSize = 10.0;

        // A located plane or line within about half a degree of the drill
        // direction counts as parallel to it, and gives no distance.
        const double kParallelTolerance = 0.01;

        // A located distance beyond this (10 m) is taken as no distance.
        const double kLargestLocatedDistance = 10000.0;

        // v with its component along unit axis removed.
        Vec3 Perpendicular(const Vec3& v, const Vec3& axis)
        {
            return v - axis * Dot(v, axis);
        }

        FrameResult Failure(const std::string& message)
        {
            FrameResult result;
            result.message = message;
            return result;
        }
    }

    Vec3 operator+(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    Vec3 operator-(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    Vec3 operator*(const Vec3& v, double scale) { return { v.x * scale, v.y * scale, v.z * scale }; }
    double Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

    Vec3 Cross(const Vec3& a, const Vec3& b)
    {
        return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    }

    double Length(const Vec3& v)
    {
        return std::sqrt(Dot(v, v));
    }

    bool Normalise(const Vec3& v, Vec3& result)
    {
        const double length = Length(v);
        if (!(length > kTinyLength))
            return false;
        result = v * (1.0 / length);
        return true;
    }

    Vec3 FramePointToWorld(const Frame& frame, const Vec3& local)
    {
        return frame.origin + frame.xAxis * local.x + frame.yAxis * local.y + frame.zAxis * local.z;
    }

    bool PositionFromPick(const PickedGeometry& pick, Vec3& position)
    {
        switch (pick.kind)
        {
        case PickKind::CircleEdge:
            position = pick.point;
            return true;

        case PickKind::LineEdge:
            position = (Length(pick.clicked - pick.start) <= Length(pick.clicked - pick.end)) ? pick.start : pick.end;
            return true;

        default:
            return false;
        }
    }

    FrameResult BuildFrame(const OriginDefinition& definition)
    {
        const PickedGeometry& plane = definition.plane;
        const OriginOptions& options = definition.options;

        if (plane.kind == PickKind::None)
            return Failure("Pick a face or an edge for the plane.");

        Vec3 z;
        if (!Normalise(plane.direction, z))
            return Failure("The plane pick has no direction.");
        if (options.flipZ)
            z = z * -1.0;

        Vec3 position;
        if (definition.position.kind != PickKind::None)
        {
            if (!PositionFromPick(definition.position, position))
                return Failure("The position must be an edge.");
        }
        else if (!PositionFromPick(plane, position))
        {
            return Failure("Pick an edge for the position.");
        }

        Vec3 x;
        const PickedGeometry& xPick = definition.xDirection;
        if (xPick.kind != PickKind::None)
        {
            if (xPick.kind != PickKind::LineEdge)
                return Failure("The X direction must be a straight edge.");

            // X runs along the edge, away from the end nearer the click.
            const bool nearStart = Length(xPick.clicked - xPick.start) <= Length(xPick.clicked - xPick.end);
            const Vec3 along = nearStart ? xPick.end - xPick.start : xPick.start - xPick.end;

            const double length = Length(along);
            const Vec3 projected = Perpendicular(along, z);
            if (!(length > kTinyLength) || Length(projected) < kMinimumProjection * length || !Normalise(projected, x))
                return Failure("The X edge is parallel to the tool axis.");
        }
        else
        {
            const Vec3 fromWorldX = Perpendicular({ 1.0, 0.0, 0.0 }, z);
            const Vec3 fromWorldY = Perpendicular({ 0.0, 1.0, 0.0 }, z);
            const Vec3& chosen = (Length(fromWorldX) >= kWorldAxisProjection) ? fromWorldX : fromWorldY;
            if (!Normalise(chosen, x))
                return Failure("Could not calculate an X direction.");
        }

        // Each quarter turn about Z takes X to where Y was.
        const int turns = ((options.xQuarterTurns % 4) + 4) % 4;
        for (int i = 0; i < turns; ++i)
            x = Cross(z, x);

        FrameResult result;
        result.ok = true;
        result.frame.zAxis = z;
        result.frame.xAxis = x;
        result.frame.yAxis = Cross(z, x);
        result.frame.origin = position;
        result.frame.origin = FramePointToWorld(result.frame, options.offset);
        return result;
    }

    PlaneRectangle PlaneCoveringBox(const Frame& frame, const Vec3& boxMin, const Vec3& boxMax, double margin)
    {
        double uMin = 0.0, uMax = 0.0, vMin = 0.0, vMax = 0.0;
        for (int i = 0; i < 8; ++i)
        {
            const Vec3 corner{ (i & 1) ? boxMax.x : boxMin.x, (i & 2) ? boxMax.y : boxMin.y,
                               (i & 4) ? boxMax.z : boxMin.z };
            const Vec3 relative = corner - frame.origin;
            const double u = Dot(relative, frame.xAxis);
            const double v = Dot(relative, frame.yAxis);
            if (i == 0)
            {
                uMin = uMax = u;
                vMin = vMax = v;
            }
            else
            {
                uMin = std::min(uMin, u);
                uMax = std::max(uMax, u);
                vMin = std::min(vMin, v);
                vMax = std::max(vMax, v);
            }
        }

        const double longest = std::max(uMax - uMin, vMax - vMin);
        if (!(longest > kTinyLength))
        {
            uMin = uMin - kEmptyPlaneHalfSize;
            uMax = uMax + kEmptyPlaneHalfSize;
            vMin = vMin - kEmptyPlaneHalfSize;
            vMax = vMax + kEmptyPlaneHalfSize;
        }
        else
        {
            const double grow = longest * margin;
            uMin -= grow;
            uMax += grow;
            vMin -= grow;
            vMax += grow;
        }

        PlaneRectangle rectangle;
        rectangle.corners[0] = FramePointToWorld(frame, { uMin, vMin, 0.0 });
        rectangle.corners[1] = FramePointToWorld(frame, { uMax, vMin, 0.0 });
        rectangle.corners[2] = FramePointToWorld(frame, { uMax, vMax, 0.0 });
        rectangle.corners[3] = FramePointToWorld(frame, { uMin, vMax, 0.0 });
        return rectangle;
    }

    double GridSpacing(double length, int divisions)
    {
        if (!(length > kTinyLength) || divisions < 1)
            return 0.0;

        // The smallest 1, 2 or 5 step that gives no more than `divisions` squares.
        const double rough = length / divisions;
        const double power = std::pow(10.0, std::floor(std::log10(rough)));
        for (const double step : { 1.0, 2.0, 5.0, 10.0 })
        {
            if (step * power >= rough * (1.0 - 1e-9))
                return step * power;
        }
        return 10.0 * power;
    }

    std::vector<LineSegment> PlaneGrid(const Frame& frame, const PlaneRectangle& rectangle, int divisions)
    {
        std::vector<LineSegment> lines;

        // The rectangle's extent in the frame's own X and Y.
        const Vec3 low = rectangle.corners[0] - frame.origin;
        const Vec3 high = rectangle.corners[2] - frame.origin;
        const double uMin = Dot(low, frame.xAxis), uMax = Dot(high, frame.xAxis);
        const double vMin = Dot(low, frame.yAxis), vMax = Dot(high, frame.yAxis);

        const double spacing = GridSpacing(std::max(uMax - uMin, vMax - vMin), divisions);
        if (!(spacing > 0.0))
            return lines;

        // Lines fall on whole multiples of the spacing, so one passes through the origin.
        const double edgeGap = spacing * 1e-6;
        for (double u = std::ceil(uMin / spacing) * spacing; u < uMax - edgeGap; u += spacing)
        {
            if (u > uMin + edgeGap)
                lines.push_back({ FramePointToWorld(frame, { u, vMin, 0.0 }), FramePointToWorld(frame, { u, vMax, 0.0 }) });
        }
        for (double v = std::ceil(vMin / spacing) * spacing; v < vMax - edgeGap; v += spacing)
        {
            if (v > vMin + edgeGap)
                lines.push_back({ FramePointToWorld(frame, { uMin, v, 0.0 }), FramePointToWorld(frame, { uMax, v, 0.0 }) });
        }
        return lines;
    }

    bool HeightAlongZ(const Frame& frame, const LocatedGeometry& geometry, double& height)
    {
        const Vec3& z = frame.zAxis;
        const Vec3 fromOrigin = geometry.point - frame.origin;
        double distance = 0.0;

        Vec3 direction;
        switch (geometry.kind)
        {
        case LocatedGeometry::Kind::Plane:
        {
            // Where the drill axis through the origin meets the plane. A plane
            // parallel to the drill direction never meets it.
            if (!Normalise(geometry.direction, direction))
                return false;
            const double across = Dot(z, direction);
            if (std::fabs(across) < kParallelTolerance)
                return false;
            distance = Dot(fromOrigin, direction) / across;
            break;
        }

        case LocatedGeometry::Kind::Line:
        {
            // The line's point nearest the drill axis through the origin. A line
            // parallel to the drill direction has no nearest point.
            if (!Normalise(geometry.direction, direction))
                return false;
            const double along = Dot(direction, z);
            const double square = 1.0 - along * along;
            if (square < kParallelTolerance * kParallelTolerance)
                return false;
            const double s = (along * Dot(fromOrigin, z) - Dot(fromOrigin, direction)) / square;
            distance = Dot(fromOrigin + direction * s, z);
            break;
        }

        default:
            distance = Dot(fromOrigin, z);
            break;
        }

        if (std::fabs(distance) > kLargestLocatedDistance)
            return false;
        height = distance;
        return true;
    }

    double DistanceAlongZ(const Frame& frame, const LocatedGeometry& geometry)
    {
        double height = 0.0;
        return HeightAlongZ(frame, geometry, height) ? std::fabs(height) : 0.0;
    }
}
