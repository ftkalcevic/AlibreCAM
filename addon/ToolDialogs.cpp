// ToolDialogs.cpp : the dialog for editing one tool, with its speeds and feeds for
// each material, and the small dialogs it and the Machine dialog use.

#include "stdafx.h"
#include "AlibreCAM.h"
#include "ToolDialogs.h"
#include "Help.h"
#include "ModelReader.h"
#include "OperationDialog.h"
#include "Log.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

using namespace camcore;

namespace
{
    std::wstring FormatNumber(double value)
    {
        wchar_t text[64] = {};
        _snwprintf_s(text, _countof(text), _TRUNCATE, L"%.10g", value);
        return text;
    }

    const wchar_t* TypeName(ToolType type)
    {
        return (type == ToolType::Drill) ? L"Drill" : L"Flat end mill";
    }

    void AppendProblem(std::wstring& problems, const std::wstring& problem)
    {
        if (!problems.empty())
            problems += L"\n";
        problems += problem;
    }
}

std::wstring MaterialName(const MaterialList& materials, const std::string& id)
{
    const Material* pMaterial = FindMaterial(materials, id);
    return (pMaterial != nullptr) ? FromUtf8(pMaterial->name) : L"(material not in the list)";
}

/////////////////////////////////////////////////////////////////////////////
// CNamePromptDlg

CNamePromptDlg::CNamePromptDlg(const std::wstring& title, const std::wstring& label, const std::wstring& name,
                               CWnd* pParent)
    : CDialog(IDD_NAME_PROMPT, pParent)
    , m_title(title)
    , m_label(label)
    , m_name(name)
{
}

BOOL CNamePromptDlg::OnInitDialog()
{
    CDialog::OnInitDialog();
    SetWindowText(m_title.c_str());
    SetDlgItemText(IDC_NAME_PROMPT_LABEL, m_label.c_str());
    SetDlgItemText(IDC_NAME_PROMPT_EDIT, m_name.c_str());

    CEdit* pEdit = static_cast<CEdit*>(GetDlgItem(IDC_NAME_PROMPT_EDIT));
    pEdit->SetSel(0, -1);
    pEdit->SetFocus();
    return FALSE;       // the focus is set
}

void CNamePromptDlg::OnOK()
{
    CString text;
    GetDlgItemText(IDC_NAME_PROMPT_EDIT, text);
    text.Trim();
    if (text.IsEmpty())
    {
        MessageBox(L"Enter a name.", L"AlibreCAM", MB_OK | MB_ICONWARNING);
        return;
    }
    m_name = static_cast<LPCWSTR>(text);
    CDialog::OnOK();
}

/////////////////////////////////////////////////////////////////////////////
// CToolFeedsDlg

BEGIN_MESSAGE_MAP(CToolFeedsDlg, CDialog)
    ON_BN_CLICKED(IDC_HELP_BUTTON, &CToolFeedsDlg::OnHelpButton)
END_MESSAGE_MAP()

void CToolFeedsDlg::OnHelpButton()
{
    ShowHelp(L"tool-feeds");
}

CToolFeedsDlg::CToolFeedsDlg(const ToolFeeds& row, const MaterialList& materials,
                             const std::vector<std::string>& usedMaterialIds, CWnd* pParent)
    : CDialog(IDD_TOOL_FEEDS, pParent)
    , m_row(row)
    , m_materials(materials)
    , m_used(usedMaterialIds)
{
}

CToolFeedsDlg::~CToolFeedsDlg() = default;

BOOL CToolFeedsDlg::OnInitDialog()
{
    CDialog::OnInitDialog();

    // The materials the tool has no other row for.
    m_material.SubclassDlgItem(IDC_FEEDS_MATERIAL, this);
    int chosen = -1;
    for (const Material& material : m_materials.materials)
    {
        if (std::find(m_used.begin(), m_used.end(), material.id) != m_used.end())
            continue;
        const int index = m_material.AddString(FromUtf8(material.name).c_str());
        m_offered.insert(m_offered.begin() + index, material.id);
        if (material.id == m_row.materialId)
            chosen = index;
    }
    // A row for a material that's since been deleted keeps it, so it can be seen.
    if (chosen < 0 && !m_row.materialId.empty())
    {
        chosen = m_material.AddString(MaterialName(m_materials, m_row.materialId).c_str());
        m_offered.insert(m_offered.begin() + chosen, m_row.materialId);
    }
    m_material.SetCurSel(chosen >= 0 ? chosen : 0);

    // The Feeds & Speeds tab's own fields and checks, over the placeholder.
    m_page = std::make_unique<CFeedsPage>(nullptr);
    if (m_page->CreateInTab(this))
    {
        CRect area;
        GetDlgItem(IDC_FEEDS_PAGE_AREA)->GetWindowRect(&area);
        ScreenToClient(&area);
        m_page->SetWindowPos(&CWnd::wndTop, area.left, area.top, area.Width(), area.Height(), SWP_SHOWWINDOW);
        SequenceStep step;
        step.feeds = m_row.feeds;
        m_page->Load(step);
    }
    else
    {
        Log(L"[tools] could not create the feeds page");
    }
    return TRUE;
}

void CToolFeedsDlg::OnOK()
{
    const int index = m_material.GetCurSel();
    if (index < 0 || static_cast<size_t>(index) >= m_offered.size())
    {
        MessageBox(m_offered.empty() ? L"Every material already has speeds and feeds for this tool. Add a "
                                       L"material on the Machine dialog's Materials tab first."
                                     : L"Choose a material.",
                   L"AlibreCAM", MB_OK | MB_ICONWARNING);
        return;
    }

    SequenceStep step;
    step.feeds = m_row.feeds;
    std::wstring problems;
    if (m_page == nullptr || m_page->GetSafeHwnd() == nullptr || !m_page->Save(step, problems))
    {
        MessageBox(problems.c_str(), L"AlibreCAM", MB_OK | MB_ICONWARNING);
        return;
    }

    m_row.materialId = m_offered[static_cast<size_t>(index)];
    m_row.feeds = step.feeds;
    CDialog::OnOK();
}

/////////////////////////////////////////////////////////////////////////////
// CToolEditDlg

BEGIN_MESSAGE_MAP(CToolEditDlg, CDialog)
    ON_BN_CLICKED(IDC_HELP_BUTTON, &CToolEditDlg::OnHelpButton)
    ON_CBN_SELCHANGE(IDC_TOOL_TYPE, &CToolEditDlg::OnTypeChanged)
    ON_BN_CLICKED(IDC_TOOL_FEEDS_ADD, &CToolEditDlg::OnAddFeeds)
    ON_BN_CLICKED(IDC_TOOL_FEEDS_EDIT, &CToolEditDlg::OnEditFeeds)
    ON_BN_CLICKED(IDC_TOOL_FEEDS_REMOVE, &CToolEditDlg::OnRemoveFeeds)
    ON_NOTIFY(NM_DBLCLK, IDC_TOOL_FEEDS_LIST, &CToolEditDlg::OnFeedsDoubleClick)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_TOOL_FEEDS_LIST, &CToolEditDlg::OnFeedsSelectionChanged)
END_MESSAGE_MAP()

void CToolEditDlg::OnHelpButton()
{
    ShowHelp(L"tool-edit");
}

CToolEditDlg::CToolEditDlg(const Tool& tool, const ToolLibrary& otherTools, const MaterialList& materials,
                           CWnd* pParent)
    : CDialog(IDD_TOOL_EDIT, pParent)
    , m_tool(tool)
    , m_otherTools(otherTools)
    , m_materials(materials)
    , m_feeds(tool.feeds)
{
}

void CToolEditDlg::SetNumber(int id, double value)
{
    SetDlgItemText(id, FormatNumber(value).c_str());
}

BOOL CToolEditDlg::OnInitDialog()
{
    CDialog::OnInitDialog();

    m_type.SubclassDlgItem(IDC_TOOL_TYPE, this);
    m_type.AddString(TypeName(ToolType::FlatEndMill));
    m_type.AddString(TypeName(ToolType::Drill));
    m_type.SetCurSel(m_tool.type == ToolType::Drill ? 1 : 0);

    m_unit.SubclassDlgItem(IDC_TOOL_UNIT, this);
    m_unit.AddString(L"Millimetres");
    m_unit.AddString(L"Inches");
    m_unit.SetCurSel(m_tool.unit == ToolUnit::Inches ? 1 : 0);

    SetNumber(IDC_TOOL_NUMBER, m_tool.number);
    SetDlgItemText(IDC_TOOL_NAME, FromUtf8(m_tool.name).c_str());
    SetNumber(IDC_TOOL_DIAMETER, m_tool.diameter);
    SetNumber(IDC_TOOL_FLUTE_LENGTH, m_tool.fluteLength);
    SetNumber(IDC_TOOL_OVERALL_LENGTH, m_tool.overallLength);
    SetNumber(IDC_TOOL_FLUTE_COUNT, m_tool.fluteCount);
    SetNumber(IDC_TOOL_POINT_ANGLE, m_tool.pointAngle);

    m_feedsList.SubclassDlgItem(IDC_TOOL_FEEDS_LIST, this);
    m_feedsList.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    m_feedsList.InsertColumn(0, L"Material", LVCFMT_LEFT, 150);
    m_feedsList.InsertColumn(1, L"Spindle (RPM)", LVCFMT_RIGHT, 85);
    m_feedsList.InsertColumn(2, L"Cut (mm/min)", LVCFMT_RIGHT, 85);
    FillFeeds();

    UpdateFieldsForType();
    return TRUE;
}

void CToolEditDlg::FillFeeds()
{
    m_feedsList.DeleteAllItems();
    for (size_t i = 0; i < m_feeds.size(); ++i)
    {
        const ToolFeeds& row = m_feeds[i];
        const int item = m_feedsList.InsertItem(static_cast<int>(i), MaterialName(m_materials, row.materialId).c_str());
        m_feedsList.SetItemText(item, 1, FormatNumber(row.feeds.spindleRpm).c_str());
        m_feedsList.SetItemText(item, 2, FormatNumber(row.feeds.cut).c_str());
    }
    const BOOL chosen = SelectedFeedsRow() >= 0 ? TRUE : FALSE;
    GetDlgItem(IDC_TOOL_FEEDS_EDIT)->EnableWindow(chosen);
    GetDlgItem(IDC_TOOL_FEEDS_REMOVE)->EnableWindow(chosen);
}

int CToolEditDlg::SelectedFeedsRow() const
{
    return m_feedsList.GetNextItem(-1, LVNI_SELECTED);
}

std::vector<std::string> CToolEditDlg::UsedMaterials(int except) const
{
    std::vector<std::string> used;
    for (size_t i = 0; i < m_feeds.size(); ++i)
    {
        if (static_cast<int>(i) != except)
            used.push_back(m_feeds[i].materialId);
    }
    return used;
}

void CToolEditDlg::OnAddFeeds()
{
    if (m_materials.materials.empty())
    {
        MessageBox(L"There are no materials yet. Add them on the Machine dialog's Materials tab.", L"AlibreCAM",
                   MB_OK | MB_ICONINFORMATION);
        return;
    }
    CToolFeedsDlg dialog(ToolFeeds(), m_materials, UsedMaterials(-1), this);
    if (dialog.DoModal() != IDOK)
        return;
    m_feeds.push_back(dialog.GetRow());
    FillFeeds();
}

void CToolEditDlg::OnEditFeeds()
{
    const int row = SelectedFeedsRow();
    if (row < 0)
        return;
    CToolFeedsDlg dialog(m_feeds[static_cast<size_t>(row)], m_materials, UsedMaterials(row), this);
    if (dialog.DoModal() != IDOK)
        return;
    m_feeds[static_cast<size_t>(row)] = dialog.GetRow();
    FillFeeds();
}

void CToolEditDlg::OnRemoveFeeds()
{
    const int row = SelectedFeedsRow();
    if (row < 0)
        return;
    m_feeds.erase(m_feeds.begin() + row);
    FillFeeds();
}

void CToolEditDlg::OnFeedsDoubleClick(NMHDR* /*pNotify*/, LRESULT* pResult)
{
    *pResult = 0;
    OnEditFeeds();
}

void CToolEditDlg::OnFeedsSelectionChanged(NMHDR* /*pNotify*/, LRESULT* pResult)
{
    *pResult = 0;
    const BOOL chosen = SelectedFeedsRow() >= 0 ? TRUE : FALSE;
    GetDlgItem(IDC_TOOL_FEEDS_EDIT)->EnableWindow(chosen);
    GetDlgItem(IDC_TOOL_FEEDS_REMOVE)->EnableWindow(chosen);
}

bool CToolEditDlg::IsDrillSelected() const
{
    return m_type.GetCurSel() == 1;
}

void CToolEditDlg::UpdateFieldsForType()
{
    const BOOL drill = IsDrillSelected() ? TRUE : FALSE;

    GetDlgItem(IDC_TOOL_FLUTE_COUNT)->EnableWindow(drill ? FALSE : TRUE);
    GetDlgItem(IDC_TOOL_POINT_ANGLE)->EnableWindow(drill);
}

void CToolEditDlg::OnTypeChanged()
{
    UpdateFieldsForType();
}

bool CToolEditDlg::ReadNumber(int id, const wchar_t* label, double& value, std::wstring& problems)
{
    CString text;
    GetDlgItemText(id, text);
    text.Trim();

    wchar_t* pEnd = nullptr;
    const double parsed = wcstod(text, &pEnd);
    if (text.IsEmpty() || pEnd == nullptr || *pEnd != L'\0')
    {
        AppendProblem(problems, std::wstring(label) + L" must be a number.");
        return false;
    }

    value = parsed;
    return true;
}

bool CToolEditDlg::ReadInteger(int id, const wchar_t* label, int& value, std::wstring& problems)
{
    double parsed = 0.0;
    if (!ReadNumber(id, label, parsed, problems))
        return false;

    if (parsed != static_cast<double>(static_cast<int>(parsed)))
    {
        AppendProblem(problems, std::wstring(label) + L" must be a whole number.");
        return false;
    }

    value = static_cast<int>(parsed);
    return true;
}

void CToolEditDlg::OnOK()
{
    Tool tool = m_tool;
    std::wstring problems;

    tool.type = IsDrillSelected() ? ToolType::Drill : ToolType::FlatEndMill;
    tool.unit = (m_unit.GetCurSel() == 1) ? ToolUnit::Inches : ToolUnit::Millimetres;
    tool.feeds = m_feeds;

    CString name;
    GetDlgItemText(IDC_TOOL_NAME, name);
    name.Trim();
    tool.name = ToUtf8(static_cast<LPCWSTR>(name));

    ReadInteger(IDC_TOOL_NUMBER, L"The tool number", tool.number, problems);
    ReadNumber(IDC_TOOL_DIAMETER, L"The diameter", tool.diameter, problems);
    ReadNumber(IDC_TOOL_FLUTE_LENGTH, L"The flute length", tool.fluteLength, problems);
    ReadNumber(IDC_TOOL_OVERALL_LENGTH, L"The overall length", tool.overallLength, problems);

    if (tool.type == ToolType::Drill)
        ReadNumber(IDC_TOOL_POINT_ANGLE, L"The point angle", tool.pointAngle, problems);
    else
        ReadInteger(IDC_TOOL_FLUTE_COUNT, L"The flute count", tool.fluteCount, problems);

    // Only check the values themselves once they've all been read as numbers.
    if (problems.empty())
    {
        for (const std::string& problem : ValidateTool(tool))
            AppendProblem(problems, FromUtf8(problem));

        for (const Tool& other : m_otherTools.tools)
        {
            if (other.id != tool.id && other.number == tool.number)
                AppendProblem(problems, L"Tool " + std::to_wstring(tool.number) + L" is already " +
                                            FromUtf8(other.name) + L".");
        }
    }

    if (!problems.empty())
    {
        MessageBox(problems.c_str(), L"AlibreCAM", MB_OK | MB_ICONWARNING);
        return;
    }

    m_tool = tool;
    CDialog::OnOK();
}
