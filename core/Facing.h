// Facing.h : the Facing operation's own settings. No Alibre or MFC code belongs here.
//
// Distances are in millimetres.

#pragma once

#include <string>
#include <vector>

#include "Features.h"
#include "Origin.h"

namespace camcore
{
    enum class CutDirection
    {
        Climb,
        Conventional,
        Mixed           // both, cutting in each direction
    };

    enum class FacingCutType
    {
        Linear,         // straight passes across the area
        Spiral          // round the area, working inwards or outwards
    };

    // A size given as a share of the tool's diameter or as a distance. Both
    // values are kept, so switching back restores the old one.
    struct ToolRelativeSize
    {
        bool usePercent = true;
        double percent = 0.0;       // of the tool's diameter
        double distance = 0.0;
    };

    // The Facing tab.
    struct FacingSettings
    {
        CutDirection direction = CutDirection::Climb;
        ToolRelativeSize stepDistance{ true, 25.0, 2.0 };   // across, between passes
        ToolRelativeSize overtravel{ true, 50.0, 3.0 };     // past the area's edge
        FacingCutType cutType = FacingCutType::Linear;
        double cutAngle = 0.0;                              // degrees: 90 runs along the origin's X, 0 along its Y
        StartCorner start = StartCorner::TopLeft;           // seen from the origin's +Z
        bool arcStepOver = false;                           // Mixed Linear steps over on half circles
    };

    // The size in mm for a tool of this diameter.
    double ResolveSize(const ToolRelativeSize& size, double toolDiameter);

    // Returns a sentence for each problem. Empty means the settings are valid.
    std::vector<std::string> ValidateFacing(const FacingSettings& settings);

    // Within these, a face or edge counts as parallel to the origin's XY plane
    // (about half a degree) and edges count as being at the same level (mm).
    constexpr double kFacingParallelTolerance = 0.01;
    constexpr double kFacingLevelTolerance = 0.01;

    struct FacingFeatureCheck
    {
        bool ok = false;
        std::string problem;    // why not. Empty when ok.
        double level = 0.0;     // the features' Z in the frame, when ok
    };

    // Facing's features are either one flat face, or any number of straight or
    // circular edges, which needn't close. The face must be parallel to the
    // frame's XY plane. The edges must all lie in one plane parallel to it, so a
    // straight edge must run across Z and a circle's axis along Z.
    // operation names the operation in the problems, as Profiling uses the same rules.
    FacingFeatureCheck CheckFacingFeatures(const Frame& frame, const std::vector<PickedGeometry>& features,
                                           const std::string& operation = "Facing");
}
