// Facing.cpp : the Facing operation's own settings.

#include "Facing.h"

#include <cmath>
#include <string>

#include "Features.h"

namespace camcore
{
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

    AreaFeatureCheck CheckAreaFeatures(const std::vector<PickedGeometry>& features, const std::string& operation)
    {
        AreaFeatureCheck check;
        if (features.empty())
        {
            check.problem = "There are no features. Pick faces or edges.";
            return check;
        }
        for (const PickedGeometry& feature : features)
        {
            const bool usable = feature.kind == PickKind::PlanarFace || feature.kind == PickKind::LineEdge ||
                                feature.kind == PickKind::CircleEdge || feature.kind == PickKind::CurveEdge;
            if (!usable)
            {
                check.problem = operation + " can only use flat faces and edges.";
                return check;
            }
        }
        check.ok = true;
        return check;
    }
}
