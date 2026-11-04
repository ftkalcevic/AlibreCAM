// Facing.cpp : the Facing operation's own settings.

#include "Facing.h"

#include <cmath>
#include <string>

#include "Features.h"

namespace camcore
{
    namespace
    {
        FacingFeatureCheck Refuse(const std::string& problem)
        {
            FacingFeatureCheck check;
            check.problem = problem;
            return check;
        }

        // A unit direction's tilt from the frame's Z, as the sine of the angle.
        double TiltFromZ(const Frame& frame, const Vec3& direction)
        {
            Vec3 unit;
            return Normalise(direction, unit) ? Length(Cross(unit, frame.zAxis)) : 1.0;
        }

        // How far a unit direction leans along the frame's Z, as the cosine.
        double LeanAlongZ(const Frame& frame, const Vec3& direction)
        {
            Vec3 unit;
            return Normalise(direction, unit) ? std::fabs(Dot(unit, frame.zAxis)) : 1.0;
        }
    }

    double ResolveSize(const ToolRelativeSize& size, double toolDiameter)
    {
        return size.usePercent ? toolDiameter * size.percent / 100.0 : size.distance;
    }

    std::vector<std::string> ValidateFacing(const FacingSettings& settings)
    {
        // Only the value in use is checked. A pass can't step more than the tool's width.
        std::vector<std::string> problems;
        const ToolRelativeSize& step = settings.stepDistance;
        if (step.usePercent ? !(step.percent > 0.0 && step.percent <= 100.0) : !(step.distance > 0.0))
        {
            problems.push_back(step.usePercent ? "The step distance must be more than 0% and at most 100% of the tool."
                                               : "The step distance must be more than 0.");
        }

        const ToolRelativeSize& over = settings.overtravel;
        if (over.usePercent ? !(over.percent >= 0.0) : !(over.distance >= 0.0))
            problems.push_back("The overtravel can't be negative.");

        return problems;
    }

    FacingFeatureCheck CheckFacingFeatures(const Frame& frame, const std::vector<PickedGeometry>& features)
    {
        if (features.empty())
            return Refuse("There are no features. Pick one flat face, or the edges around the area to face.");

        size_t faces = 0;
        for (const PickedGeometry& feature : features)
        {
            if (feature.kind == PickKind::PlanarFace)
                ++faces;
            else if (feature.kind != PickKind::LineEdge && feature.kind != PickKind::CircleEdge)
                return Refuse("Facing can only use a flat face, or straight and circular edges.");
        }

        if (faces > 1)
            return Refuse("Facing can use only one face.");
        if (faces == 1 && features.size() > 1)
            return Refuse("Facing uses either one face or edges, not both.");

        FacingFeatureCheck check;
        if (faces == 1)
        {
            const PickedGeometry& face = features.front();
            if (TiltFromZ(frame, face.direction) > kFacingParallelTolerance)
                return Refuse("The face isn't parallel to the origin's XY plane.");
            check.ok = true;
            check.level = WorldToFrame(frame, face.point).z;
            return check;
        }

        // Every edge must lie flat, and all at one level.
        for (size_t i = 0; i < features.size(); ++i)
        {
            const PickedGeometry& edge = features[i];
            const std::string which = "Edge " + std::to_string(i + 1);
            double level = 0.0;

            if (edge.kind == PickKind::LineEdge)
            {
                if (LeanAlongZ(frame, edge.end - edge.start) > kFacingParallelTolerance)
                    return Refuse(which + " isn't parallel to the origin's XY plane.");
                level = WorldToFrame(frame, edge.start).z;
            }
            else
            {
                if (TiltFromZ(frame, edge.direction) > kFacingParallelTolerance)
                    return Refuse(which + " isn't parallel to the origin's XY plane.");
                level = WorldToFrame(frame, edge.point).z;
            }

            if (i == 0)
                check.level = level;
            else if (std::fabs(level - check.level) > kFacingLevelTolerance)
                return Refuse("The edges aren't all on the same plane. " + which + " is at a different height.");
        }

        check.ok = true;
        return check;
    }
}
