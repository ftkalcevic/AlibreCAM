// CamJob.h : the CAM job and its .cam file. No Alibre or MFC code belongs here.
//
// Text is UTF-8 throughout this library. All lengths will be in millimetres.

#pragma once

#include <filesystem>
#include <string>

namespace camcore
{
    // The newest .cam format this build can read and the one it writes.
    constexpr int kCamFormatVersion = 1;

    // Which assembly a job belongs to. The GUID comes from the assembly file,
    // and is empty if it couldn't be read.
    struct AssemblyIdentity
    {
        std::string name;
        std::string guid;
    };

    // Machine, stock and the operation sequence are added in later steps.
    struct Job
    {
        AssemblyIdentity assembly;

        // Empty uses the add-on's default tool library. Otherwise, as returned
        // by StoredLibraryPath.
        std::string toolLibraryPath;

        // The ID of the selected tool in the library, or empty.
        std::string selectedToolId;
    };

    enum class LoadStatus
    {
        Ok,
        NotFound,       // no file at this path
        Unreadable,     // the file couldn't be read, or isn't valid JSON
        NotACamFile,    // valid JSON, but not a usable .cam file
        NewerVersion    // written by a newer build, so it mustn't be overwritten
    };

    struct LoadResult
    {
        LoadStatus status = LoadStatus::Unreadable;
        Job job;
        int fileVersion = 0;
        std::string message;    // empty when status is Ok
    };

    struct SaveResult
    {
        bool ok = false;
        std::string message;    // empty when ok
    };

    enum class IdentityMatch
    {
        Same,
        Different,
        Unknown     // at least one GUID is missing, so they can't be compared
    };

    LoadResult LoadJob(const std::filesystem::path& path);

    // Writes a temporary file and then replaces the target, so a failed save
    // leaves any existing file as it was.
    SaveResult SaveJob(const Job& job, const std::filesystem::path& path);

    IdentityMatch CompareIdentity(const AssemblyIdentity& a, const AssemblyIdentity& b);

    // Bracket.AD_ASM becomes Bracket.cam, in the same folder.
    std::filesystem::path CamPathForAssembly(const std::filesystem::path& assemblyPath);
}
