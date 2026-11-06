// SourceTests.cpp : checks on the source files themselves.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
    // The first string literal on the line with a byte outside ASCII, or empty.
    // Comments after // are ignored. Good enough for this code's style.
    bool StringHasNonAscii(const std::string& line)
    {
        const size_t comment = line.find("//");
        const std::string code = line.substr(0, comment);
        bool inString = false;
        for (size_t i = 0; i < code.size(); ++i)
        {
            const char ch = code[i];
            if (ch == '\\' && inString)
            {
                ++i;
                continue;
            }
            if (ch == '"')
                inString = !inString;
            else if (inString && static_cast<unsigned char>(ch) >= 0x80)
                return true;
        }
        return false;
    }
}

// The compiler reads the source as Windows-1252, so a typed degree sign or dash
// in a string shows as two odd characters. They must be written as escapes.
TEST_CASE("string literals in the source are ASCII")
{
    const std::filesystem::path root = std::filesystem::path(__FILE__).parent_path().parent_path();
    int checked = 0;
    for (const char* folder : { "addon", "core" })
    {
        for (const auto& entry : std::filesystem::directory_iterator(root / folder))
        {
            const std::string extension = entry.path().extension().string();
            if (extension != ".cpp" && extension != ".h" && extension != ".rc")
                continue;
            std::ifstream in(entry.path(), std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            ++checked;
            size_t lineNumber = 0;
            size_t start = 0;
            while (start <= text.size())
            {
                const size_t end = text.find('\n', start);
                const std::string line = text.substr(start, (end == std::string::npos ? text.size() : end) - start);
                ++lineNumber;
                if (StringHasNonAscii(line))
                    FAIL_CHECK(entry.path().filename().string() << " line " << lineNumber
                                                                << " has a non-ASCII character in a string.");
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }
        }
    }
    CHECK(checked > 20);
}
