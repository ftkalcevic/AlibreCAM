// Stock.cpp : the job's stock, the material the parts are cut from.

#include "Stock.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace camcore
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;

        Vec3 RotateAbout(const Vec3& v, int axis, double degrees)
        {
            const double a = degrees * kPi / 180.0;
            const double c = std::cos(a);
            const double s = std::sin(a);
            switch (axis)
            {
            case 0: return { v.x, c * v.y - s * v.z, s * v.y + c * v.z };
            case 1: return { c * v.x + s * v.z, v.y, -s * v.x + c * v.z };
            default: return { c * v.x - s * v.y, s * v.x + c * v.y, v.z };
            }
        }

        Vec3 Rotate(const Vec3& v, const Vec3& rotation)
        {
            return RotateAbout(RotateAbout(RotateAbout(v, 0, rotation.x), 1, rotation.y), 2, rotation.z);
        }
    }

    std::string StockKindTitle(StockKind kind)
    {
        switch (kind)
        {
        case StockKind::Box: return "Box Stock";
        case StockKind::PartBox: return "Part Box Stock";
        case StockKind::Imported: return "Imported Stock";
        default: return std::string();
        }
    }

    std::vector<std::string> ValidateStock(const Stock& stock)
    {
        std::vector<std::string> problems;
        switch (stock.kind)
        {
        case StockKind::Box:
            if (!(stock.boxSize.x > 0.0) || !(stock.boxSize.y > 0.0) || !(stock.boxSize.z > 0.0))
                problems.push_back("Each size must be more than 0.");
            break;

        case StockKind::PartBox:
            for (double offset : { stock.offsetPlusX, stock.offsetMinusX, stock.offsetPlusY, stock.offsetMinusY,
                                   stock.offsetPlusZ, stock.offsetMinusZ })
            {
                if (!(offset >= 0.0))
                {
                    problems.push_back("The offsets can't be negative.");
                    break;
                }
            }
            break;

        case StockKind::Imported:
            if (stock.partPath.empty())
                problems.push_back("Choose the part to use as stock.");
            break;

        default:
            break;
        }
        return problems;
    }

    StockBox BoxStockBox(const Stock& stock)
    {
        return { stock.boxCorner, stock.boxCorner + stock.boxSize };
    }

    StockBox PartBoxStockBox(const Stock& stock, const Vec3& partsLower, const Vec3& partsUpper)
    {
        StockBox box;
        box.lower = { partsLower.x - stock.offsetMinusX, partsLower.y - stock.offsetMinusY,
                      partsLower.z - stock.offsetMinusZ };
        box.upper = { partsUpper.x + stock.offsetPlusX, partsUpper.y + stock.offsetPlusY,
                      partsUpper.z + stock.offsetPlusZ };
        return box;
    }

    Frame ImportedStockFrame(const Stock& stock)
    {
        Frame frame;
        frame.origin = stock.position;
        frame.xAxis = Rotate({ 1, 0, 0 }, stock.rotation);
        frame.yAxis = Rotate({ 0, 1, 0 }, stock.rotation);
        frame.zAxis = Rotate({ 0, 0, 1 }, stock.rotation);
        return frame;
    }

    std::vector<LineSegment> BoxEdges(const StockBox& box)
    {
        const Vec3& a = box.lower;
        const Vec3& b = box.upper;
        const Vec3 corners[8] = {
            { a.x, a.y, a.z }, { b.x, a.y, a.z }, { b.x, b.y, a.z }, { a.x, b.y, a.z },
            { a.x, a.y, b.z }, { b.x, a.y, b.z }, { b.x, b.y, b.z }, { a.x, b.y, b.z },
        };
        std::vector<LineSegment> edges;
        for (int i = 0; i < 4; ++i)
        {
            edges.push_back({ corners[i], corners[(i + 1) % 4] });              // bottom
            edges.push_back({ corners[4 + i], corners[4 + (i + 1) % 4] });      // top
            edges.push_back({ corners[i], corners[4 + i] });                    // upright
        }
        return edges;
    }

    std::vector<LineSegment> MeshOutlineEdges(const std::vector<Vec3>& triangles, double angleDegrees,
                                              double tolerance)
    {
        // Number the points, merging those within the tolerance (by rounding to a grid).
        using Cell = std::tuple<long long, long long, long long>;
        std::map<Cell, size_t> numbers;
        std::vector<Vec3> points;
        auto number = [&](const Vec3& p)
        {
            const double step = (tolerance > 0.0) ? tolerance : 1e-9;
            const Cell cell{ std::llround(p.x / step), std::llround(p.y / step), std::llround(p.z / step) };
            const auto found = numbers.find(cell);
            if (found != numbers.end())
                return found->second;
            numbers[cell] = points.size();
            points.push_back(p);
            return points.size() - 1;
        };

        // Each edge, with the normals of the triangles beside it.
        std::map<std::pair<size_t, size_t>, std::vector<Vec3>> edges;
        std::vector<std::pair<size_t, size_t>> order;
        for (size_t i = 0; i + 2 < triangles.size(); i += 3)
        {
            const size_t ids[3] = { number(triangles[i]), number(triangles[i + 1]), number(triangles[i + 2]) };
            if (ids[0] == ids[1] || ids[1] == ids[2] || ids[0] == ids[2])
                continue;       // no area
            Vec3 normal;
            if (!Normalise(Cross(triangles[i + 1] - triangles[i], triangles[i + 2] - triangles[i]), normal))
                continue;
            for (int k = 0; k < 3; ++k)
            {
                const size_t a = ids[k];
                const size_t b = ids[(k + 1) % 3];
                const std::pair<size_t, size_t> key(std::min(a, b), std::max(a, b));
                auto& normals = edges[key];
                if (normals.empty())
                    order.push_back(key);
                normals.push_back(normal);
            }
        }

        const double limit = std::cos(angleDegrees * kPi / 180.0);
        std::vector<LineSegment> result;
        for (const auto& key : order)
        {
            const std::vector<Vec3>& normals = edges[key];
            // An edge with one triangle is on the outside; with two, it's drawn where
            // they bend sharply. Anything else is odd, so it's drawn.
            const bool draw = normals.size() != 2 || Dot(normals[0], normals[1]) < limit;
            if (draw)
                result.push_back({ points[key.first], points[key.second] });
        }
        return result;
    }

    std::vector<Vec3> BoxCorners(const StockBox& box)
    {
        std::vector<Vec3> corners;
        for (int i = 0; i < 8; ++i)
        {
            corners.push_back({ (i & 1) ? box.upper.x : box.lower.x, (i & 2) ? box.upper.y : box.lower.y,
                                (i & 4) ? box.upper.z : box.lower.z });
        }
        return corners;
    }

    bool HighestAlongZ(const Frame& frame, const std::vector<Vec3>& points, double& top)
    {
        if (points.empty())
            return false;
        bool first = true;
        for (const Vec3& point : points)
        {
            const double height = Dot(point - frame.origin, frame.zAxis);
            if (first || height > top)
                top = height;
            first = false;
        }
        return true;
    }

    bool PointsBox(const std::vector<Vec3>& points, StockBox& box)
    {
        if (points.empty())
            return false;
        box.lower = box.upper = points.front();
        for (const Vec3& p : points)
        {
            box.lower = { std::min(box.lower.x, p.x), std::min(box.lower.y, p.y), std::min(box.lower.z, p.z) };
            box.upper = { std::max(box.upper.x, p.x), std::max(box.upper.y, p.y), std::max(box.upper.z, p.z) };
        }
        return true;
    }
}
