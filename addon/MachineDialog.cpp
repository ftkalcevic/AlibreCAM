// MachineDialog.cpp : the Machine dialog, which chooses the job's machine and edits
// machines, their tools, and the shared list of materials.

#include "stdafx.h"
#include "AlibreCAM.h"
#include "MachineDialog.h"
#include "Help.h"
#include "ModelReader.h"
#include "ProgramSettings.h"
#include "SettingsDialog.h"
#include "ToolDialogs.h"
#include "Log.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace fs = std::filesystem;
using namespace camcore;

namespace
{
    fs::path AppDataFolder()
    {
        wchar_t buffer[MAX_PATH] = {};
        const DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer, MAX_PATH);
        fs::path folder = (length > 0 && length < MAX_PATH) ? fs::path(buffer) : fs::temp_directory_path();
        folder /= L"AlibreCAM";
        std::error_code ignored;
        fs::create_directories(folder, ignored);
        return folder;
    }

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

    const RotaryAxis kRotaryAxes[] = { RotaryAxis::MinusX, RotaryAxis::PlusX, RotaryAxis::MinusY, RotaryAxis::PlusY };

    // The controls on each tab, shown only while it's chosen.
    const int kMachineTabControls[] = { IDC_MACHINE_NAME_LABEL, IDC_MACHINE_NAME, IDC_MACHINE_AXES_LABEL,
                                        IDC_MACHINE_AXES, IDC_MACHINE_ROTARY_LABEL, IDC_MACHINE_ROTARY,
                                        IDC_MACHINE_TRAVEL_LABEL, IDC_MACHINE_TRAVEL_X_LABEL, IDC_MACHINE_TRAVEL_X,
                                        IDC_MACHINE_TRAVEL_Y_LABEL, IDC_MACHINE_TRAVEL_Y, IDC_MACHINE_TRAVEL_Z_LABEL,
                                        IDC_MACHINE_TRAVEL_Z, IDC_MACHINE_SPINDLE_LABEL, IDC_MACHINE_SPINDLE,
                                        IDC_MACHINE_SPINDLE_UNIT };

    // The machine tab's number boxes, and what each is called in a message.
    struct NumberBox
    {
        int id;
        const wchar_t* label;
        double Machine::*field;
    };
    const NumberBox kNumberBoxes[] = {
        { IDC_MACHINE_TRAVEL_X, L"The X travel", &Machine::travelX },
        { IDC_MACHINE_TRAVEL_Y, L"The Y travel", &Machine::travelY },
        { IDC_MACHINE_TRAVEL_Z, L"The Z travel", &Machine::travelZ },
        { IDC_MACHINE_SPINDLE, L"The maximum spindle speed", &Machine::maxSpindleRpm },
    };
    const int kToolsTabControls[] = { IDC_LIB_LIST, IDC_LIB_ADD, IDC_LIB_EDIT, IDC_LIB_COPY, IDC_LIB_DELETE,
                                      IDC_LIB_IMPORT, IDC_LIB_EXPORT };
    const int kMaterialsTabControls[] = { IDC_MATERIAL_LIST, IDC_MATERIAL_ADD, IDC_MATERIAL_RENAME,
                                          IDC_MATERIAL_DELETE };
}

fs::path MachinesFolder()
{
    const fs::path folder = AppDataFolder() / L"machines";
    std::error_code ignored;
    fs::create_directories(folder, ignored);
    return folder;
}

fs::path MaterialsPath()
{
    return AppDataFolder() / L"materials.json";
}

std::string DefaultMachineId()
{
    const std::vector<MachineEntry> entries = FindMachines(MachinesFolder());

    const ProgramSettingsLoadResult settings = LoadProgramSettings(ProgramSettingsPath());
    const std::string last = settings.settings.lastMachineId;
    for (const MachineEntry& entry : entries)
    {
        if (entry.status == LoadStatus::Ok && !last.empty() && entry.machine.id == last)
            return last;
    }
    for (const MachineEntry& entry : entries)
    {
        if (entry.status == LoadStatus::Ok)
            return entry.machine.id;
    }

    // None yet, so the first one is made, with no tools.
    Machine machine;
    machine.id = NewStableId();
    machine.name = "Machine 1";
    const SaveResult saved = SaveMachine(machine, MachineFilePath(MachinesFolder(), machine.id));
    if (!saved.ok)
    {
        Log(L"[machine] could not make the first machine: %s", FromUtf8(saved.message).c_str());
        return std::string();
    }
    Log(L"[machine] made the first machine");
    RememberLastMachine(machine.id);
    return machine.id;
}

void RememberLastMachine(const std::string& id)
{
    const fs::path path = ProgramSettingsPath();
    ProgramSettingsLoadResult loaded = LoadProgramSettings(path);
    if (loaded.status != LoadStatus::Ok && loaded.status != LoadStatus::NotFound)
        return;     // damaged or newer, so left alone
    if (loaded.settings.lastMachineId == id)
        return;
    loaded.settings.lastMachineId = id;
    const SaveResult saved = SaveProgramSettings(loaded.settings, path);
    if (!saved.ok)
        Log(L"[machine] could not remember the last machine: %s", FromUtf8(saved.message).c_str());
}

/////////////////////////////////////////////////////////////////////////////
// CMachineDlg

BEGIN_MESSAGE_MAP(CMachineDlg, CDialog)
    ON_BN_CLICKED(IDC_HELP_BUTTON, &CMachineDlg::OnHelpButton)
    ON_NOTIFY(TCN_SELCHANGE, IDC_MACHINE_TABS, &CMachineDlg::OnTabChanged)
    ON_CBN_SELCHANGE(IDC_MACHINE_LIST, &CMachineDlg::OnMachineChosen)
    ON_BN_CLICKED(IDC_MACHINE_NEW, &CMachineDlg::OnNewMachine)
    ON_BN_CLICKED(IDC_MACHINE_COPY, &CMachineDlg::OnCopyMachine)
    ON_BN_CLICKED(IDC_MACHINE_DELETE, &CMachineDlg::OnDeleteMachine)
    ON_EN_KILLFOCUS(IDC_MACHINE_NAME, &CMachineDlg::OnMachineFieldChanged)
    ON_EN_KILLFOCUS(IDC_MACHINE_TRAVEL_X, &CMachineDlg::OnMachineFieldChanged)
    ON_EN_KILLFOCUS(IDC_MACHINE_TRAVEL_Y, &CMachineDlg::OnMachineFieldChanged)
    ON_EN_KILLFOCUS(IDC_MACHINE_TRAVEL_Z, &CMachineDlg::OnMachineFieldChanged)
    ON_EN_KILLFOCUS(IDC_MACHINE_SPINDLE, &CMachineDlg::OnMachineFieldChanged)
    ON_CBN_SELCHANGE(IDC_MACHINE_AXES, &CMachineDlg::OnMachineFieldChanged)
    ON_CBN_SELCHANGE(IDC_MACHINE_ROTARY, &CMachineDlg::OnMachineFieldChanged)
    ON_BN_CLICKED(IDC_LIB_ADD, &CMachineDlg::OnAddTool)
    ON_BN_CLICKED(IDC_LIB_EDIT, &CMachineDlg::OnEditTool)
    ON_BN_CLICKED(IDC_LIB_COPY, &CMachineDlg::OnCopyTool)
    ON_BN_CLICKED(IDC_LIB_DELETE, &CMachineDlg::OnDeleteTool)
    ON_BN_CLICKED(IDC_LIB_IMPORT, &CMachineDlg::OnImportTools)
    ON_BN_CLICKED(IDC_LIB_EXPORT, &CMachineDlg::OnExportTools)
    ON_NOTIFY(NM_DBLCLK, IDC_LIB_LIST, &CMachineDlg::OnToolDoubleClick)
    ON_NOTIFY(LVN_ITEMCHANGED, IDC_LIB_LIST, &CMachineDlg::OnToolSelectionChanged)
    ON_BN_CLICKED(IDC_MATERIAL_ADD, &CMachineDlg::OnAddMaterial)
    ON_BN_CLICKED(IDC_MATERIAL_RENAME, &CMachineDlg::OnRenameMaterial)
    ON_BN_CLICKED(IDC_MATERIAL_DELETE, &CMachineDlg::OnDeleteMaterial)
    ON_LBN_SELCHANGE(IDC_MATERIAL_LIST, &CMachineDlg::OnMaterialSelectionChanged)
    ON_LBN_DBLCLK(IDC_MATERIAL_LIST, &CMachineDlg::OnRenameMaterial)
END_MESSAGE_MAP()

void CMachineDlg::OnHelpButton()
{
    const int tab = m_tabs.GetCurSel();
    ShowHelp(tab == ToolsTab ? L"machine-tools" : tab == MaterialsTab ? L"machine-materials" : L"machine");
}

CMachineDlg::CMachineDlg(const std::string& machineId, const std::string& materialId, Tab startTab, CWnd* pParent)
    : CDialog(IDD_MACHINE, pParent)
    , m_machineId(machineId)
    , m_materialId(materialId)
    , m_startTab(startTab)
    , m_canEdit(false)
    , m_filling(false)
    , m_materialsWritable(false)
{
}

BOOL CMachineDlg::OnInitDialog()
{
    CDialog::OnInitDialog();

    m_machineList.SubclassDlgItem(IDC_MACHINE_LIST, this);
    m_tabs.SubclassDlgItem(IDC_MACHINE_TABS, this);
    m_tabs.InsertItem(MachineTab, L"Machine");
    m_tabs.InsertItem(ToolsTab, L"Tools");
    m_tabs.InsertItem(MaterialsTab, L"Materials");

    m_axes.SubclassDlgItem(IDC_MACHINE_AXES, this);
    m_axes.AddString(L"3");
    m_axes.AddString(L"4");
    m_rotary.SubclassDlgItem(IDC_MACHINE_ROTARY, this);
    for (RotaryAxis axis : kRotaryAxes)
        m_rotary.AddString(FromUtf8(RotaryAxisName(axis)).c_str());

    m_tools.SubclassDlgItem(IDC_LIB_LIST, this);
    m_tools.SetExtendedStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    m_tools.InsertColumn(0, L"No.", LVCFMT_RIGHT, 40);
    m_tools.InsertColumn(1, L"Name", LVCFMT_LEFT, 170);
    m_tools.InsertColumn(2, L"Type", LVCFMT_LEFT, 80);
    m_tools.InsertColumn(3, L"Diameter", LVCFMT_RIGHT, 60);
    m_tools.InsertColumn(4, L"Units", LVCFMT_LEFT, 40);
    m_tools.InsertColumn(5, L"Materials", LVCFMT_RIGHT, 60);

    m_materialList.SubclassDlgItem(IDC_MATERIAL_LIST, this);

    // The shared materials. A damaged or newer file is shown but not changed.
    const MaterialsLoadResult materials = LoadMaterials(MaterialsPath());
    m_materials = materials.list;
    m_materialsWritable = materials.status == LoadStatus::Ok || materials.status == LoadStatus::NotFound;
    if (!m_materialsWritable)
        Log(L"[machine] materials: %s", FromUtf8(materials.message).c_str());
    FillMaterials();

    FillMachines();

    m_tabs.SetCurSel(m_startTab);
    ShowTab(m_startTab);
    return TRUE;
}

void CMachineDlg::FillMachines()
{
    m_filling = true;
    m_entries = FindMachines(MachinesFolder());
    m_machineList.ResetContent();
    int chosen = -1;
    for (size_t i = 0; i < m_entries.size(); ++i)
    {
        const MachineEntry& entry = m_entries[i];
        const std::wstring text = (entry.status == LoadStatus::Ok)
                                      ? FromUtf8(entry.machine.name)
                                      : entry.path.filename().wstring() + L" (can't be read)";
        m_machineList.AddString(text.c_str());
        const std::string id = (entry.status == LoadStatus::Ok) ? entry.machine.id : entry.path.stem().u8string();
        if (id == m_machineId)
            chosen = static_cast<int>(i);
    }
    m_machineList.SetCurSel(chosen);
    m_filling = false;
    ShowMachine();
}

void CMachineDlg::ShowMachine()
{
    m_filling = true;
    const int index = m_machineList.GetCurSel();
    const MachineEntry* pEntry = (index >= 0 && static_cast<size_t>(index) < m_entries.size())
                                     ? &m_entries[static_cast<size_t>(index)]
                                     : nullptr;
    m_machine = Machine();
    m_canEdit = pEntry != nullptr && pEntry->status == LoadStatus::Ok;
    std::wstring note;
    if (pEntry == nullptr)
        note = m_machineId.empty() ? L"Choose a machine, or make one with New."
                                   : L"This job's machine isn't in the machines folder. Choose another.";
    else if (!m_canEdit)
        note = L"This machine can't be used, and it won't be changed. " + FromUtf8(pEntry->message);
    else
        m_machine = pEntry->machine;

    SetDlgItemText(IDC_LIB_NOTE, note.c_str());
    SetDlgItemText(IDC_MACHINE_NAME, FromUtf8(m_machine.name).c_str());
    m_axes.SetCurSel(m_machine.axes == 4 ? 1 : 0);
    for (const NumberBox& box : kNumberBoxes)
        SetDlgItemText(box.id, m_canEdit ? FormatNumber(m_machine.*box.field).c_str() : L"");
    for (int i = 0; i < 4; ++i)
    {
        if (kRotaryAxes[i] == m_machine.rotary)
            m_rotary.SetCurSel(i);
    }
    FillTools();
    m_filling = false;
    UpdateButtons();
}

void CMachineDlg::ShowTab(int tab)
{
    for (int id : kMachineTabControls)
        GetDlgItem(id)->ShowWindow(tab == MachineTab ? SW_SHOW : SW_HIDE);
    for (int id : kToolsTabControls)
        GetDlgItem(id)->ShowWindow(tab == ToolsTab ? SW_SHOW : SW_HIDE);
    for (int id : kMaterialsTabControls)
        GetDlgItem(id)->ShowWindow(tab == MaterialsTab ? SW_SHOW : SW_HIDE);
}

void CMachineDlg::OnTabChanged(NMHDR* /*pNotify*/, LRESULT* pResult)
{
    *pResult = 0;
    ApplyMachineFields();
    ShowTab(m_tabs.GetCurSel());
}

void CMachineDlg::UpdateButtons()
{
    const BOOL edit = m_canEdit ? TRUE : FALSE;
    const BOOL tool = (m_canEdit && SelectedTool() >= 0) ? TRUE : FALSE;
    const BOOL material = (m_materialsWritable && m_materialList.GetCurSel() >= 0) ? TRUE : FALSE;

    GetDlgItem(IDC_MACHINE_COPY)->EnableWindow(edit);
    GetDlgItem(IDC_MACHINE_DELETE)->EnableWindow(m_machineList.GetCurSel() >= 0 ? TRUE : FALSE);
    GetDlgItem(IDC_MACHINE_NAME)->EnableWindow(edit);
    GetDlgItem(IDC_MACHINE_AXES)->EnableWindow(edit);
    for (const NumberBox& box : kNumberBoxes)
        GetDlgItem(box.id)->EnableWindow(edit);
    GetDlgItem(IDC_MACHINE_ROTARY)->EnableWindow((m_canEdit && m_axes.GetCurSel() == 1) ? TRUE : FALSE);
    GetDlgItem(IDC_LIB_ADD)->EnableWindow(edit);
    GetDlgItem(IDC_LIB_IMPORT)->EnableWindow(edit);
    GetDlgItem(IDC_LIB_EXPORT)->EnableWindow((m_canEdit && !m_machine.tools.tools.empty()) ? TRUE : FALSE);
    GetDlgItem(IDC_LIB_EDIT)->EnableWindow(tool);
    GetDlgItem(IDC_LIB_COPY)->EnableWindow(tool);
    GetDlgItem(IDC_LIB_DELETE)->EnableWindow(tool);
    GetDlgItem(IDC_MATERIAL_ADD)->EnableWindow(m_materialsWritable ? TRUE : FALSE);
    GetDlgItem(IDC_MATERIAL_RENAME)->EnableWindow(material);
    GetDlgItem(IDC_MATERIAL_DELETE)->EnableWindow(material);
}

bool CMachineDlg::SaveCurrentMachine()
{
    const SaveResult saved = SaveMachine(m_machine, MachineFilePath(MachinesFolder(), m_machine.id));
    if (saved.ok)
    {
        Log(L"[machine] saved %s with %zu tools", FromUtf8(m_machine.name).c_str(), m_machine.tools.tools.size());
        return true;
    }
    MessageBox((L"Could not save the machine. " + FromUtf8(saved.message)).c_str(), L"AlibreCAM",
               MB_OK | MB_ICONERROR);
    return false;
}

bool CMachineDlg::ApplyMachineFields()
{
    if (!m_canEdit || m_filling)
        return true;

    CString name;
    GetDlgItemText(IDC_MACHINE_NAME, name);
    name.Trim();
    Machine changed = m_machine;
    changed.name = ToUtf8(static_cast<LPCWSTR>(name));
    changed.axes = (m_axes.GetCurSel() == 1) ? 4 : 3;
    const int rotary = m_rotary.GetCurSel();
    if (rotary >= 0 && rotary < 4)
        changed.rotary = kRotaryAxes[rotary];
    UpdateButtons();

    // The travel and spindle speed, each a number more than 0. A bad one goes back to what was saved.
    for (const NumberBox& box : kNumberBoxes)
    {
        CString text;
        GetDlgItemText(box.id, text);
        text.Trim();
        wchar_t* pEnd = nullptr;
        const double value = wcstod(text, &pEnd);
        if (text.IsEmpty() || pEnd == nullptr || *pEnd != L'\0' || !(value > 0.0))
        {
            MessageBox((std::wstring(box.label) + L" must be a number more than 0.").c_str(), L"AlibreCAM",
                       MB_OK | MB_ICONWARNING);
            SetDlgItemText(box.id, FormatNumber(m_machine.*box.field).c_str());
            return false;
        }
        changed.*box.field = value;
    }

    if (changed.name == m_machine.name && changed.axes == m_machine.axes && changed.rotary == m_machine.rotary &&
        changed.travelX == m_machine.travelX && changed.travelY == m_machine.travelY &&
        changed.travelZ == m_machine.travelZ && changed.maxSpindleRpm == m_machine.maxSpindleRpm)
        return true;
    if (changed.name.empty())
    {
        MessageBox(L"The machine needs a name.", L"AlibreCAM", MB_OK | MB_ICONWARNING);
        SetDlgItemText(IDC_MACHINE_NAME, FromUtf8(m_machine.name).c_str());
        return false;
    }

    const Machine before = m_machine;
    m_machine = changed;
    if (!SaveCurrentMachine())
    {
        m_machine = before;
        return false;
    }

    // Show the new name in the list.
    if (changed.name != before.name)
        FillMachines();
    else
        m_entries[static_cast<size_t>(m_machineList.GetCurSel())].machine = m_machine;
    return true;
}

void CMachineDlg::OnMachineFieldChanged()
{
    ApplyMachineFields();
}

void CMachineDlg::OnMachineChosen()
{
    if (m_filling)
        return;
    const int index = m_machineList.GetCurSel();
    if (index < 0 || static_cast<size_t>(index) >= m_entries.size())
        return;
    const MachineEntry& entry = m_entries[static_cast<size_t>(index)];
    if (entry.status != LoadStatus::Ok)
    {
        ShowMachine();
        return;
    }
    m_machineId = entry.machine.id;
    RememberLastMachine(m_machineId);
    ShowMachine();
}

void CMachineDlg::OnNewMachine()
{
    ApplyMachineFields();
    CNamePromptDlg prompt(L"New Machine", L"Name:", L"", this);
    if (prompt.DoModal() != IDOK)
        return;

    Machine machine;
    machine.id = NewStableId();
    machine.name = ToUtf8(prompt.GetName());
    const SaveResult saved = SaveMachine(machine, MachineFilePath(MachinesFolder(), machine.id));
    if (!saved.ok)
    {
        MessageBox((L"Could not save the machine. " + FromUtf8(saved.message)).c_str(), L"AlibreCAM",
                   MB_OK | MB_ICONERROR);
        return;
    }
    m_machineId = machine.id;
    RememberLastMachine(m_machineId);
    FillMachines();
}

void CMachineDlg::OnCopyMachine()
{
    if (!m_canEdit || !ApplyMachineFields())
        return;
    CNamePromptDlg prompt(L"Copy Machine", L"Name of the copy:", FromUtf8(m_machine.name) + L" copy", this);
    if (prompt.DoModal() != IDOK)
        return;

    // The tools keep their IDs, so jobs moved to the copy still find them.
    Machine copy = m_machine;
    copy.id = NewStableId();
    copy.name = ToUtf8(prompt.GetName());
    const SaveResult saved = SaveMachine(copy, MachineFilePath(MachinesFolder(), copy.id));
    if (!saved.ok)
    {
        MessageBox((L"Could not save the machine. " + FromUtf8(saved.message)).c_str(), L"AlibreCAM",
                   MB_OK | MB_ICONERROR);
        return;
    }
    m_machineId = copy.id;
    RememberLastMachine(m_machineId);
    FillMachines();
}

void CMachineDlg::OnDeleteMachine()
{
    const int index = m_machineList.GetCurSel();
    if (index < 0 || static_cast<size_t>(index) >= m_entries.size())
        return;

    int readable = 0;
    for (const MachineEntry& entry : m_entries)
        readable += (entry.status == LoadStatus::Ok) ? 1 : 0;
    const MachineEntry entry = m_entries[static_cast<size_t>(index)];
    if (entry.status == LoadStatus::Ok && readable <= 1)
    {
        MessageBox(L"This is the only machine, so it can't be deleted. Make another one first.", L"AlibreCAM",
                   MB_OK | MB_ICONINFORMATION);
        return;
    }

    const std::wstring name = (entry.status == LoadStatus::Ok) ? FromUtf8(entry.machine.name)
                                                               : entry.path.filename().wstring();
    const std::wstring question = L"Delete the machine " + name + L" and its tools?\n\nOther jobs that use it will "
                                  L"need another machine. This can't be undone.";
    if (MessageBox(question.c_str(), L"AlibreCAM", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    std::error_code error;
    if (!fs::remove(entry.path, error))
    {
        MessageBox(L"Could not delete the machine's file.", L"AlibreCAM", MB_OK | MB_ICONERROR);
        return;
    }
    Log(L"[machine] deleted %s", entry.path.c_str());

    // The job moves to the first machine left.
    m_machineId.clear();
    for (const MachineEntry& other : FindMachines(MachinesFolder()))
    {
        if (other.status == LoadStatus::Ok)
        {
            m_machineId = other.machine.id;
            break;
        }
    }
    RememberLastMachine(m_machineId);
    FillMachines();
}

/////////////////////////////////////////////////////////////////////////////
// The Tools tab

void CMachineDlg::FillTools()
{
    std::stable_sort(m_machine.tools.tools.begin(), m_machine.tools.tools.end(),
                     [](const Tool& a, const Tool& b) { return a.number < b.number; });

    m_tools.DeleteAllItems();
    for (size_t i = 0; i < m_machine.tools.tools.size(); ++i)
    {
        const Tool& tool = m_machine.tools.tools[i];
        const int row = m_tools.InsertItem(static_cast<int>(i), std::to_wstring(tool.number).c_str());
        m_tools.SetItemText(row, 1, FromUtf8(tool.name).c_str());
        m_tools.SetItemText(row, 2, TypeName(tool.type));
        m_tools.SetItemText(row, 3, FormatNumber(tool.diameter).c_str());
        m_tools.SetItemText(row, 4, UnitName(tool.unit));
        m_tools.SetItemText(row, 5, std::to_wstring(tool.feeds.size()).c_str());
    }
    UpdateButtons();
}

int CMachineDlg::SelectedTool() const
{
    return m_tools.GetNextItem(-1, LVNI_SELECTED);
}

void CMachineDlg::OnToolSelectionChanged(NMHDR* /*pNotify*/, LRESULT* pResult)
{
    *pResult = 0;
    UpdateButtons();
}

void CMachineDlg::AddTool(const Tool& tool)
{
    CToolEditDlg dialog(tool, m_machine.tools, m_materials, this);
    if (dialog.DoModal() != IDOK)
        return;

    m_machine.tools.tools.push_back(dialog.GetTool());
    if (!SaveCurrentMachine())
        m_machine.tools.tools.pop_back();
    FillTools();
}

void CMachineDlg::OnAddTool()
{
    if (!m_canEdit)
        return;
    Tool tool;
    tool.id = NewToolId();
    tool.number = NextFreeToolNumber(m_machine.tools);
    AddTool(tool);
}

void CMachineDlg::OnCopyTool()
{
    const int index = SelectedTool();
    if (index < 0 || !m_canEdit)
        return;
    Tool tool = m_machine.tools.tools[static_cast<size_t>(index)];
    tool.id = NewToolId();
    tool.number = NextFreeToolNumber(m_machine.tools);
    tool.name += " copy";
    AddTool(tool);
}

void CMachineDlg::OnEditTool()
{
    const int index = SelectedTool();
    if (index < 0 || !m_canEdit)
        return;

    Tool& tool = m_machine.tools.tools[static_cast<size_t>(index)];
    CToolEditDlg dialog(tool, m_machine.tools, m_materials, this);
    if (dialog.DoModal() != IDOK)
        return;

    const Tool before = tool;
    tool = dialog.GetTool();
    if (!SaveCurrentMachine())
        tool = before;
    FillTools();
}

void CMachineDlg::OnToolDoubleClick(NMHDR* /*pNotify*/, LRESULT* pResult)
{
    *pResult = 0;
    OnEditTool();
}

void CMachineDlg::OnDeleteTool()
{
    const int index = SelectedTool();
    if (index < 0 || !m_canEdit)
        return;

    const Tool tool = m_machine.tools.tools[static_cast<size_t>(index)];
    const std::wstring question = L"Delete " + FromUtf8(tool.name) + L"?\n\nJobs on this machine that use it will "
                                  L"need another tool.";
    if (MessageBox(question.c_str(), L"AlibreCAM", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    m_machine.tools.tools.erase(m_machine.tools.tools.begin() + index);
    if (!SaveCurrentMachine())
        m_machine.tools.tools.insert(m_machine.tools.tools.begin() + index, tool);
    FillTools();
}

void CMachineDlg::OnImportTools()
{
    if (!m_canEdit)
        return;
    CFileDialog dialog(TRUE, L"json", nullptr, OFN_FILEMUSTEXIST | OFN_HIDEREADONLY,
                       L"Tool libraries (*.json)|*.json|All files (*.*)|*.*||", this);
    if (dialog.DoModal() != IDOK)
        return;

    const fs::path path = static_cast<LPCWSTR>(dialog.GetPathName());
    const ToolLibraryLoadResult loaded = LoadToolLibrary(path);
    if (loaded.status != LoadStatus::Ok)
    {
        MessageBox((L"That tool library can't be read. " + FromUtf8(loaded.message)).c_str(), L"AlibreCAM",
                   MB_OK | MB_ICONWARNING);
        return;
    }

    // Tools already on the machine (the same ID) are replaced, or skipped.
    bool replace = false;
    const int shared = CountSharedTools(m_machine.tools, loaded.library);
    if (shared > 0)
    {
        const std::wstring question = std::to_wstring(shared) + L" of these tools are already on this machine. "
                                      L"Replace them with the imported ones?\n\nYes replaces them, No keeps the "
                                      L"machine's and imports the rest.";
        const int answer = MessageBox(question.c_str(), L"AlibreCAM", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (answer == IDCANCEL)
            return;
        replace = answer == IDYES;
    }

    const Machine before = m_machine;
    const ToolImportResult result = ImportTools(m_machine.tools, loaded.library, replace);
    if (!SaveCurrentMachine())
    {
        m_machine = before;
        FillTools();
        return;
    }
    FillTools();

    std::wstring report = L"Imported " + std::to_wstring(result.added) + L" new tools";
    if (result.replaced > 0)
        report += L", replaced " + std::to_wstring(result.replaced);
    if (result.skipped > 0)
        report += L", kept " + std::to_wstring(result.skipped) + L" the machine already had";
    report += L".";
    if (result.renumbered > 0)
        report += L" " + std::to_wstring(result.renumbered) + L" were given new numbers, as theirs were taken.";
    Log(L"[machine] %s", report.c_str());
    MessageBox(report.c_str(), L"AlibreCAM", MB_OK | MB_ICONINFORMATION);
}

void CMachineDlg::OnExportTools()
{
    if (!m_canEdit)
        return;
    const std::wstring suggested = FromUtf8(m_machine.name) + L" tools.json";
    CFileDialog dialog(FALSE, L"json", suggested.c_str(), OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST,
                       L"Tool libraries (*.json)|*.json|All files (*.*)|*.*||", this);
    if (dialog.DoModal() != IDOK)
        return;

    const fs::path path = static_cast<LPCWSTR>(dialog.GetPathName());
    const SaveResult saved = SaveToolLibrary(m_machine.tools, path);
    if (!saved.ok)
    {
        MessageBox((L"Could not export the tools. " + FromUtf8(saved.message)).c_str(), L"AlibreCAM",
                   MB_OK | MB_ICONERROR);
        return;
    }
    Log(L"[machine] exported %zu tools to %s", m_machine.tools.tools.size(), path.c_str());
}

/////////////////////////////////////////////////////////////////////////////
// The Materials tab

void CMachineDlg::FillMaterials()
{
    const int chosen = m_materialList.GetCurSel();
    m_materialList.ResetContent();
    for (const Material& material : m_materials.materials)
        m_materialList.AddString(FromUtf8(material.name).c_str());
    if (chosen >= 0 && chosen < m_materialList.GetCount())
        m_materialList.SetCurSel(chosen);
    UpdateButtons();
}

void CMachineDlg::OnMaterialSelectionChanged()
{
    UpdateButtons();
}

bool CMachineDlg::SaveMaterialList()
{
    const SaveResult saved = SaveMaterials(m_materials, MaterialsPath());
    if (saved.ok)
        return true;
    MessageBox((L"Could not save the materials. " + FromUtf8(saved.message)).c_str(), L"AlibreCAM",
               MB_OK | MB_ICONERROR);
    return false;
}

bool CMachineDlg::MaterialNameTaken(const std::wstring& name, const std::string& except) const
{
    const std::wstring wanted = CString(name.c_str()).MakeLower().GetString();
    for (const Material& material : m_materials.materials)
    {
        if (material.id != except && CString(FromUtf8(material.name).c_str()).MakeLower().GetString() == wanted)
            return true;
    }
    return false;
}

void CMachineDlg::OnAddMaterial()
{
    if (!m_materialsWritable)
        return;
    CNamePromptDlg prompt(L"New Material", L"Name:", L"", this);
    if (prompt.DoModal() != IDOK)
        return;
    if (MaterialNameTaken(prompt.GetName(), std::string()))
    {
        MessageBox(L"There's already a material with that name.", L"AlibreCAM", MB_OK | MB_ICONWARNING);
        return;
    }

    Material material;
    material.id = NewStableId();
    material.name = ToUtf8(prompt.GetName());
    m_materials.materials.push_back(material);
    if (!SaveMaterialList())
        m_materials.materials.pop_back();
    FillMaterials();
}

void CMachineDlg::OnRenameMaterial()
{
    const int index = m_materialList.GetCurSel();
    if (index < 0 || !m_materialsWritable)
        return;
    Material& material = m_materials.materials[static_cast<size_t>(index)];
    CNamePromptDlg prompt(L"Rename Material", L"Name:", FromUtf8(material.name), this);
    if (prompt.DoModal() != IDOK)
        return;
    if (MaterialNameTaken(prompt.GetName(), material.id))
    {
        MessageBox(L"There's already a material with that name.", L"AlibreCAM", MB_OK | MB_ICONWARNING);
        return;
    }
    const std::string before = material.name;
    material.name = ToUtf8(prompt.GetName());
    if (!SaveMaterialList())
        material.name = before;
    FillMaterials();
}

void CMachineDlg::OnDeleteMaterial()
{
    const int index = m_materialList.GetCurSel();
    if (index < 0 || !m_materialsWritable)
        return;
    const Material material = m_materials.materials[static_cast<size_t>(index)];
    const std::wstring question = L"Delete the material " + FromUtf8(material.name) + L"?\n\nEvery machine's tools "
                                  L"lose their speeds and feeds for it, and jobs that use it will need another "
                                  L"material.";
    if (MessageBox(question.c_str(), L"AlibreCAM", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    m_materials.materials.erase(m_materials.materials.begin() + index);
    if (!SaveMaterialList())
    {
        m_materials.materials.insert(m_materials.materials.begin() + index, material);
        FillMaterials();
        return;
    }

    // Its rows go from every machine's tools.
    for (const MachineEntry& entry : FindMachines(MachinesFolder()))
    {
        if (entry.status != LoadStatus::Ok)
            continue;
        Machine machine = entry.machine;
        bool changed = false;
        for (Tool& tool : machine.tools.tools)
        {
            const size_t before = tool.feeds.size();
            tool.feeds.erase(std::remove_if(tool.feeds.begin(), tool.feeds.end(),
                                            [&](const ToolFeeds& row) { return row.materialId == material.id; }),
                             tool.feeds.end());
            changed = changed || tool.feeds.size() != before;
        }
        if (changed && !SaveMachine(machine, entry.path).ok)
            Log(L"[machine] could not remove a deleted material from %s", entry.path.c_str());
    }
    if (m_materialId == material.id)
        m_materialId.clear();

    FillMaterials();
    FillMachines();     // the shown machine's tools changed too
}

/////////////////////////////////////////////////////////////////////////////
// Closing

void CMachineDlg::OnOK()
{
    if (!ApplyMachineFields())
        return;
    CDialog::OnOK();
}

void CMachineDlg::OnCancel()
{
    // Everything is saved as it's changed, so closing is the same as OK.
    OnOK();
}
