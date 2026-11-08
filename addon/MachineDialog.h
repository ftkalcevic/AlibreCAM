// MachineDialog.h : the Machine dialog, which chooses the job's machine and edits
// machines, their tools, and the shared list of materials.
//
// Machines are files in %APPDATA%\CAMForAlibre\machines\, and the materials are
// %APPDATA%\CAMForAlibre\materials.json. Changes are saved to them straight away.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "resource.h"
#include "Machine.h"
#include "Materials.h"

// %APPDATA%\CAMForAlibre\machines\. Creates the folder if needed.
std::filesystem::path MachinesFolder();

// %APPDATA%\CAMForAlibre\materials.json.
std::filesystem::path MaterialsPath();

// The machine a job with none takes: the last one used if it's there, otherwise
// the first readable one, otherwise a new empty "Machine 1". Empty only if no
// machine can be made.
std::string DefaultMachineId();

// Remembers the machine in settings.json, unless that file is damaged or newer.
void RememberLastMachine(const std::string& id);

class CMachineDlg : public CDialog
{
public:
    enum Tab
    {
        MachineTab,
        ToolsTab,
        MaterialsTab
    };

    CMachineDlg(const std::string& machineId, const std::string& materialId, Tab startTab, CWnd* pParent);

    // The machine chosen, which the job then uses, and the job's material,
    // cleared if it was deleted.
    const std::string& GetMachineId() const { return m_machineId; }
    const std::string& GetMaterialId() const { return m_materialId; }

    enum { IDD = IDD_MACHINE };

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    void OnCancel() override;

    afx_msg void OnTabChanged(NMHDR* pNotify, LRESULT* pResult);
    afx_msg void OnMachineChosen();
    afx_msg void OnNewMachine();
    afx_msg void OnCopyMachine();
    afx_msg void OnDeleteMachine();
    afx_msg void OnMachineFieldChanged();
    afx_msg void OnAddTool();
    afx_msg void OnEditTool();
    afx_msg void OnCopyTool();
    afx_msg void OnDeleteTool();
    afx_msg void OnImportTools();
    afx_msg void OnExportTools();
    afx_msg void OnToolDoubleClick(NMHDR* pNotify, LRESULT* pResult);
    afx_msg void OnToolSelectionChanged(NMHDR* pNotify, LRESULT* pResult);
    afx_msg void OnAddMaterial();
    afx_msg void OnRenameMaterial();
    afx_msg void OnDeleteMaterial();
    afx_msg void OnMaterialSelectionChanged();
    afx_msg void OnHelpButton();

    DECLARE_MESSAGE_MAP()

private:
    void FillMachines();
    void ShowMachine();
    void ShowTab(int tab);
    void UpdateButtons();
    bool ApplyMachineFields();      // the name and axes, saved if they changed
    bool SaveCurrentMachine();
    void FillTools();
    int SelectedTool() const;
    void AddTool(const camcore::Tool& tool);
    void FillMaterials();
    bool SaveMaterialList();
    bool MaterialNameTaken(const std::wstring& name, const std::string& except) const;

    std::string m_machineId;
    std::string m_materialId;
    Tab m_startTab;

    std::vector<camcore::MachineEntry> m_entries;
    camcore::Machine m_machine;     // the chosen machine as saved
    bool m_canEdit;                 // false if its file can't be read, so it isn't overwritten
    bool m_filling;

    camcore::MaterialList m_materials;
    bool m_materialsWritable;

    CComboBox m_machineList;
    CTabCtrl m_tabs;
    CComboBox m_axes;
    CComboBox m_rotary;
    CListCtrl m_tools;
    CListBox m_materialList;
};
