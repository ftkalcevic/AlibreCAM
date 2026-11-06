// ToolDialogs.h : the tool library dialog and the dialog for editing one tool.

#pragma once

#include "resource.h"
#include "ToolLibrary.h"

// %APPDATA%\AlibreCAM\tools.json. Creates the folder if needed.
std::filesystem::path DefaultToolLibraryPath();

class CToolEditDlg : public CDialog
{
public:
    // otherTools is used to check that the tool number isn't already taken.
    CToolEditDlg(const camcore::Tool& tool, const camcore::ToolLibrary& otherTools, CWnd* pParent);

    const camcore::Tool& GetTool() const { return m_tool; }

    enum { IDD = IDD_TOOL_EDIT };

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    afx_msg void OnTypeChanged();
    afx_msg void OnHelpButton();

    DECLARE_MESSAGE_MAP()

private:
    bool IsDrillSelected() const;
    void UpdateFieldsForType();
    void SetNumber(int id, double value);
    bool ReadNumber(int id, const wchar_t* label, double& value, std::wstring& problems);
    bool ReadInteger(int id, const wchar_t* label, int& value, std::wstring& problems);

    camcore::Tool m_tool;
    camcore::ToolLibrary m_otherTools;
    CComboBox m_type;
    CComboBox m_unit;
};

// Tool changes are saved to the library file straight away. Choosing another
// library changes GetLibraryPath, which the job then stores.
class CToolLibraryDlg : public CDialog
{
public:
    CToolLibraryDlg(const std::filesystem::path& libraryPath, const std::string& selectedToolId, CWnd* pParent);

    const std::filesystem::path& GetLibraryPath() const { return m_path; }

    enum { IDD = IDD_TOOL_LIBRARY };

protected:
    BOOL OnInitDialog() override;
    afx_msg void OnAdd();
    afx_msg void OnEdit();
    afx_msg void OnCopy();
    afx_msg void OnDelete();
    afx_msg void OnOtherLibrary();
    afx_msg void OnDefaultLibrary();
    afx_msg void OnListDoubleClick(NMHDR* pNotify, LRESULT* pResult);
    afx_msg void OnListSelectionChanged(NMHDR* pNotify, LRESULT* pResult);
    afx_msg void OnHelpButton();

    DECLARE_MESSAGE_MAP()

private:
    void LoadFromFile();
    void FillList();
    void UpdateButtons();
    int SelectedIndex() const;
    bool SaveLibrary();
    void AddTool(const camcore::Tool& tool);

    std::filesystem::path m_path;
    std::string m_selectedToolId;
    camcore::ToolLibrary m_library;
    bool m_canEdit;
    CListCtrl m_list;
};
