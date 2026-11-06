// ToolDialogs.cpp : the tool library dialog and the dialog for editing one tool.

#include "stdafx.h"
#include "AlibreCAM.h"
#include "ToolDialogs.h"
#include "Help.h"
#include "ModelReader.h"
#include "Log.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace fs = std::filesystem;
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

    const wchar_t* UnitName(ToolUnit unit)
    {
        return (unit == ToolUnit::Inches) ? L"in" : L"mm";
    }

    void AppendProblem(std::wstring& problems, const std::wstring& problem)
    {
        if (!problems.empty())
            problems += L"\n";
        problems += problem;
    }
}

fs::path DefaultToolLibraryPath()
{
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer, MAX_PATH);

    fs::path folder = (length > 0 && length < MAX_PATH) ? fs::path(buffer) : fs::temp_directory_path();
    folder /= L"AlibreCAM";

    std::error_code ignored;
    fs::create_directories(folder, ignored);
    return folder / L"tools.json";
}

/////////////////////////////////////////////////////////////////////////////
// CToolEditDlg

BEGIN_MESSAGE_MAP(CToolEditDlg, CDialog)
    ON_BN_CLICKED(IDC_HELP_BUTTON, &CToolEditDlg::OnHelpButton)
    ON_CBN_SELCHANGE(IDC_TOOL_TYPE, &CToolEditDlg::OnTypeChanged)
END_MESSAGE_MAP()

void CToolEditDlg::OnHelpButton()
{
    ShowHelp(L"tool-edit");
}

CToolEditDlg::CToolEditDlg(const Tool& tool, const ToolLibrary& otherTools, CWnd* pParent)
    : CDialog(IDD_TOOL_EDIT, pParent)
    , m_tool(tool)
    , m_otherTools(otherTools)
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

    UpdateFieldsForType();
    return TRUE;
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

/////////////////////////////////////////////////////////////////////////////
// CToolLibraryDlg

BEGIN_MESSAGE_MAP(CToolLibraryDlg, CDialog)
    ON_BN_CLICKED(IDC_HELP_BUTTON, &CToolLibraryDlg::OnHelpButton)
    ON_BN_CLICKED(IDC_LIB_ADD, &CToolLibraryDlg::OnAdd)
    ON_BN_CLICKED(IDC_LIB_EDIT, &CToolLibraryDlg::OnEdit)
    ON_BN_CLICKED(IDC_LIB_COPY, &CToolLibraryDlg::OnCopy)
    ON_BN_CLICKED(IDC_LIB_DELETE, &CToolLibraryDlg::OnDelete)
    ON_BN_CLICKED(IDC_LIB_OTHER, &CToolLibraryDlg::OnOtherLibrary)
    ON_BN_CLICKED(IDC_LIB_DEFAULT, &CToolLibraryDlg::OnDefaultLibrary)
    ON_NOTIFY(NM_DBLCLK, IDC_LIB_LIST, &CToolLibraryDlg::OnListDoubleClick)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_LIB_LIST, &CToolLibraryDlg::OnListSelectionChanged)
END_MESSAGE_MAP()

void CToolLibraryDlg::OnHelpButton()
{
    ShowHelp(L"tool-library");
}

CToolLibraryDlg::CToolLibraryDlg(const fs::path& libraryPath, const std::string& selectedToolId, CWnd* pParent)
    : CDialog(IDD_TOOL_LIBRARY, pParent)
    , m_path(libraryPath)
    , m_selectedToolId(selectedToolId)
    , m_canEdit(false)
{
}

BOOL CToolLibraryDlg::OnInitDialog()
{
    CDialog::OnInitDialog();

    m_list.SubclassDlgItem(IDC_LIB_LIST, this);
    m_list.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    m_list.InsertColumn(0, L"No.", LVCFMT_RIGHT, 40);
    m_list.InsertColumn(1, L"Name", LVCFMT_LEFT, 200);
    m_list.InsertColumn(2, L"Type", LVCFMT_LEFT, 90);
    m_list.InsertColumn(3, L"Diameter", LVCFMT_RIGHT, 70);
    m_list.InsertColumn(4, L"Units", LVCFMT_LEFT, 45);

    LoadFromFile();
    return TRUE;
}

void CToolLibraryDlg::LoadFromFile()
{
    const ToolLibraryLoadResult loaded = LoadToolLibrary(m_path);

    m_library = ToolLibrary();
    m_canEdit = false;

    std::wstring note;
    switch (loaded.status)
    {
    case LoadStatus::Ok:
        m_library = loaded.library;
        m_canEdit = true;
        break;

    case LoadStatus::NotFound:
        m_canEdit = true;
        note = L"This file doesn't exist yet. It's created when you add a tool.";
        break;

    default:
        note = L"This library can't be used, and it won't be changed. " + FromUtf8(loaded.message);
        Log(L"[tools] could not load '%s': %s", m_path.c_str(), FromUtf8(loaded.message).c_str());
        break;
    }

    SetDlgItemText(IDC_LIB_PATH, m_path.c_str());
    SetDlgItemText(IDC_LIB_NOTE, note.c_str());
    FillList();
}

void CToolLibraryDlg::FillList()
{
    std::stable_sort(m_library.tools.begin(), m_library.tools.end(),
                     [](const Tool& a, const Tool& b) { return a.number < b.number; });

    m_list.DeleteAllItems();
    for (size_t i = 0; i < m_library.tools.size(); ++i)
    {
        const Tool& tool = m_library.tools[i];
        const int row = m_list.InsertItem(static_cast<int>(i), std::to_wstring(tool.number).c_str());
        m_list.SetItemText(row, 1, FromUtf8(tool.name).c_str());
        m_list.SetItemText(row, 2, TypeName(tool.type));
        m_list.SetItemText(row, 3, FormatNumber(tool.diameter).c_str());
        m_list.SetItemText(row, 4, UnitName(tool.unit));
    }
    UpdateButtons();
}

int CToolLibraryDlg::SelectedIndex() const
{
    return m_list.GetNextItem(-1, LVNI_SELECTED);
}

void CToolLibraryDlg::UpdateButtons()
{
    const BOOL hasSelection = (m_canEdit && SelectedIndex() >= 0) ? TRUE : FALSE;

    GetDlgItem(IDC_LIB_ADD)->EnableWindow(m_canEdit ? TRUE : FALSE);
    GetDlgItem(IDC_LIB_EDIT)->EnableWindow(hasSelection);
    GetDlgItem(IDC_LIB_COPY)->EnableWindow(hasSelection);
    GetDlgItem(IDC_LIB_DELETE)->EnableWindow(hasSelection);
}

void CToolLibraryDlg::OnListSelectionChanged(NMHDR* /*pNotify*/, LRESULT* pResult)
{
    UpdateButtons();
    *pResult = 0;
}

bool CToolLibraryDlg::SaveLibrary()
{
    const SaveResult saved = SaveToolLibrary(m_library, m_path);
    if (saved.ok)
    {
        Log(L"[tools] saved %zu tools to '%s'", m_library.tools.size(), m_path.c_str());
        SetDlgItemText(IDC_LIB_NOTE, L"");
        return true;
    }

    // Show what's actually in the file, not the change that wasn't saved.
    const std::wstring message = L"Could not save the tool library. " + FromUtf8(saved.message);
    Log(L"[tools] %s", message.c_str());
    MessageBox(message.c_str(), L"AlibreCAM", MB_OK | MB_ICONERROR);
    LoadFromFile();
    return false;
}

void CToolLibraryDlg::AddTool(const Tool& tool)
{
    CToolEditDlg dialog(tool, m_library, this);
    if (dialog.DoModal() != IDOK)
        return;

    m_library.tools.push_back(dialog.GetTool());
    if (SaveLibrary())
        FillList();
}

void CToolLibraryDlg::OnAdd()
{
    Tool tool;
    tool.id = NewToolId();
    tool.number = NextFreeToolNumber(m_library);
    AddTool(tool);
}

void CToolLibraryDlg::OnCopy()
{
    const int index = SelectedIndex();
    if (index < 0)
        return;

    Tool tool = m_library.tools[static_cast<size_t>(index)];
    tool.id = NewToolId();
    tool.number = NextFreeToolNumber(m_library);
    tool.name += " copy";
    AddTool(tool);
}

void CToolLibraryDlg::OnEdit()
{
    const int index = SelectedIndex();
    if (index < 0 || !m_canEdit)
        return;

    CToolEditDlg dialog(m_library.tools[static_cast<size_t>(index)], m_library, this);
    if (dialog.DoModal() != IDOK)
        return;

    m_library.tools[static_cast<size_t>(index)] = dialog.GetTool();
    if (SaveLibrary())
        FillList();
}

void CToolLibraryDlg::OnListDoubleClick(NMHDR* /*pNotify*/, LRESULT* pResult)
{
    OnEdit();
    *pResult = 0;
}

void CToolLibraryDlg::OnDelete()
{
    const int index = SelectedIndex();
    if (index < 0)
        return;

    const Tool& tool = m_library.tools[static_cast<size_t>(index)];

    std::wstring question = L"Delete " + FromUtf8(tool.name) + L"?";
    if (tool.id == m_selectedToolId)
        question += L"\n\nThis job has it selected, so the job will need another tool.";
    question += L"\n\nOther jobs that use this library may need it too.";

    if (MessageBox(question.c_str(), L"AlibreCAM", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    m_library.tools.erase(m_library.tools.begin() + index);
    if (SaveLibrary())
        FillList();
}

void CToolLibraryDlg::OnOtherLibrary()
{
    // No "must exist" flag, because naming a new file starts a new library.
    CFileDialog dialog(TRUE, L"json", L"tools.json", OFN_HIDEREADONLY | OFN_PATHMUSTEXIST,
                       L"Tool libraries (*.json)|*.json|All files (*.*)|*.*||", this);
    if (dialog.DoModal() != IDOK)
        return;

    m_path = static_cast<LPCWSTR>(dialog.GetPathName());
    LoadFromFile();
}

void CToolLibraryDlg::OnDefaultLibrary()
{
    m_path = DefaultToolLibraryPath();
    LoadFromFile();
}
