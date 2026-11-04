// Boundary.h : turns picked outlines into one closed boundary, with arcs kept as arcs.
// No Alibre or MFC code belongs here.
//
// Points are in an operation's origin frame, in millimetres. Only X and Y shape
// the boundary; every point is put at the given level, and arcs turn about Z.

#pragma once

#include <vector>

#include "Origin.h"

namespace camcore
{
    using Polyline = std::vector<Vec3>;

    // Points closer than this are the same point.
    constexpr double kBoundaryJoinTolerance = 0.001;

    // Arcs are drawn as straight segments this many degrees apart.
    constexpr double kArcDrawingStepDegrees = 5.0;

    // A point on a path, and how the path goes on from it to the next point:
    // straight, or round an arc. An arc runs anticlockwise about its axis
    // (right-hand rule), round its centre.
    struct PathVertex
    {
        Vec3 point;
        bool arc = false;
        Vec3 centre;
        Vec3 axis{ 0.0, 0.0, 1.0 };
    };

    // In an open path the last vertex goes nowhere, so its arc is ignored. In a
    // closed path the last vertex goes on to the first.
    using Path = std::vector<PathVertex>;

    // A path of straight lines through the points.
    Path PathFromPoints(const Polyline& points);

    // True if an arc in the XY plane, seen from +Z, runs anticlockwise.
    inline bool ArcIsAnticlockwise(const PathVertex& vertex) { return vertex.axis.z > 0.0; }

    // The signed angle in radians an arc in XY sweeps from start to end round
    // centre: positive anticlockwise. Coincident ends make a full turn.
    double ArcSweep(const Vec3& start, const Vec3& end, const Vec3& centre, bool anticlockwise);

    // An arc in XY as points after its start, up to and including end, no more
    // than kArcDrawingStepDegrees apart. Z changes evenly from the start's to the end's.
    Polyline ArcPoints(const Vec3& start, const Vec3& end, const Vec3& centre, bool anticlockwise);

    // A path as points, arcs sampled for drawing. A closed path ends back on its start.
    Polyline PathPoints(const Path& path, bool closed);

    // The lowest and highest of Dot(point, direction) over a closed path,
    // counting the farthest reach of its arcs. direction is a unit vector in XY.
    void PathExtent(const Path& path, const Vec3& direction, double& low, double& high);

    // Where a closed path crosses the line Dot(p, across) = v, as values of
    // Dot(p, along). along and across are unit vectors in XY, square to each other.
    std::vector<double> PathCrossings(const Path& path, const Vec3& along, const Vec3& across, double v);

    // Joins pieces, such as a face's loop edges or loose edges, into one closed
    // path. It starts with the first piece, then repeatedly adds whichever
    // remaining piece has an end nearest to where the chain has got to, turned
    // round if that end is its last. Gaps are bridged with straight lines, and the
    // chain is closed back to its start. Repeated points are dropped, arcs not
    // about Z become straight segments, and the result runs anticlockwise seen
    // from +Z, without the first point repeated at the end. A path that encloses
    // nothing is empty.
    Path ClosePath(const std::vector<Path>& pieces, double level);

    // ClosePath for straight pieces, as points.
    Polyline ClosePolygon(const std::vector<Polyline>& pieces, double level);

    // The area in X and Y: positive anticlockwise, negative clockwise.
    double SignedArea(const Polyline& polygon);
    double SignedArea(const Path& path);
}
