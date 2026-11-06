// Materials.h : the shared list of materials a job can be cut from. No Alibre or MFC code belongs here.
//
// The list is one file for every machine, %APPDATA%\AlibreCAM\materials.json.
// Tools keep feeds per material by the material's ID, so a material can be renamed.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "CamJob.h"     // LoadStatus, SaveResult

namespace camcore
{
    constexpr int kMaterialsFormatVersion = 1;

    struct Material
    {
        std::string id;     // stable
        std::string name;
    };

    struct MaterialList
    {
        std::vector<Material> materials;
    };

    // Returns a sentence for each problem. Empty means the list is valid.
    // Every material needs an ID and a name, and names and IDs are unique
    // (names ignoring case).
    std::vector<std::string> ValidateMaterials(const MaterialList& list);

    const Material* FindMaterial(const MaterialList& list, const std::string& id);

    struct MaterialsLoadResult
    {
        LoadStatus status = LoadStatus::Unreadable;
        MaterialList list;
        std::string message;    // empty when status is Ok
    };

    MaterialsLoadResult LoadMaterials(const std::filesystem::path& path);
    SaveResult SaveMaterials(const MaterialList& list, const std::filesystem::path& path);
}
