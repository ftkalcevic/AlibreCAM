// Stock.h : the job's stock, the material the parts are cut from. No Alibre or MFC code belongs here.
//
// Everything is in the assembly's coordinate system, in millimetres. A job has
// at most one stock.

#pragma once

#include <string>
#include <vector>

#include "Boundary.h"
#include "Origin.h"

namespace camcore
{
    enum class StockKind
    {
        None,
        Box,        // a box from entered dimensions and position
        PartBox,    // a box around some or all of the parts, plus six offsets
        Imported    // another Alibre part, linked by its file and placed by position and rotation
    };

    // As shown in the tree: "Box Stock", "Part Box Stock", "Imported Stock". Empty for None.
    std::string StockKindTitle(StockKind kind);

    // A part a Part Box Stock wraps.
    struct StockPart
    {
        PersistentKey occurrenceKey;
        std::string name;
    };

    struct Stock
    {
        StockKind kind = StockKind::None;

        // Box: the corner with the lowest X, Y and Z, and the size along each axis.
        Vec3 boxCorner;
        Vec3 boxSize{ 100.0, 100.0, 25.0 };

        // Part Box: the parts it wraps, empty for every part. Each offset adds to
        // the outside of the parts' bounding box on its side.
        std::vector<StockPart> parts;
        double offsetPlusX = 1.0;
        double offsetMinusX = 1.0;
        double offsetPlusY = 1.0;
        double offsetMinusY = 1.0;
        double offsetPlusZ = 1.0;
        double offsetMinusZ = 0.0;

        // Imported: the part file, and where it's placed. The part is turned about
        // the assembly's X, then Y, then Z axes (degrees), then moved by the position.
        std::string partPath;
        Vec3 position;
        Vec3 rotation;
    };

    // Returns a sentence for each problem. Empty means the stock is valid.
    std::vector<std::string> ValidateStock(const Stock& stock);

    // An axis-aligned box in the assembly.
    struct StockBox
    {
        Vec3 lower;
        Vec3 upper;
    };

    // A Box Stock's box.
    StockBox BoxStockBox(const Stock& stock);

    // A Part Box Stock's box, from the bounding box of its parts.
    StockBox PartBoxStockBox(const Stock& stock, const Vec3& partsLower, const Vec3& partsUpper);

    // Where an imported part sits: its own origin and axes in the assembly.
    Frame ImportedStockFrame(const Stock& stock);

    // The 12 edges of a box.
    std::vector<LineSegment> BoxEdges(const StockBox& box);

    // The edges of a triangle mesh worth drawing: those on its outside, and those
    // between triangles that meet at more than angleDegrees, such as a box's
    // corners, but not the lines across a flat face or round a smooth curve. The
    // mesh is three points to a triangle. Points within tolerance are one.
    std::vector<LineSegment> MeshOutlineEdges(const std::vector<Vec3>& triangles, double angleDegrees,
                                              double tolerance = 1e-6);

    // The box around some points. False if there are none.
    bool PointsBox(const std::vector<Vec3>& points, StockBox& box);

    // A box's 8 corners.
    std::vector<Vec3> BoxCorners(const StockBox& box);

    // The highest of some points along a frame's Z. False if there are none.
    bool HighestAlongZ(const Frame& frame, const std::vector<Vec3>& points, double& top);

    // The box seen from above along the frame's Z: the outline of its corners
    // projected onto the frame's XY plane, anticlockwise, as a closed path in the
    // frame at the box's highest point along Z. A rectangle unless the frame is tilted.
    Path StockFootprint(const Frame& frame, const StockBox& box);
}
