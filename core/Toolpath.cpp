// Toolpath.cpp : the moves an operation makes.

#include "Toolpath.h"

#include "Boundary.h"

namespace camcore
{
    std::vector<Vec3> MovePoints(const Vec3& from, const ToolpathMove& move)
    {
        if (move.kind == MoveKind::Feed && move.arc)
            return ArcPoints(from, move.to, move.centre, !move.clockwise);
        return { move.to };
    }
}
