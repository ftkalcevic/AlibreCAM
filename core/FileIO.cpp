// FileIO.cpp : file helpers shared by the job and tool library files.

#include "FileIO.h"

#include <fstream>
#include <sstream>
#include <system_error>

namespace camcore
{
    bool WriteFileAtomically(const std::filesystem::path& path, const std::string& text, std::string& message)
    {
        namespace fs = std::filesystem;

        fs::path temporary = path;
        temporary += ".tmp";

        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                message = "Could not write " + temporary.u8string();
                return false;
            }

            file.write(text.data(), static_cast<std::streamsize>(text.size()));
            file.close();

            if (!file)
            {
                std::error_code ignored;
                fs::remove(temporary, ignored);
                message = "Could not write " + temporary.u8string();
                return false;
            }
        }

        std::error_code error;
        fs::rename(temporary, path, error);
        if (error)
        {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            message = "Could not replace " + path.u8string() + ": " + error.message();
            return false;
        }

        return true;
    }

    bool ReadWholeFile(const std::filesystem::path& path, std::string& text)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return false;

        std::ostringstream buffer;
        buffer << file.rdbuf();
        if (file.bad())
            return false;

        text = buffer.str();
        return true;
    }
}
