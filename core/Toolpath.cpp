// Toolpath.cpp : the moves an operation makes.

#include "Toolpath.h"

#include <algorithm>
#include <cmath>

namespace camcore
{
    namespace
    {
        const double kPi = 3.14159265358979323846;
    }

    void ToPlane(ArcPlane plane, const Vec3& point, double& u, double& v, double& w)
    {
        switch (plane)
        {
        case ArcPlane::ZX: u = point.z; v = point.x; w = point.y; break;
        case ArcPlane::YZ: u = point.y; v = point.z; w = point.x; break;
        default:           u = point.x; v = point.y; w = point.z; break;
        }
    }

    Vec3 FromPlane(ArcPlane plane, double u, double v, double w)
    {
        switch (plane)
        {
        case ArcPlane::ZX: return { v, w, u };
        case ArcPlane::YZ: return { w, u, v };
        default:           return { u, v, w };
        }
    }

    double ArcMoveSweep(const Vec3& from, const ToolpathMove& move)
    {
        double fu, fv, fw, tu, tv, tw, cu, cv, cw;
        ToPlane(move.plane, from, fu, fv, fw);
        ToPlane(move.plane, move.to, tu, tv, tw);
        ToPlane(move.plane, move.centre, cu, cv, cw);
        const double a0 = std::atan2(fv - cv, fu - cu);
        const double a1 = std::atan2(tv - cv, tu - cu);
        double sweep = a1 - a0;
        if (move.clockwise)
        {
            while (sweep >= -1e-12)
                sweep -= 2.0 * kPi;
        }
        else
        {
            while (sweep <= 1e-12)
                sweep += 2.0 * kPi;
        }
        return sweep;
    }

    std::vector<Vec3> ArcMovePoints(const Vec3& from, const ToolpathMove& move, double step)
    {
        double fu, fv, fw, tu, tv, tw, cu, cv, cw;
        ToPlane(move.plane, from, fu, fv, fw);
        ToPlane(move.plane, move.to, tu, tv, tw);
        ToPlane(move.plane, move.centre, cu, cv, cw);
        const double radius = std::hypot(fu - cu, fv - cv);
        const double a0 = std::atan2(fv - cv, fu - cu);
        const double sweep = ArcMoveSweep(from, move);
        const int count = (std::max)(1, static_cast<int>(std::ceil(std::fabs(sweep) / (std::max)(1e-6, step))));
        std::vector<Vec3> points;
        points.reserve(static_cast<size_t>(count));
        for (int i = 1; i < count; ++i)
        {
            const double t = static_cast<double>(i) / count;
            const double angle = a0 + sweep * t;
            points.push_back(FromPlane(move.plane, cu + radius * std::cos(angle), cv + radius * std::sin(angle),
                                       fw + (tw - fw) * t));
        }
        points.push_back(move.to);
        return points;
    }

    std::vector<Vec3> MovePoints(const Vec3& from, const ToolpathMove& move)
    {
        if (move.kind == MoveKind::Feed && move.arc)
            return ArcMovePoints(from, move, 5.0 * kPi / 180.0);
        return { move.to };
    }
}
