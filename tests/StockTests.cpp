// StockTests.cpp : tests for the job's stock.

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>

#include "Boundary.h"
#include "CamJob.h"
#include "Stock.h"

using namespace camcore;

namespace
{
    bool Near(const Vec3& a, const Vec3& b, double tolerance = 1e-9)
    {
        return std::fabs(a.x - b.x) <= tolerance && std::fabs(a.y - b.y) <= tolerance &&
               std::fabs(a.z - b.z) <= tolerance;
    }

    // A box's 12 triangles, two to a face.
    std::vector<Vec3> BoxMesh(const Vec3& a, const Vec3& b)
    {
        const Vec3 c[8] = {
            { a.x, a.y, a.z }, { b.x, a.y, a.z }, { b.x, b.y, a.z }, { a.x, b.y, a.z },
            { a.x, a.y, b.z }, { b.x, a.y, b.z }, { b.x, b.y, b.z }, { a.x, b.y, b.z },
        };
        const int faces[6][4] = { { 0, 3, 2, 1 }, { 4, 5, 6, 7 }, { 0, 1, 5, 4 },
                                  { 1, 2, 6, 5 }, { 2, 3, 7, 6 }, { 3, 0, 4, 7 } };
        std::vector<Vec3> mesh;
        for (const auto& f : faces)
        {
            for (int i : { f[0], f[1], f[2], f[0], f[2], f[3] })
                mesh.push_back(c[i]);
        }
        return mesh;
    }
}

TEST_CASE("stock kinds have titles for the tree")
{
    CHECK(StockKindTitle(StockKind::Box) == "Box Stock");
    CHECK(StockKindTitle(StockKind::PartBox) == "Part Box Stock");
    CHECK(StockKindTitle(StockKind::Imported) == "Imported Stock");
    CHECK(StockKindTitle(StockKind::None).empty());
}

TEST_CASE("a box stock runs from its corner by its size")
{
    Stock stock;
    stock.kind = StockKind::Box;
    stock.boxCorner = { -10, -20, -5 };
    stock.boxSize = { 100, 50, 25 };
    const StockBox box = BoxStockBox(stock);
    CHECK(Near(box.lower, { -10, -20, -5 }));
    CHECK(Near(box.upper, { 90, 30, 20 }));
    CHECK(ValidateStock(stock).empty());

    stock.boxSize.z = 0;
    CHECK_FALSE(ValidateStock(stock).empty());
}

TEST_CASE("a part box stock adds each offset outside its side")
{
    Stock stock;
    stock.kind = StockKind::PartBox;
    stock.offsetPlusX = 1;
    stock.offsetMinusX = 2;
    stock.offsetPlusY = 3;
    stock.offsetMinusY = 4;
    stock.offsetPlusZ = 5;
    stock.offsetMinusZ = 0;
    const StockBox box = PartBoxStockBox(stock, { 0, 0, 0 }, { 10, 20, 30 });
    CHECK(Near(box.lower, { -2, -4, 0 }));
    CHECK(Near(box.upper, { 11, 23, 35 }));
    CHECK(ValidateStock(stock).empty());

    stock.offsetMinusZ = -1;
    CHECK_FALSE(ValidateStock(stock).empty());
}

TEST_CASE("an imported stock turns about X, then Y, then Z, then moves")
{
    Stock stock;
    stock.kind = StockKind::Imported;
    CHECK_FALSE(ValidateStock(stock).empty());      // no part chosen
    stock.partPath = "C:/Stock/Block.AD_PRT";
    CHECK(ValidateStock(stock).empty());

    stock.position = { 5, 6, 7 };
    stock.rotation = { 0, 0, 90 };
    Frame frame = ImportedStockFrame(stock);
    CHECK(Near(frame.xAxis, { 0, 1, 0 }));
    CHECK(Near(FramePointToWorld(frame, { 1, 0, 0 }), { 5, 7, 7 }));

    // X first: a quarter about X takes Y to Z, then a quarter about Z leaves Z alone.
    stock.position = {};
    stock.rotation = { 90, 0, 90 };
    frame = ImportedStockFrame(stock);
    CHECK(Near(frame.yAxis, { 0, 0, 1 }));
    CHECK(Near(frame.xAxis, { 0, 1, 0 }));
}

TEST_CASE("a box has 12 edges")
{
    const std::vector<LineSegment> edges = BoxEdges({ { 0, 0, 0 }, { 1, 2, 3 } });
    CHECK(edges.size() == 12);
}

TEST_CASE("a box mesh outlines its 12 edges, not its face diagonals")
{
    const std::vector<LineSegment> edges = MeshOutlineEdges(BoxMesh({ 0, 0, 0 }, { 10, 20, 30 }), 20.0);
    CHECK(edges.size() == 12);
    for (const LineSegment& edge : edges)
    {
        // Every edge runs along one axis.
        const Vec3 d = edge.to - edge.from;
        const int along = (std::fabs(d.x) > 1e-9) + (std::fabs(d.y) > 1e-9) + (std::fabs(d.z) > 1e-9);
        CHECK(along == 1);
    }
}

TEST_CASE("an open mesh outlines its loose edges")
{
    // One square of two triangles: its four sides, not the diagonal.
    const std::vector<Vec3> square = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 } };
    CHECK(MeshOutlineEdges(square, 20.0).size() == 4);
}

TEST_CASE("the stock's top along an origin's Z")
{
    const std::vector<Vec3> corners = BoxCorners({ { 0, 0, 0 }, { 10, 20, 30 } });
    CHECK(corners.size() == 8);

    double top = 0;
    Frame frame;
    frame.origin = { 0, 0, 5 };
    REQUIRE(HighestAlongZ(frame, corners, top));
    CHECK(top == doctest::Approx(25));

    // A frame looking along -X: the highest is the side at X 0.
    frame.origin = { 0, 0, 0 };
    frame.zAxis = { -1, 0, 0 };
    frame.xAxis = { 0, 0, 1 };
    frame.yAxis = { 0, 1, 0 };
    REQUIRE(HighestAlongZ(frame, corners, top));
    CHECK(top == doctest::Approx(0));

    CHECK_FALSE(HighestAlongZ(frame, {}, top));
}

TEST_CASE("the box around some points")
{
    StockBox box;
    CHECK_FALSE(PointsBox({}, box));
    REQUIRE(PointsBox({ { 1, 5, -2 }, { -3, 2, 4 } }, box));
    CHECK(Near(box.lower, { -3, 2, -2 }));
    CHECK(Near(box.upper, { 1, 5, 4 }));
}

TEST_CASE("the stock is saved with the job")
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / "CAMForAlibre-stock-test.cam";

    Job job;
    job.assembly.name = "Bracket";
    job.assembly.guid = "{guid}";
    job.stock.kind = StockKind::PartBox;
    job.stock.offsetPlusZ = 2.5;
    StockPart part;
    part.name = "Plate<1>";
    part.occurrenceKey.elementType = 17;
    part.occurrenceKey.hex = "0a0b";
    job.stock.parts.push_back(part);
    REQUIRE(SaveJob(job, path).ok);

    LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    CHECK(loaded.job.stock.kind == StockKind::PartBox);
    CHECK(loaded.job.stock.offsetPlusZ == 2.5);
    REQUIRE(loaded.job.stock.parts.size() == 1);
    CHECK(loaded.job.stock.parts[0].name == "Plate<1>");
    CHECK(loaded.job.stock.parts[0].occurrenceKey.hex == "0a0b");

    job.stock = Stock();
    job.stock.kind = StockKind::Imported;
    job.stock.partPath = "C:/Stock/Block.AD_PRT";
    job.stock.position = { 1, 2, 3 };
    job.stock.rotation = { 0, 90, 0 };
    REQUIRE(SaveJob(job, path).ok);
    loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    CHECK(loaded.job.stock.kind == StockKind::Imported);
    CHECK(loaded.job.stock.partPath == "C:/Stock/Block.AD_PRT");
    CHECK(Near(loaded.job.stock.rotation, { 0, 90, 0 }));

    job.stock = Stock();
    REQUIRE(SaveJob(job, path).ok);
    loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    CHECK(loaded.job.stock.kind == StockKind::None);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST_CASE("the stock's footprint is its box seen from above, at its top")
{
    const StockBox box{ { 0, 0, 0 }, { 40, 30, 10 } };
    Frame frame;        // the assembly's own axes
    const Path flat = StockFootprint(frame, box);
    REQUIRE(flat.size() == 4);
    double area = 0.0;
    for (size_t i = 0; i < flat.size(); ++i)
    {
        const Vec3& a = flat[i].point;
        const Vec3& b = flat[(i + 1) % flat.size()].point;
        area += a.x * b.y - b.x * a.y;
        CHECK(a.z == doctest::Approx(10));
        CHECK((a.x == doctest::Approx(0) || a.x == doctest::Approx(40)));
        CHECK((a.y == doctest::Approx(0) || a.y == doctest::Approx(30)));
    }
    CHECK(area / 2.0 == doctest::Approx(1200));     // anticlockwise

    // Tilted 45 degrees about X, the box's side shows too: a wider outline.
    const double c = std::sqrt(0.5);
    frame.yAxis = { 0, c, c };
    frame.zAxis = { 0, -c, c };
    const Path tilted = StockFootprint(frame, box);
    CHECK(tilted.size() >= 4);
    double low = 0.0, high = 0.0;
    PathExtent(tilted, { 0, 1, 0 }, low, high);
    CHECK(high - low == doctest::Approx(30 * c + 10 * c));
    for (const PathVertex& vertex : tilted)
        CHECK(vertex.point.z == doctest::Approx(10 * c));     // the highest corner: y 0, z 10
}
