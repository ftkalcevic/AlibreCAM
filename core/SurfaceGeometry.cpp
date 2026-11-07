// SurfaceGeometry.cpp : exact closest points on simple surfaces.

#include "SurfaceGeometry.h"

#include <cmath>

namespace camcore
{
    namespace
    {
        const double kTiny = 1e-12;

        // p split into its distance along the axis from the centre and its offset across it.
        void Split(const AnalyticSurface& s, const Vec3& p, double& along, Vec3& across)
        {
            const Vec3 v = p - s.centre;
            along = Dot(v, s.axis);
            across = v - s.axis * along;
        }
    }

    bool ClosestPointOn(const AnalyticSurface& s, const Vec3& p, Vec3& closest)
    {
        switch (s.kind)
        {
        case SurfaceKind::Sphere:
        {
            const Vec3 v = p - s.centre;
            const double length = Length(v);
            if (length < kTiny)
                return false;
            closest = s.centre + v * (s.radius / length);
            return true;
        }
        case SurfaceKind::Cylinder:
        {
            double along = 0.0;
            Vec3 across;
            Split(s, p, along, across);
            const double out = Length(across);
            if (out < kTiny)
                return false;
            closest = s.centre + s.axis * along + across * (s.radius / out);
            return true;
        }
        case SurfaceKind::Cone:
        {
            // In the half plane through the axis and p, the side is the line
            // r = radius + slope t; the nearest point on it, or the apex past it.
            double along = 0.0;
            Vec3 across;
            Split(s, p, along, across);
            const double out = Length(across);
            if (out < kTiny)
                return false;
            const Vec3 direction = across * (1.0 / out);
            const double k = s.slope;
            double t = (along + k * (out - s.radius)) / (1.0 + k * k);
            double r = s.radius + k * t;
            if (r < 0.0)
            {
                // Past the apex: the apex itself.
                t = (std::fabs(k) > kTiny) ? -s.radius / k : 0.0;
                r = 0.0;
            }
            closest = s.centre + s.axis * t + direction * r;
            return true;
        }
        case SurfaceKind::Torus:
        {
            double along = 0.0;
            Vec3 across;
            Split(s, p, along, across);
            const double out = Length(across);
            if (out < kTiny)
                return false;
            const Vec3 middle = s.centre + across * (s.radius / out);
            const Vec3 v = p - middle;
            const double length = Length(v);
            if (length < kTiny)
                return false;
            closest = middle + v * (s.minorRadius / length);
            return true;
        }
        default:
            return false;
        }
    }

    bool SurfaceFitsPoints(const AnalyticSurface& surface, const std::vector<Vec3>& points, double tolerance)
    {
        if (surface.kind == SurfaceKind::None || points.empty())
            return false;
        for (const Vec3& p : points)
        {
            Vec3 closest;
            if (!ClosestPointOn(surface, p, closest))
            {
                // A point with no single answer (on the axis) is on the surface only
                // if the surface reaches the axis there; let the others decide.
                continue;
            }
            if (Length(p - closest) > tolerance)
                return false;
        }
        return true;
    }
}
