// ToolDialogs.h : the dialog for editing one tool, with its speeds and feeds for
// each material, and the small dialogs it and the Machine dialog use.

#pragma once

#include <memory>

#include "resource.h"
#include "Materials.h"
#include "ToolLibrary.h"

class CFeedsPage;

// Asks for a name, such as a new machine's or material's.
class CNamePromptDlg : public CDialog
{
public:
    CNamePromptDlg(const std::wstring& title, const std::wstring& label, const std::wstring& name, CWnd* pParent);

    const std::wstring& GetName() const { return m_name; }

    enum { IDD = IDD_NAME_PROMPT };

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;

private:
    std::wstring m_title;
    std::wstring m_label;
    std::wstring m_name;
};

// A tool's speeds and feeds for one material: a material, and the Feeds & Speeds tab's fields.
class CToolFeedsDlg : public CDialog
{
public:
    // usedMaterialIds are the materials the tool's other rows already have, so they aren't offered.
    CToolFeedsDlg(const camcore::ToolFeeds& row, const camcore::MaterialList& materials,
                  const std::vector<std::string>& usedMaterialIds, CWnd* pParent);
    ~CToolFeedsDlg() override;

    const camcore::ToolFeeds& GetRow() const { return m_row; }

    enum { IDD = IDD_TOOL_FEEDS };

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    afx_msg void OnHelpButton();

    DECLARE_MESSAGE_MAP()

private:
    camcore::ToolFeeds m_row;
    camcore::MaterialList m_materials;
    std::vector<std::string> m_offered;     // material IDs, in the combo's order
    std::vector<std::string> m_used;
    CComboBox m_material;
    std::unique_ptr<CFeedsPage> m_page;
};

class CToolEditDlg : public CDialog
{
public:
    // otherTools is used to check that the tool number isn't already taken.
    CToolEditDlg(const camcore::Tool& tool, const camcore::ToolLibrary& otherTools,
                 const camcore::MaterialList& materials, CWnd* pParent);

    const camcore::Tool& GetTool() const { return m_tool; }

    enum { IDD = IDD_TOOL_EDIT };

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    afx_msg void OnTypeChanged();
    afx_msg void OnHelpButton();
    afx_msg void OnAddFeeds();
    afx_msg void OnEditFeeds();
    afx_msg void OnRemoveFeeds();
    afx_msg void OnFeedsDoubleClick(NMHDR* pNotify, LRESULT* pResult);
    afx_msg void OnFeedsSelectionChanged(NMHDR* pNotify, LRESULT* pResult);

    DECLARE_MESSAGE_MAP()

private:
    camcore::ToolType SelectedType() const;
    void UpdateFieldsForType();
    void SetNumber(int id, double value);
    bool ReadNumber(int id, const wchar_t* label, double& value, std::wstring& problems);
    bool ReadInteger(int id, const wchar_t* label, int& value, std::wstring& problems);
    void FillFeeds();
    int SelectedFeedsRow() const;
    std::vector<std::string> UsedMaterials(int except) const;

    camcore::Tool m_tool;
    camcore::ToolLibrary m_otherTools;
    camcore::MaterialList m_materials;
    std::vector<camcore::ToolFeeds> m_feeds;    // as edited, saved into m_tool on OK
    CComboBox m_type;
    CComboBox m_unit;
    CListCtrl m_feedsList;
};

// The material's name, or a note that it's no longer in the list.
std::wstring MaterialName(const camcore::MaterialList& materials, const std::string& id);
