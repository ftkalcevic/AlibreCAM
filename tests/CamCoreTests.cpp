// CamCoreTests.cpp : tests for CamCore. These run without Alibre.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <sstream>
#include <string>

#include "CamJob.h"

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

TEST_CASE("the .cam file sits next to the assembly with the same name")
{
    CHECK(CamPathForAssembly("folder/Bracket.AD_ASM") == fs::path("folder/Bracket.cam"));
    CHECK(CamPathForAssembly("folder/My.Bracket.v2.AD_ASM") == fs::path("folder/My.Bracket.v2.cam"));
    CHECK(CamPathForAssembly("").empty());
}
