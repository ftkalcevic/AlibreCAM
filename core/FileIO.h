// FileIO.h : file helpers shared by the job and tool library files.

#pragma once

#include <filesystem>
#include <string>

namespace camcore
{
    // Writes a temporary file and then replaces the target, so a failed write
    // leaves any existing file as it was. On failure, message says why.
    bool WriteFileAtomically(const std::filesystem::path& path, const std::string& text, std::string& message);

    // Returns false if the file couldn't be opened or read.
    bool ReadWholeFile(const std::filesystem::path& path, std::string& text);
}
