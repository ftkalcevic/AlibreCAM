// Features.h : the geometry an operation machines, and the order it goes in.
// No Alibre or MFC code belongs here.
//
// Features are picks in assembly coordinates and millimetres. Sorting works in
// the operation's origin frame, looking down its Z axis.

#pragma once

#include <vector>

#include "Origin.h"

namespace camcore
{
    // What an operation's Features tab accepts.
    enum class FeatureFilter
    {
        Points,     // Drill: anything that reduces to a point
        FacingArea  // Facing: flat faces, and edges around the area. The edges
                    // needn't make a closed shape.
    };

    bool FeatureAllowed(FeatureFilter filter, PickKind kind);

    // Where a feature is machined, in assembly coordinates. A circle edge or
    // design point is its point. A cylindrical face is the centre of its end
    // nearer the frame's +Z.
    Vec3 FeaturePoint(const Frame& frame, const PickedGeometry& feature);

    // A point in assembly coordinates, in the frame's own X, Y and Z.
    Vec3 WorldToFrame(const Frame& frame, const Vec3& world);

    // True if both picks are the same item in the model.
    bool SameFeature(const PickedGeometry& a, const PickedGeometry& b);

    // The list after the view's selection changes from `before` to `after`. Only
    // the change counts: a feature that was selected and no longer is goes, and
    // one newly selected is added at the end, in selection order. Everything else
    // keeps its place, including features the view can't show, such as design
    // points, and features the selection hasn't been told about yet.
    std::vector<PickedGeometry> ApplySelectionChange(const std::vector<PickedGeometry>& list,
                                                     const std::vector<PickedGeometry>& before,
                                                     const std::vector<PickedGeometry>& after);

    enum class FeatureOrder
    {
        MinimumDistance,    // from the first feature, always to the nearest one left
        RowsXFirst,         // zigzag rows along X, stepping in Y
        RowsYFirst          // zigzag columns along Y, stepping in X
    };

    // Seen from the frame's +Z, with X to the right and Y up.
    enum class StartCorner
    {
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight
    };

    // Features closer than this across a row count as being in the same row.
    constexpr double kFeatureRowTolerance = 0.5;

    // The new order as indices into points, which are in frame coordinates. Only
    // X and Y are used. The corner applies to the row orders only.
    std::vector<size_t> SortFeatures(const std::vector<Vec3>& points, FeatureOrder order, StartCorner corner);
}
