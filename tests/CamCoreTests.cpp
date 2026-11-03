// CamCoreTests.cpp : tests for CamCore. These run without Alibre.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>

#include "CamJob.h"
#include "FileIO.h"
#include "ToolLibrary.h"

namespace fs = std::filesystem;
using namespace camcore;

namespace
{
    // A folder that is removed at the end of the test.
    class TempFolder
    {
    public:
        TempFolder()
        {
            static std::atomic<int> counter{ 0 };
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            m_path = fs::temp_directory_path() /
                     ("camcore_test_" + std::to_string(stamp) + "_" + std::to_string(counter++));
            fs::create_directories(m_path);
        }

        ~TempFolder()
        {
            std::error_code ignored;
            fs::remove_all(m_path, ignored);
        }

        fs::path operator/(const char* name) const { return m_path / name; }

    private:
        fs::path m_path;
    };

    void WriteText(const fs::path& path, const std::string& text)
    {
        std::ofstream file(path, std::ios::binary);
        file << text;
    }

    std::string ReadText(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        std::ostringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

    Job SampleJob()
    {
        Job job;
        job.assembly.name = "Bracket";
        job.assembly.guid = "A4A39FD0-567C-4D6B-A085-77688483C8EA";
        return job;
    }

    size_t CountFiles(const TempFolder& folder)
    {
        size_t count = 0;
        for (const auto& entry : fs::directory_iterator(folder / ""))
        {
            (void)entry;
            ++count;
        }
        return count;
    }
}

TEST_CASE("a saved job loads back unchanged")
{
    TempFolder folder;
    const fs::path path = folder / "Bracket.cam";

    const SaveResult saved = SaveJob(SampleJob(), path);
    REQUIRE(saved.ok);
    CHECK(saved.message.empty());

    const LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    CHECK(loaded.message.empty());
    CHECK(loaded.fileVersion == kCamFormatVersion);
    CHECK(loaded.job.assembly.name == "Bracket");
    CHECK(loaded.job.assembly.guid == "A4A39FD0-567C-4D6B-A085-77688483C8EA");
}

TEST_CASE("non-ASCII names and paths survive a save and load")
{
    TempFolder folder;
    const fs::path path = folder / "" / fs::u8path("Tr\xC3\xA4ger \xE6\x94\xAF\xE6\x9E\xB6.cam");

    Job job = SampleJob();
    job.assembly.name = "Tr\xC3\xA4ger \xE6\x94\xAF\xE6\x9E\xB6";

    REQUIRE(SaveJob(job, path).ok);

    const LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    CHECK(loaded.job.assembly.name == job.assembly.name);
}

TEST_CASE("the file records its format and version")
{
    TempFolder folder;
    const fs::path path = folder / "Bracket.cam";
    REQUIRE(SaveJob(SampleJob(), path).ok);

    const std::string text = ReadText(path);
    CHECK(text.find("\"format\": \"AlibreCAM\"") != std::string::npos);
    CHECK(text.find("\"formatVersion\": 1") != std::string::npos);
    CHECK(text.find("\"sequence\": []") != std::string::npos);
}

TEST_CASE("saving replaces an existing file and leaves no temporary file")
{
    TempFolder folder;
    const fs::path path = folder / "Bracket.cam";
    REQUIRE(SaveJob(SampleJob(), path).ok);

    Job changed = SampleJob();
    changed.assembly.name = "Renamed";
    REQUIRE(SaveJob(changed, path).ok);

    CHECK(LoadJob(path).job.assembly.name == "Renamed");
    CHECK(CountFiles(folder) == 1);
}

TEST_CASE("a failed save reports an error and leaves nothing behind")
{
    TempFolder folder;

    SUBCASE("the folder doesn't exist")
    {
        const SaveResult saved = SaveJob(SampleJob(), folder / "missing/Bracket.cam");
        CHECK_FALSE(saved.ok);
        CHECK_FALSE(saved.message.empty());
        CHECK(CountFiles(folder) == 0);
    }

    SUBCASE("the target can't be replaced")
    {
        const fs::path target = folder / "Bracket.cam";
        fs::create_directories(target);

        const SaveResult saved = SaveJob(SampleJob(), target);
        CHECK_FALSE(saved.ok);
        CHECK_FALSE(saved.message.empty());
        CHECK(fs::is_directory(target));
        CHECK(CountFiles(folder) == 1);
    }
}

TEST_CASE("files that can't be used are reported")
{
    TempFolder folder;
    const fs::path path = folder / "Bracket.cam";

    SUBCASE("missing")
    {
        CHECK(LoadJob(path).status == LoadStatus::NotFound);
    }

    SUBCASE("not JSON")
    {
        WriteText(path, "this is not json {");
        CHECK(LoadJob(path).status == LoadStatus::Unreadable);
    }

    SUBCASE("empty")
    {
        WriteText(path, "");
        CHECK(LoadJob(path).status == LoadStatus::Unreadable);
    }

    SUBCASE("JSON from something else")
    {
        WriteText(path, R"({"name":"package.json"})");
        CHECK(LoadJob(path).status == LoadStatus::NotACamFile);
    }

    SUBCASE("not an object")
    {
        WriteText(path, "[1,2,3]");
        CHECK(LoadJob(path).status == LoadStatus::NotACamFile);
    }

    SUBCASE("version zero")
    {
        WriteText(path, R"({"format":"AlibreCAM","formatVersion":0,"assembly":{"name":"a","guid":"b"}})");
        CHECK(LoadJob(path).status == LoadStatus::NotACamFile);
    }

    SUBCASE("version isn't a number")
    {
        WriteText(path, R"({"format":"AlibreCAM","formatVersion":"1","assembly":{"name":"a","guid":"b"}})");
        CHECK(LoadJob(path).status == LoadStatus::NotACamFile);
    }

    SUBCASE("no assembly section")
    {
        WriteText(path, R"({"format":"AlibreCAM","formatVersion":1})");
        CHECK(LoadJob(path).status == LoadStatus::NotACamFile);
    }

    SUBCASE("assembly GUID is the wrong type")
    {
        WriteText(path, R"({"format":"AlibreCAM","formatVersion":1,"assembly":{"name":"a","guid":5}})");
        CHECK(LoadJob(path).status == LoadStatus::NotACamFile);
    }
}

TEST_CASE("a file from a newer build is identified and left alone")
{
    TempFolder folder;
    const fs::path path = folder / "Bracket.cam";
    const std::string text = R"({"format":"AlibreCAM","formatVersion":999,"future":true})";
    WriteText(path, text);

    const LoadResult loaded = LoadJob(path);
    CHECK(loaded.status == LoadStatus::NewerVersion);
    CHECK(loaded.fileVersion == 999);
    CHECK_FALSE(loaded.message.empty());
    CHECK(ReadText(path) == text);
}

TEST_CASE("assembly identities are compared by GUID")
{
    AssemblyIdentity a{ "Bracket", "A4A39FD0-567C-4D6B-A085-77688483C8EA" };
    AssemblyIdentity lower{ "Renamed", "a4a39fd0-567c-4d6b-a085-77688483c8ea" };
    AssemblyIdentity other{ "Bracket", "7E79980B-0000-0000-0000-000000000000" };
    AssemblyIdentity none{ "Bracket", "" };

    CHECK(CompareIdentity(a, a) == IdentityMatch::Same);
    CHECK(CompareIdentity(a, lower) == IdentityMatch::Same);
    CHECK(CompareIdentity(a, other) == IdentityMatch::Different);
    CHECK(CompareIdentity(a, none) == IdentityMatch::Unknown);
    CHECK(CompareIdentity(none, a) == IdentityMatch::Unknown);
}

namespace
{
    Tool SampleMill()
    {
        Tool tool;
        tool.id = NewToolId();
        tool.number = 1;
        tool.name = "6 mm flat end mill";
        tool.type = ToolType::FlatEndMill;
        tool.unit = ToolUnit::Millimetres;
        tool.diameter = 6.0;
        tool.fluteLength = 22.0;
        tool.overallLength = 60.0;
        tool.fluteCount = 2;
        return tool;
    }

    Tool SampleDrill()
    {
        Tool tool;
        tool.id = NewToolId();
        tool.number = 2;
        tool.name = "1/8 in drill";
        tool.type = ToolType::Drill;
        tool.unit = ToolUnit::Inches;
        tool.diameter = 0.125;
        tool.fluteLength = 1.5;
        tool.overallLength = 2.5;
        tool.pointAngle = 135.0;
        return tool;
    }

    bool HasProblem(const std::vector<std::string>& problems, const std::string& text)
    {
        for (const std::string& problem : problems)
        {
            if (problem.find(text) != std::string::npos)
                return true;
        }
        return false;
    }
}

TEST_CASE("a saved tool library loads back unchanged")
{
    TempFolder folder;
    const fs::path path = folder / "tools.json";

    ToolLibrary library;
    library.tools.push_back(SampleMill());
    library.tools.push_back(SampleDrill());

    const SaveResult saved = SaveToolLibrary(library, path);
    REQUIRE(saved.ok);

    const ToolLibraryLoadResult loaded = LoadToolLibrary(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    REQUIRE(loaded.library.tools.size() == 2);

    const Tool& mill = loaded.library.tools[0];
    CHECK(mill.id == library.tools[0].id);
    CHECK(mill.number == 1);
    CHECK(mill.name == "6 mm flat end mill");
    CHECK(mill.type == ToolType::FlatEndMill);
    CHECK(mill.unit == ToolUnit::Millimetres);
    CHECK(mill.diameter == 6.0);
    CHECK(mill.fluteLength == 22.0);
    CHECK(mill.overallLength == 60.0);
    CHECK(mill.fluteCount == 2);

    const Tool& drill = loaded.library.tools[1];
    CHECK(drill.type == ToolType::Drill);
    CHECK(drill.unit == ToolUnit::Inches);
    CHECK(drill.diameter == 0.125);
    CHECK(drill.pointAngle == 135.0);
}

TEST_CASE("cutting settings in a library are ignored and not written back")
{
    TempFolder folder;
    const fs::path path = folder / "tools.json";

    {
        std::ofstream file(path, std::ios::binary);
        file << R"({
            "format": "AlibreCAM tool library",
            "formatVersion": 1,
            "tools": [{
                "id": "abc", "number": 1, "name": "Mill", "type": "flatEndMill", "unit": "mm",
                "diameter": 6.0, "fluteLength": 22.0, "overallLength": 60.0, "fluteCount": 2,
                "spindleSpeed": 18000.0, "plungeFeed": 300.0, "feed": 1200.0, "stepdown": 3.0, "stepover": 2.4
            }]
        })";
    }

    const ToolLibraryLoadResult loaded = LoadToolLibrary(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    REQUIRE(loaded.library.tools.size() == 1);

    REQUIRE(SaveToolLibrary(loaded.library, path).ok);

    std::string text;
    REQUIRE(ReadWholeFile(path, text));
    CHECK(text.find("feed") == std::string::npos);
    CHECK(text.find("spindleSpeed") == std::string::npos);
    CHECK(text.find("stepover") == std::string::npos);
}

TEST_CASE("an empty tool library is valid")
{
    TempFolder folder;
    const fs::path path = folder / "tools.json";
    REQUIRE(SaveToolLibrary(ToolLibrary(), path).ok);

    const ToolLibraryLoadResult loaded = LoadToolLibrary(path);
    CHECK(loaded.status == LoadStatus::Ok);
    CHECK(loaded.library.tools.empty());
}

TEST_CASE("tool validation")
{
    CHECK(ValidateTool(SampleMill()).empty());
    CHECK(ValidateTool(SampleDrill()).empty());

    Tool mill = SampleMill();

    SUBCASE("number") { mill.number = 0; CHECK(HasProblem(ValidateTool(mill), "number")); }
    SUBCASE("name") { mill.name.clear(); CHECK(HasProblem(ValidateTool(mill), "name")); }
    SUBCASE("diameter") { mill.diameter = 0.0; CHECK(HasProblem(ValidateTool(mill), "diameter")); }
    SUBCASE("flute length") { mill.fluteLength = -1.0; CHECK(HasProblem(ValidateTool(mill), "flute length")); }
    SUBCASE("overall length") { mill.overallLength = 10.0; CHECK(HasProblem(ValidateTool(mill), "overall length")); }
    SUBCASE("flute count") { mill.fluteCount = 0; CHECK(HasProblem(ValidateTool(mill), "flute count")); }

    SUBCASE("drills ignore mill fields, but check their own")
    {
        Tool drill = SampleDrill();
        drill.fluteCount = 0;
        CHECK(ValidateTool(drill).empty());

        drill.pointAngle = 200.0;
        CHECK(HasProblem(ValidateTool(drill), "point angle"));
    }
}

TEST_CASE("duplicate tool numbers aren't allowed")
{
    ToolLibrary library;
    library.tools.push_back(SampleMill());
    Tool second = SampleDrill();
    second.number = 1;
    library.tools.push_back(second);

    CHECK(HasProblem(ValidateLibrary(library), "same number"));

    TempFolder folder;
    const SaveResult saved = SaveToolLibrary(library, folder / "tools.json");
    CHECK_FALSE(saved.ok);
    CHECK(CountFiles(folder) == 0);
}

TEST_CASE("inch tools convert to millimetres exactly")
{
    const Tool drill = ToolInMillimetres(SampleDrill());
    CHECK(drill.unit == ToolUnit::Millimetres);
    CHECK(drill.diameter == doctest::Approx(3.175));
    CHECK(drill.fluteLength == doctest::Approx(38.1));
    CHECK(drill.overallLength == doctest::Approx(63.5));
    CHECK(drill.pointAngle == 135.0);

    const Tool mill = ToolInMillimetres(SampleMill());
    CHECK(mill.diameter == 6.0);
}

TEST_CASE("tool libraries that can't be used are reported and left alone")
{
    TempFolder folder;
    const fs::path path = folder / "tools.json";

    SUBCASE("missing")
    {
        CHECK(LoadToolLibrary(path).status == LoadStatus::NotFound);
    }

    SUBCASE("not JSON")
    {
        WriteText(path, "{ nope");
        CHECK(LoadToolLibrary(path).status == LoadStatus::Unreadable);
    }

    SUBCASE("a .cam file instead")
    {
        REQUIRE(SaveJob(SampleJob(), path).ok);
        CHECK(LoadToolLibrary(path).status == LoadStatus::NotACamFile);
    }

    SUBCASE("newer version")
    {
        const std::string text = R"({"format":"AlibreCAM tool library","formatVersion":99,"tools":[]})";
        WriteText(path, text);
        CHECK(LoadToolLibrary(path).status == LoadStatus::NewerVersion);
        CHECK(ReadText(path) == text);
    }

    SUBCASE("no tools section")
    {
        WriteText(path, R"({"format":"AlibreCAM tool library","formatVersion":1})");
        CHECK(LoadToolLibrary(path).status == LoadStatus::NotACamFile);
    }

    SUBCASE("a tool with a missing field")
    {
        WriteText(path, R"({"format":"AlibreCAM tool library","formatVersion":1,"tools":[{"id":"a","number":1}]})");
        const ToolLibraryLoadResult loaded = LoadToolLibrary(path);
        CHECK(loaded.status == LoadStatus::NotACamFile);
        CHECK_FALSE(loaded.message.empty());
    }

    SUBCASE("a tool with an invalid value")
    {
        ToolLibrary library;
        library.tools.push_back(SampleMill());
        REQUIRE(SaveToolLibrary(library, path).ok);

        std::string text = ReadText(path);
        const std::string field = "\"diameter\": 6.0";
        REQUIRE(text.find(field) != std::string::npos);
        text.replace(text.find(field), field.size(), "\"diameter\": -6.0");
        WriteText(path, text);

        const ToolLibraryLoadResult loaded = LoadToolLibrary(path);
        CHECK(loaded.status == LoadStatus::NotACamFile);
        CHECK(loaded.message.find("diameter") != std::string::npos);
    }
}

TEST_CASE("finding tools and choosing numbers")
{
    ToolLibrary library;
    CHECK(NextFreeToolNumber(library) == 1);

    Tool mill = SampleMill();
    mill.number = 1;
    Tool drill = SampleDrill();
    drill.number = 3;
    library.tools.push_back(mill);
    library.tools.push_back(drill);

    CHECK(NextFreeToolNumber(library) == 2);
    REQUIRE(FindToolById(library, drill.id) != nullptr);
    CHECK(FindToolById(library, drill.id)->number == 3);
    CHECK(FindToolById(library, "missing") == nullptr);
    CHECK(NewToolId().size() == 32);
    CHECK(NewToolId() != NewToolId());
}

TEST_CASE("a job remembers its tool library and selected tool")
{
    TempFolder folder;
    const fs::path path = folder / "Bracket.cam";

    Job job = SampleJob();
    job.toolLibraryPath = "shop_tools.json";
    job.selectedToolId = "abc123";
    REQUIRE(SaveJob(job, path).ok);

    const LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    CHECK(loaded.job.toolLibraryPath == "shop_tools.json");
    CHECK(loaded.job.selectedToolId == "abc123");
}

TEST_CASE("jobs saved before tools existed still load")
{
    TempFolder folder;
    const fs::path path = folder / "Bracket.cam";
    WriteText(path, R"({"format":"AlibreCAM","formatVersion":1,"assembly":{"name":"a","guid":"b"},"setup":{},"sequence":[]})");

    const LoadResult loaded = LoadJob(path);
    REQUIRE(loaded.status == LoadStatus::Ok);
    CHECK(loaded.job.toolLibraryPath.empty());
    CHECK(loaded.job.selectedToolId.empty());
}

TEST_CASE("a library beside the job is stored by name, so they can move together")
{
    const fs::path cam = "folder/Bracket.cam";

    CHECK(StoredLibraryPath(cam, "folder/tools.json") == "tools.json");
    CHECK(StoredLibraryPath(cam, "other/tools.json") == fs::path("other/tools.json").u8string());
    CHECK(StoredLibraryPath(fs::path(), "other/tools.json") == fs::path("other/tools.json").u8string());
    CHECK(StoredLibraryPath(cam, fs::path()).empty());

    CHECK(ResolveLibraryPath(cam, "tools.json") == fs::path("folder/tools.json"));
    CHECK(ResolveLibraryPath("moved/Bracket.cam", "tools.json") == fs::path("moved/tools.json"));
    CHECK(ResolveLibraryPath(cam, "other/tools.json") == fs::path("other/tools.json"));
    CHECK(ResolveLibraryPath(cam, "").empty());
}

TEST_CASE("the .cam file sits next to the assembly with the same name")
{
    CHECK(CamPathForAssembly("folder/Bracket.AD_ASM") == fs::path("folder/Bracket.cam"));
    CHECK(CamPathForAssembly("folder/My.Bracket.v2.AD_ASM") == fs::path("folder/My.Bracket.v2.cam"));
    CHECK(CamPathForAssembly("").empty());
}
