// Origin.h : CAM origins, and the frames they define. No Alibre or MFC code belongs here.
//
// Everything is in the assembly's coordinate system, in millimetres. The add-on
// reads picked faces and edges from Alibre and converts them before they get here.

#pragma once

#include <string>

namespace camcore
{
    struct Vec3
    {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };

    Vec3 operator+(const Vec3& a, const Vec3& b);
    Vec3 operator-(const Vec3& a, const Vec3& b);
    Vec3 operator*(const Vec3& v, double scale);
    double Dot(const Vec3& a, const Vec3& b);
    Vec3 Cross(const Vec3& a, const Vec3& b);
    double Length(const Vec3& v);

    // Returns false, and leaves result alone, if v is too short to have a direction.
    bool Normalise(const Vec3& v, Vec3& result);

    // A position and a right-handed set of unit axes. Z is the tool axis.
    struct Frame
    {
        Vec3 origin;
        Vec3 xAxis{ 1.0, 0.0, 0.0 };
        Vec3 yAxis{ 0.0, 1.0, 0.0 };
        Vec3 zAxis{ 0.0, 0.0, 1.0 };
    };

    // A frame point or direction in assembly coordinates.
    Vec3 FramePointToWorld(const Frame& frame, const Vec3& local);

    // A persistent Alibre key as it's saved: the array's element type and its bytes in hex.
    struct PersistentKey
    {
        int elementType = 0;
        std::string hex;

        bool IsEmpty() const { return hex.empty(); }
    };

    enum class PickKind
    {
        None,
        PlanarFace,
        CylindricalFace,
        LineEdge,
        CircleEdge      // a full circle or a circular arc
    };

    // A face or edge picked in the view.
    struct PickedGeometry
    {
        PickKind kind = PickKind::None;

        // Which item was picked, so a later step can find it again. Face and edge
        // keys bind in the part, and parts can share them, so the occurrence is kept too.
        PersistentKey occurrenceKey;
        PersistentKey targetKey;
        std::string occurrenceName;

        Vec3 clicked;       // where the user clicked

        // Planar face: a point on it, and its normal pointing out of the material.
        // Cylindrical face: a point on its axis, and the axis.
        // Circle edge: its centre, and its axis, out of the material where the add-on could tell.
        // Line edge: its start, and the direction from start to end.
        Vec3 point;
        Vec3 direction;

        // Line edges only
        Vec3 start;
        Vec3 end;
    };

    struct OriginOptions
    {
        bool flipZ = false;
        int xQuarterTurns = 0;      // X is turned about Z by this many 90 degree steps, 0 to 3
        Vec3 offset;                // in the origin's own axes, applied last
    };

    // What a geometry Set Origin step was made from. Stock presets come with stock.
    struct OriginDefinition
    {
        PickedGeometry plane;       // sets Z: a face or an edge
        PickedGeometry position;    // an edge. Empty uses the plane pick, if it's an edge.
        PickedGeometry xDirection;  // a line edge. Empty calculates X.
        OriginOptions options;
    };

    struct FrameResult
    {
        bool ok = false;
        Frame frame;
        std::string message;    // why it failed. Empty when ok.
    };

    // Works out the frame for a definition. The rules are in docs\SPEC.md.
    FrameResult BuildFrame(const OriginDefinition& definition);

    // The point an edge gives as a position: the centre of a circle, or the end
    // of a line nearer the click. Returns false for faces.
    bool PositionFromPick(const PickedGeometry& pick, Vec3& position);

    // A rectangle in the frame's XY plane that covers an axis-aligned box once it's
    // projected onto the plane. Each side grows by margin times the longer side, so
    // 0.1 adds 10%. An empty box gives a 20 mm square. The corners go anticlockwise
    // seen from +Z.
    struct PlaneRectangle
    {
        Vec3 corners[4];
    };

    PlaneRectangle PlaneCoveringBox(const Frame& frame, const Vec3& boxMin, const Vec3& boxMax, double margin);
}
