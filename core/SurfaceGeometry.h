// SurfaceGeometry.h : exact closest points on simple surfaces. No Alibre or MFC code belongs here.

#pragma once

#include <memory>
#include <vector>

#include "Nurbs.h"
#include "Origin.h"

namespace camcore
{
    enum class SurfaceKind
    {
        None,           // not known here: ask Alibre
        Sphere,         // centre, radius
        Cylinder,       // a point on the axis (centre), axis, radius
        Cone,           // a point on the axis (centre) where the radius is `radius`, axis; it widens by
                        // `slope` (the radius gained per unit along the axis; negative narrows)
        Torus,          // centre, axis, radius (to the tube's middle), minorRadius (the tube's)
        Nurbs           // nurbs, prepared (NurbsSurface::Prepare)
    };

    // A face's surface, unbounded, in the frame, in millimetres.
    struct AnalyticSurface
    {
        SurfaceKind kind = SurfaceKind::None;
        Vec3 centre;
        Vec3 axis{ 0.0, 0.0, 1.0 };     // a unit vector
        double radius = 0.0;
        double minorRadius = 0.0;
        double slope = 0.0;
        // How far a point may be from the face's own facets and still be on the
        // face: twice Alibre's 0.05 mm facets.
        double onFaceTolerance = 0.1;
        std::shared_ptr<const NurbsSurface> nurbs;
    };

    // The closest point on the (unbounded) surface to p. False where there's no
    // single answer, as for p on a sphere's centre or an axis.
    bool ClosestPointOn(const AnalyticSurface& surface, const Vec3& p, Vec3& closest);

    // Whether every point lies within tolerance of the surface: a check that a
    // surface read from Alibre is the one its facets show.
    bool SurfaceFitsPoints(const AnalyticSurface& surface, const std::vector<Vec3>& points, double tolerance);
}
