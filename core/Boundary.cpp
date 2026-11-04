// Boundary.cpp : turns picked outlines into one closed polygon.

#include "Boundary.h"

#include <algorithm>
#include <cmath>

namespace camcore
{
    namespace
    {
        double DistanceXY(const Vec3& a, const Vec3& b)
        {
            return std::hypot(a.x - b.x, a.y - b.y);
        }
    }

    double SignedArea(const Polyline& polygon)
    {
        double twice = 0.0;
        for (size_t i = 0; i < polygon.size(); ++i)
        {
            const Vec3& a = polygon[i];
            const Vec3& b = polygon[(i + 1) % polygon.size()];
            twice += a.x * b.y - b.x * a.y;
        }
        return twice / 2.0;
    }

    Polyline ClosePolygon(const std::vector<Polyline>& pieces, double level)
    {
        std::vector<Polyline> remaining;
        for (const Polyline& piece : pieces)
        {
            if (!piece.empty())
                remaining.push_back(piece);
        }

        Polyline chain;
        if (remaining.empty())
            return chain;

        chain = remaining.front();
        remaining.erase(remaining.begin());

        // Each time, the piece with an end nearest the chain's end comes next.
        while (!remaining.empty())
        {
            const Vec3& end = chain.back();
            size_t best = 0;
            bool reverse = false;
            double bestDistance = -1.0;
            for (size_t i = 0; i < remaining.size(); ++i)
            {
                const double toStart = DistanceXY(end, remaining[i].front());
                const double toEnd = DistanceXY(end, remaining[i].back());
                if (bestDistance < 0.0 || toStart < bestDistance)
                {
                    best = i;
                    reverse = false;
                    bestDistance = toStart;
                }
                if (toEnd < bestDistance)
                {
                    best = i;
                    reverse = true;
                    bestDistance = toEnd;
                }
            }

            Polyline next = remaining[best];
            remaining.erase(remaining.begin() + static_cast<ptrdiff_t>(best));
            if (reverse)
                std::reverse(next.begin(), next.end());

            // A gap becomes a straight line, just by carrying on to the next piece.
            chain.insert(chain.end(), next.begin(), next.end());
        }

        // Flatten onto the level, and drop points that repeat the one before.
        Polyline polygon;
        for (const Vec3& point : chain)
        {
            const Vec3 flat{ point.x, point.y, level };
            if (polygon.empty() || DistanceXY(polygon.back(), flat) > kBoundaryJoinTolerance)
                polygon.push_back(flat);
        }

        // Closing back to the start is implied, so a repeated start goes.
        while (polygon.size() > 1 && DistanceXY(polygon.back(), polygon.front()) <= kBoundaryJoinTolerance)
            polygon.pop_back();

        if (polygon.size() < 3)
            return Polyline();

        if (SignedArea(polygon) < 0.0)
            std::reverse(polygon.begin(), polygon.end());
        return polygon;
    }
}
