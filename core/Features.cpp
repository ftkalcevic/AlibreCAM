// Features.cpp : the geometry an operation machines, and the order it goes in.

#include "Features.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace camcore
{
    namespace
    {
        double DistanceXY(const Vec3& a, const Vec3& b)
        {
            return std::hypot(a.x - b.x, a.y - b.y);
        }

        std::vector<size_t> NearestNeighbour(const std::vector<Vec3>& points)
        {
            std::vector<size_t> order;
            if (points.empty())
                return order;

            std::vector<bool> used(points.size(), false);
            size_t current = 0;
            used[0] = true;
            order.push_back(0);

            for (size_t step = 1; step < points.size(); ++step)
            {
                size_t best = 0;
                double bestDistance = -1.0;
                for (size_t i = 0; i < points.size(); ++i)
                {
                    if (used[i])
                        continue;
                    const double distance = DistanceXY(points[current], points[i]);
                    if (bestDistance < 0.0 || distance < bestDistance)
                    {
                        best = i;
                        bestDistance = distance;
                    }
                }
                used[best] = true;
                order.push_back(best);
                current = best;
            }
            return order;
        }

        // Zigzag rows. `across` picks the coordinate rows are grouped by, and
        // `along` the one each row runs in.
        std::vector<size_t> Rows(const std::vector<Vec3>& points, double Vec3::*across, double Vec3::*along,
                                 bool acrossAscending, bool firstAlongAscending)
        {
            std::vector<size_t> byRow(points.size());
            std::iota(byRow.begin(), byRow.end(), size_t{ 0 });
            std::stable_sort(byRow.begin(), byRow.end(), [&](size_t a, size_t b)
            {
                return acrossAscending ? points[a].*across < points[b].*across
                                       : points[a].*across > points[b].*across;
            });

            std::vector<size_t> order;
            bool ascending = firstAlongAscending;
            size_t rowStart = 0;
            while (rowStart < byRow.size())
            {
                // A row is everything within the tolerance of its first feature.
                const double rowValue = points[byRow[rowStart]].*across;
                size_t rowEnd = rowStart + 1;
                while (rowEnd < byRow.size() &&
                       std::fabs(points[byRow[rowEnd]].*across - rowValue) <= kFeatureRowTolerance)
                    ++rowEnd;

                std::vector<size_t> row(byRow.begin() + static_cast<ptrdiff_t>(rowStart),
                                        byRow.begin() + static_cast<ptrdiff_t>(rowEnd));
                std::stable_sort(row.begin(), row.end(), [&](size_t a, size_t b)
                {
                    return ascending ? points[a].*along < points[b].*along : points[a].*along > points[b].*along;
                });
                order.insert(order.end(), row.begin(), row.end());

                ascending = !ascending;
                rowStart = rowEnd;
            }
            return order;
        }
    }

    bool FeatureAllowed(FeatureFilter filter, PickKind kind)
    {
        switch (filter)
        {
        case FeatureFilter::Points:
            return kind == PickKind::CircleEdge || kind == PickKind::CylindricalFace || kind == PickKind::DesignPoint;
        case FeatureFilter::FacingArea:
            return kind == PickKind::PlanarFace || kind == PickKind::LineEdge || kind == PickKind::CircleEdge ||
                   kind == PickKind::CurveEdge;
        default:
            return false;
        }
    }

    Vec3 FeaturePoint(const Frame& frame, const PickedGeometry& feature)
    {
        if (feature.kind == PickKind::CylindricalFace)
        {
            const double startHeight = Dot(feature.start - frame.origin, frame.zAxis);
            const double endHeight = Dot(feature.end - frame.origin, frame.zAxis);
            return (endHeight > startHeight) ? feature.end : feature.start;
        }
        return feature.point;
    }

    Vec3 WorldToFrame(const Frame& frame, const Vec3& world)
    {
        const Vec3 relative = world - frame.origin;
        return { Dot(relative, frame.xAxis), Dot(relative, frame.yAxis), Dot(relative, frame.zAxis) };
    }

    bool SameFeature(const PickedGeometry& a, const PickedGeometry& b)
    {
        if (a.kind != b.kind || a.occurrenceKey.hex != b.occurrenceKey.hex)
            return false;

        // Design points have no key, so their name stands in for one.
        if (a.kind == PickKind::DesignPoint)
            return a.targetName == b.targetName && a.occurrenceName == b.occurrenceName;
        return a.targetKey.hex == b.targetKey.hex;
    }

    std::vector<PickedGeometry> ApplySelectionChange(const std::vector<PickedGeometry>& list,
                                                     const std::vector<PickedGeometry>& before,
                                                     const std::vector<PickedGeometry>& after)
    {
        const auto contains = [](const std::vector<PickedGeometry>& items, const PickedGeometry& item)
        {
            return std::any_of(items.begin(), items.end(),
                               [&](const PickedGeometry& other) { return SameFeature(other, item); });
        };

        std::vector<PickedGeometry> changed;
        for (const PickedGeometry& feature : list)
        {
            const bool deselected = contains(before, feature) && !contains(after, feature);
            if (!deselected && !contains(changed, feature))
                changed.push_back(feature);
        }
        for (const PickedGeometry& feature : after)
        {
            if (!contains(changed, feature) && !contains(before, feature))
                changed.push_back(feature);
        }
        return changed;
    }

    std::vector<size_t> SortFeatures(const std::vector<Vec3>& points, FeatureOrder order, StartCorner corner)
    {
        const bool top = (corner == StartCorner::TopLeft || corner == StartCorner::TopRight);
        const bool left = (corner == StartCorner::TopLeft || corner == StartCorner::BottomLeft);

        switch (order)
        {
        case FeatureOrder::RowsXFirst:
            // Rows stepping away from the corner's edge in Y, the first running away from its side in X.
            return Rows(points, &Vec3::y, &Vec3::x, !top, left);
        case FeatureOrder::RowsYFirst:
            return Rows(points, &Vec3::x, &Vec3::y, left, !top);
        default:
            return NearestNeighbour(points);
        }
    }
}
