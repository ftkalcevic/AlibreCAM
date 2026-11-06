// CamTabCommand.cpp : the two-way toggle command that owns the CAM tab.

#include "stdafx.h"
#include "AlibreCAM.h"
#include "AddOnInterface.h"
#include "CamTabCommand.h"
#include "DisplaySettings.h"
#include "DrillToolpath.h"
#include "FacingToolpath.h"
#include "HolePocketToolpath.h"
#include "PocketToolpath.h"
#include "ProfileToolpath.h"
#include "FileIO.h"
#include "PostDialog.h"
#include "ScriptedPost.h"
#include "SettingsDialog.h"
#include "StockDialog.h"
#include "PseudoPost.h"
#include "GeometryReader.h"
#include "Help.h"
#include "ModelReader.h"
#include "MachineDialog.h"
#include "ToolDialogs.h"
#include "Log.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace fs = std::filesystem;

namespace
{
    // The resize timer also checks whether the tab has focus.
    const UINT_PTR kResizeTimer = 1;
    const UINT kResizeMilliseconds = 250;
    const UINT WM_APP_LOAD_JOB = WM_APP + 1;
    const UINT WM_APP_CLEAR_SELECTION = WM_APP + 2;

    // m_editIndex while the dialog makes a new step.
    const size_t kNewStep = static_cast<size_t>(-1);

    enum
    {
        IDC_CAM_TREE = 14001,
        IDC_CAM_STATUS,
        IDC_CAM_SAVE,
        IDC_CAM_REFRESH,
        IDC_CAM_FOREGROUND,
        IDC_CAM_TOOLBAR,
        IDC_CAM_SHOW_TOOLPATHS,
        IDC_CAM_SETTINGS_BAR,
        IDC_CAM_SETTINGS,
        IDC_CAM_HELP,
        IDC_CAM_SHOW_STOCK,

        // Tree menu commands
        ID_CAM_ADD_ORIGIN,
        ID_CAM_ADD_FOLDER,
        ID_CAM_ADD_DRILL,
        ID_CAM_ADD_HOLE_POCKETING,
        ID_CAM_ADD_FACING,
        ID_CAM_ADD_PROFILING,
        ID_CAM_ADD_POCKETING,
        ID_CAM_EDIT_STEP,
        ID_CAM_STOCK_BOX,
        ID_CAM_STOCK_PART_BOX,
        ID_CAM_STOCK_IMPORT,
        ID_CAM_STOCK_EDIT,
        ID_CAM_STOCK_DELETE,
        ID_CAM_MACHINE_EDIT,
        ID_CAM_MATERIAL_EDIT,
        ID_CAM_MATERIAL_NONE,

        // Choosing a material from the Material node's menu: the first, and room for many.
        ID_CAM_MATERIAL_FIRST = 15000,
        ID_CAM_MATERIAL_LAST = 15999,
        ID_CAM_RENAME_STEP,
        ID_CAM_DUPLICATE_STEP,
        ID_CAM_SUPPRESS_STEP,
        ID_CAM_GENERATE,
        ID_CAM_POST,
        ID_CAM_DELETE_STEP
    };

    std::wstring FileName(const fs::path& path)
    {
        return path.filename().wstring();
    }

    int AskYesNo(const std::wstring& question)
    {
        return ::MessageBoxW(theApp.m_hAlibreWnd, question.c_str(), L"AlibreCAM", MB_YESNO | MB_ICONQUESTION);
    }

    void ShowError(const std::wstring& message)
    {
        ::MessageBoxW(theApp.m_hAlibreWnd, message.c_str(), L"AlibreCAM", MB_OK | MB_ICONERROR);
    }

    // "Set Origin 2", using the lowest number no step already has.
    std::string NewStepName(const camcore::Job& job, const std::string& base)
    {
        for (int number = 1;; ++number)
        {
            const std::string name = base + " " + std::to_string(number);
            const bool used = std::any_of(job.sequence.begin(), job.sequence.end(),
                                          [&](const camcore::SequenceStep& step) { return step.name == name; });
            if (!used)
                return name;
        }
    }

    // "Drill 2", unique across the operations in every folder.
    std::string NewOperationName(const camcore::Job& job, const std::string& base)
    {
        for (int number = 1;; ++number)
        {
            const std::string name = base + " " + std::to_string(number);
            bool used = false;
            for (const camcore::SequenceStep& step : job.sequence)
            {
                used = used || std::any_of(step.children.begin(), step.children.end(),
                                           [&](const camcore::SequenceStep& child) { return child.name == name; });
            }
            if (!used)
                return name;
        }
    }

    // A tree item's data for a step: the step's index in the high half, and the
    // operation's index plus one in the low half, or 0 for a top-level step.
    DWORD_PTR StepItemData(const StepRef& ref)
    {
        const DWORD_PTR child = ref.IsOperation() ? static_cast<DWORD_PTR>(ref.child) + 1 : 0;
        return (static_cast<DWORD_PTR>(ref.step) << 32) | child;
    }

    StepRef StepFromItemData(DWORD_PTR data)
    {
        const DWORD_PTR child = data & 0xFFFFFFFFu;
        return StepRef{ static_cast<size_t>(data >> 32), child == 0 ? StepRef::kNone : static_cast<size_t>(child - 1) };
    }

    // Applies a drop to job. Returns false, changing nothing, if it isn't allowed
    // or wouldn't change anything. moved is where the step ends up.
    bool ApplyMove(camcore::Job& job, const StepRef& from, const StepRef& target, DropSide side, StepRef& moved)
    {
        size_t newIndex = 0;
        const size_t after = (side == DropSide::After) ? 1 : 0;

        if (!from.IsOperation())
        {
            // Top-level steps go before or after another top-level step.
            if (target.IsOperation() || side == DropSide::Into ||
                !camcore::MoveStep(job, from.step, target.step + after, newIndex))
                return false;
            moved = StepRef{ newIndex };
            return true;
        }

        if (side == DropSide::Into)
        {
            // Onto a folder: at its end.
            if (target.IsOperation() || target.step >= job.sequence.size() ||
                !camcore::MoveOperation(job, from.step, from.child, target.step,
                                        job.sequence[target.step].children.size(), newIndex))
                return false;
        }
        else if (!target.IsOperation() ||
                 !camcore::MoveOperation(job, from.step, from.child, target.step, target.child + after, newIndex))
        {
            return false;
        }

        moved = StepRef{ target.step, newIndex };
        return true;
    }
}

/////////////////////////////////////////////////////////////////////////////
// CCamTabCommand

CCamTabCommand::CCamTabCommand(CAddOnInterface* pAddOn, const std::wstring& sessionId)
    : CCommandBase(L"tab")
    , m_pAddOn(pAddOn)
    , m_sessionId(sessionId)
    , m_pWnd(nullptr)
    , m_saveBlocked(false)
    , m_unsavedChanges(false)
    , m_selected()
    , m_pView(nullptr)
    , m_haveBounds(false)
    , m_pOriginDlg(nullptr)
    , m_editIndex(kNewStep)
    , m_pOperationDlg(nullptr)
    , m_showingFeatures(false)
    , m_operationTarget()
    , m_pPartBoxDlg(nullptr)
    , m_stockShapeValid(false)
    , m_filterChanged(false)
    , m_previousFilter()
{
}

CCamTabCommand::~CCamTabCommand()
{
}

IADSessionPtr CCamTabCommand::GetSession() const
{
    return theApp.m_pRoot->GetSessions()->GetItem(_variant_t(m_sessionId.c_str()));
}

void CCamTabCommand::SetStatus(const std::wstring& text)
{
    Log(L"[tab] status: %s", text.c_str());
    m_jobStatus = text;
    ShowStatus();
}

void CCamTabCommand::ShowStatus()
{
    if (m_pWnd == nullptr)
        return;

    std::wstring text;
    for (const std::wstring* pLine : { &m_jobStatus, &m_toolStatus, &m_sequenceStatus })
    {
        if (pLine->empty())
            continue;
        if (!text.empty())
            text += L"\n";
        text += *pLine;
    }
    m_pWnd->SetStatusText(text);
}

void CCamTabCommand::SetSaveBlocked(bool blocked)
{
    m_saveBlocked = blocked;
    if (m_pWnd != nullptr)
        m_pWnd->EnableSave(!blocked);
}

STDMETHODIMP CCamTabCommand::IsTwoWayToggle(VARIANT_BOOL* pValue)
{
    if (pValue == nullptr)
        return E_POINTER;
    *pValue = VARIANT_TRUE;
    return S_OK;
}

STDMETHODIMP CCamTabCommand::AddTab(VARIANT_BOOL* pValue)
{
    if (pValue == nullptr)
        return E_POINTER;
    *pValue = VARIANT_TRUE;
    return S_OK;
}

STDMETHODIMP CCamTabCommand::get_TabName(BSTR* pName)
{
    if (pName == nullptr)
        return E_POINTER;
    *pName = _bstr_t(L"CAM").Detach();
    return S_OK;
}

STDMETHODIMP CCamTabCommand::OnShowUI(__int64 hWnd)
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    if (m_pWnd == nullptr)
    {
        m_pWnd = new CCamTabWnd(this);
        if (!m_pWnd->CreateInTab(reinterpret_cast<HWND>(hWnd)))
        {
            Log(L"[tab] could not create the tab window");

            // If creation got far enough, PostNcDestroy already cleared this.
            delete m_pWnd;
            m_pWnd = nullptr;
        }
    }
    return S_OK;
}

STDMETHODIMP CCamTabCommand::OnTerminate()
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    Log(L"[tab] OnTerminate");

    CloseOriginDialog(true);
    CloseOperationDialog();
    ClosePartBoxDialog(true);
    StopView(L"tab closing");
    WarnIfUnsaved();
    DestroyWindowIfOpen();
    m_site = nullptr;
    LeaveAddOn();
    return S_OK;
}

void CCamTabCommand::DestroyWindowIfOpen()
{
    if (m_pWnd != nullptr)
    {
        CCamTabWnd* pWnd = m_pWnd;
        m_pWnd = nullptr;
        pWnd->DetachFromCommand();
        pWnd->DestroyWindow();
    }
}

void CCamTabCommand::LeaveAddOn()
{
    // This drops the add-on's reference, which may be the last one.
    AddRef();
    if (m_pAddOn != nullptr)
    {
        CAddOnInterface* pAddOn = m_pAddOn;
        m_pAddOn = nullptr;
        pAddOn->RemoveTabForSession(m_sessionId);
    }
    Release();
}

void CCamTabCommand::CloseFromAddOn()
{
    m_pAddOn = nullptr;
    CloseOriginDialog(true);
    CloseOperationDialog();
    ClosePartBoxDialog(true);
    StopView(L"add-on unloading");
    WarnIfUnsaved();
    DestroyWindowIfOpen();
}

void CCamTabCommand::OnWindowClosing()
{
    // Closing the assembly gets here. It can't be cancelled, and the session
    // may already be closing, so the warning doesn't call Alibre.
    WarnIfUnsaved();
}

void CCamTabCommand::MarkJobChanged()
{
    if (m_saveBlocked)
        return;

    m_unsavedChanges = true;
    SetStatus(L"Unsaved changes.");
}

void CCamTabCommand::WarnIfUnsaved()
{
    if (!m_unsavedChanges)
        return;

    m_unsavedChanges = false;
    Log(L"[tab] warning about unsaved changes");

    if (m_camPath.empty())
    {
        ::MessageBoxW(theApp.m_hAlibreWnd,
                      L"The assembly was never saved, so its unsaved CAM changes can't be kept.",
                      L"AlibreCAM", MB_OK | MB_ICONWARNING);
        Log(L"[tab] unsaved changes lost: the assembly has no file");
        return;
    }

    const std::wstring fileName = FileName(m_camPath);
    if (AskYesNo(L"Save CAM changes to " + fileName + L"?\n\nNo discards them.") != IDYES)
    {
        Log(L"[tab] unsaved changes discarded");
        return;
    }

    const camcore::SaveResult saved = camcore::SaveJob(m_job, m_camPath);
    Log(L"[tab] saved on closing: %s", saved.ok ? L"ok" : FromUtf8(saved.message).c_str());
    if (!saved.ok)
        ShowError(L"Could not save " + fileName + L". " + FromUtf8(saved.message));
}

void CCamTabCommand::OnWindowGone()
{
    // Closing the assembly destroys the tab window without calling OnTerminate.
    // The session may be going, so nothing here calls Alibre. Alibre ends the
    // view command itself.
    Log(L"[tab] window destroyed by Alibre");
    m_pWnd = nullptr;
    CloseOriginDialog(false);
    CloseOperationDialog();
    ClosePartBoxDialog(false);
    LeaveAddOn();
}

/////////////////////////////////////////////////////////////////////////////
// Loading and saving the .cam file

void CCamTabCommand::ReloadTools()
{
    // A job with no machine takes the last one used. That alone isn't an unsaved change.
    if (m_job.machineId.empty() && !m_saveBlocked)
        m_job.machineId = DefaultMachineId();

    m_machine = camcore::Machine();
    m_library = camcore::ToolLibrary();
    std::wstring machineName;
    const camcore::MachineLoadResult loaded =
        m_job.machineId.empty() ? camcore::MachineLoadResult()
                                : camcore::LoadMachine(camcore::MachineFilePath(MachinesFolder(), m_job.machineId));
    if (loaded.status == camcore::LoadStatus::Ok)
    {
        m_machine = loaded.machine;
        m_library = m_machine.tools;
        machineName = FromUtf8(m_machine.name);

        if (m_library.tools.empty())
            m_toolStatus = L"This machine has no tools. Double-click Tools to add one.";
        else if (m_job.selectedToolId.empty())
            m_toolStatus = L"No tool selected.";
        else if (const camcore::Tool* pTool = camcore::FindToolById(m_library, m_job.selectedToolId))
            m_toolStatus = L"Selected tool: T" + std::to_wstring(pTool->number) + L" " + FromUtf8(pTool->name);
        else
            m_toolStatus = L"The selected tool isn't on this machine. Select another.";
    }
    else if (loaded.status == camcore::LoadStatus::NotFound || m_job.machineId.empty())
    {
        machineName = L"not found";
        m_toolStatus = L"This job's machine isn't in the machines folder. Double-click Machine to choose one.";
    }
    else
    {
        machineName = L"can't be read";
        m_toolStatus = L"This job's machine can't be used. " + FromUtf8(loaded.message);
    }

    // The shared materials.
    const camcore::MaterialsLoadResult materials = camcore::LoadMaterials(MaterialsPath());
    m_materials = materials.list;
    if (materials.status != camcore::LoadStatus::Ok && materials.status != camcore::LoadStatus::NotFound)
        Log(L"[machine] materials: %s", FromUtf8(materials.message).c_str());

    Log(L"[machine] %s: %zu tools. %s", machineName.c_str(), m_library.tools.size(), m_toolStatus.c_str());

    if (m_pWnd != nullptr)
    {
        m_pWnd->ShowMachine(machineName);
        m_pWnd->ShowTools(m_library, m_job.selectedToolId);
        m_pWnd->ShowMaterial(GetMaterialName());
    }

    SetStatus(m_jobStatus);
}

std::wstring CCamTabCommand::GetMaterialName() const
{
    if (m_job.materialId.empty())
        return L"none";
    const camcore::Material* pMaterial = camcore::FindMaterial(m_materials, m_job.materialId);
    return (pMaterial != nullptr) ? FromUtf8(pMaterial->name) : L"not in the list";
}

void CCamTabCommand::ChooseMaterial(const std::string& id)
{
    if (m_saveBlocked || id == m_job.materialId)
        return;
    m_job.materialId = id;
    MarkJobChanged();
    if (m_pWnd != nullptr)
        m_pWnd->ShowMaterial(GetMaterialName());
}

void CCamTabCommand::OpenMachineDialog(int tab)
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    if (m_saveBlocked)
        return;

    CMachineDlg dialog(m_job.machineId, m_job.materialId, static_cast<CMachineDlg::Tab>(tab), m_pWnd);
    dialog.DoModal();

    if (dialog.GetMachineId() != m_job.machineId)
    {
        m_job.machineId = dialog.GetMachineId();
        Log(L"[machine] the job's machine is now %s", FromUtf8(m_job.machineId).c_str());
        MarkJobChanged();
    }
    if (dialog.GetMaterialId() != m_job.materialId)
    {
        m_job.materialId = dialog.GetMaterialId();
        MarkJobChanged();
    }
    ReloadTools();
}

void CCamTabCommand::SelectToolByIndex(size_t index)
{
    if (m_saveBlocked || index >= m_library.tools.size())
        return;

    const std::string& id = m_library.tools[index].id;
    if (id == m_job.selectedToolId)
        return;

    m_job.selectedToolId = id;
    MarkJobChanged();

    if (m_pWnd != nullptr)
        m_pWnd->MarkSelectedTool(m_library, m_job.selectedToolId);

    const camcore::Tool& tool = m_library.tools[index];
    m_toolStatus = L"Selected tool: T" + std::to_wstring(tool.number) + L" " + FromUtf8(tool.name);
    SetStatus(m_jobStatus);
}

void CCamTabCommand::LoadJobFile()
{
    ReadJobFile();

    m_selected = m_job.sequence.empty() ? StepRef() : StepRef{ 0 };
    ShowSequence();
    ReadModelSize();
    RebuildScene();
}

void CCamTabCommand::ReadJobFile()
{
    AssemblyInfo current;
    bool haveAssembly = false;
    try
    {
        haveAssembly = ReadAssemblyInfo(GetSession(), current);
    }
    catch (const _com_error& error)
    {
        Log(L"[tab] could not find the session: hr=0x%08X", static_cast<unsigned>(error.Error()));
    }

    if (!haveAssembly)
    {
        SetSaveBlocked(true);
        SetStatus(L"Could not read the assembly.");
        return;
    }

    m_job = camcore::Job();
    m_job.assembly = current.identity;
    m_unsavedChanges = false;
    SetSaveBlocked(false);

    if (current.path.empty())
    {
        SetStatus(L"New job. Save the assembly before saving CAM data.");
        ReloadTools();
        return;
    }

    m_camPath = camcore::CamPathForAssembly(current.path);
    const std::wstring fileName = FileName(m_camPath);
    const camcore::LoadResult loaded = camcore::LoadJob(m_camPath);

    switch (loaded.status)
    {
    case camcore::LoadStatus::Ok:
        break;

    case camcore::LoadStatus::NotFound:
        SetStatus(L"New job. Not saved yet.");
        ReloadTools();
        return;

    case camcore::LoadStatus::NewerVersion:
        SetSaveBlocked(true);
        SetStatus(fileName + L" was made by a newer version of AlibreCAM, so it wasn't opened. " +
                  FromUtf8(loaded.message));
        return;

    default:
        SetSaveBlocked(true);
        SetStatus(fileName + L" couldn't be opened, and saving is off to protect it. " +
                  FromUtf8(loaded.message));
        return;
    }

    const camcore::IdentityMatch match = camcore::CompareIdentity(loaded.job.assembly, current.identity);
    if (match == camcore::IdentityMatch::Different)
    {
        const std::wstring storedName = FromUtf8(loaded.job.assembly.name);
        const int answer = AskYesNo(fileName + L" was made for a different assembly (" + storedName +
                                    L").\n\nUse it with this assembly anyway?\n\n"
                                    L"Yes links it to this assembly the next time you save.\n"
                                    L"No leaves the file as it is and turns saving off.");
        if (answer != IDYES)
        {
            SetSaveBlocked(true);
            SetStatus(fileName + L" belongs to a different assembly (" + storedName +
                      L"), so it wasn't opened.");
            return;
        }

        m_job = loaded.job;
        m_job.assembly = current.identity;
        SetStatus(L"Using " + fileName + L". Save to link it to this assembly.");
        ReloadTools();
        return;
    }

    m_job = loaded.job;
    m_job.assembly = current.identity;
    SetStatus(L"Loaded " + fileName + L".");
    ReloadTools();
}

void CCamTabCommand::SaveJobFile()
{
    if (m_saveBlocked)
        return;

    AssemblyInfo current;
    bool haveAssembly = false;
    try
    {
        haveAssembly = ReadAssemblyInfo(GetSession(), current);
    }
    catch (const _com_error& error)
    {
        Log(L"[tab] could not find the session: hr=0x%08X", static_cast<unsigned>(error.Error()));
    }

    if (!haveAssembly)
    {
        SetStatus(L"Could not read the assembly, so nothing was saved.");
        return;
    }

    if (current.path.empty())
    {
        SetStatus(L"Save the assembly first, so the .cam file has somewhere to go.");
        return;
    }

    const fs::path target = camcore::CamPathForAssembly(current.path);
    const std::wstring fileName = FileName(target);

    // The assembly may have been saved somewhere new since this job was opened.
    std::error_code ignored;
    if (target != m_camPath && fs::exists(target, ignored))
    {
        if (AskYesNo(fileName + L" already exists.\n\nReplace it?") != IDYES)
        {
            SetStatus(L"Not saved. " + fileName + L" was left as it is.");
            return;
        }
    }

    m_job.assembly = current.identity;

    const camcore::SaveResult saved = camcore::SaveJob(m_job, target);
    if (!saved.ok)
    {
        const std::wstring message = L"Could not save " + fileName + L". " + FromUtf8(saved.message);
        SetStatus(message);
        ShowError(message);
        return;
    }

    m_camPath = target;
    m_unsavedChanges = false;
    SetStatus(L"Saved " + fileName + L".");
}

/////////////////////////////////////////////////////////////////////////////
// The stock

void CCamTabCommand::MakeStock(camcore::StockKind kind)
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    if (!CanEditSequence() || FocusOpenDialog() || HasStock())
        return;
    OpenStockDialog(kind);
}

void CCamTabCommand::EditStock()
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    if (!CanEditSequence() || FocusOpenDialog() || !HasStock())
        return;
    OpenStockDialog(m_job.stock.kind);
}

void CCamTabCommand::DeleteStock()
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    if (!CanEditSequence() || FocusOpenDialog() || !HasStock())
        return;
    const std::wstring question = L"Delete the " + FromUtf8(camcore::StockKindTitle(m_job.stock.kind)) + L"?";
    if (AfxMessageBox(question.c_str(), MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    ApplyStock(camcore::Stock());
}

void CCamTabCommand::OpenStockDialog(camcore::StockKind kind)
{
    CWnd* pParent = CWnd::FromHandle(theApp.m_hAlibreWnd);
    switch (kind)
    {
    case camcore::StockKind::Box:
    {
        ReadModelSize();
        CBoxStockDlg dialog(m_job.stock, m_haveBounds, m_boundsLower, m_boundsUpper, pParent);
        if (dialog.DoModal() == IDOK)
            ApplyStock(dialog.GetStock());
        break;
    }

    case camcore::StockKind::Imported:
    {
        CImportStockDlg dialog(m_job.stock, pParent);
        if (dialog.DoModal() == IDOK)
            ApplyStock(dialog.GetStock());
        break;
    }

    case camcore::StockKind::PartBox:
    {
        // Modeless, so parts can be clicked in the view.
        StartView(L"part box stock");
        m_pPartBoxDlg = new CPartBoxStockDlg(this, m_job.stock);
        if (!m_pPartBoxDlg->CreateModeless(pParent))
        {
            Log(L"[stock] could not create the Part Box Stock dialog");
            delete m_pPartBoxDlg;
            m_pPartBoxDlg = nullptr;
            return;
        }
        m_pPartBoxDlg->ShowWindow(SW_SHOW);
        RebuildScene();
        break;
    }

    default:
        break;
    }
}

void CCamTabCommand::ApplyStock(const camcore::Stock& stock)
{
    if (!CanEditSequence())
        return;

    m_job.stock = stock;
    m_stockShapeValid = false;
    MarkJobChanged();
    if (m_pWnd != nullptr)
        m_pWnd->ShowStock(m_job.stock);

    // Say now if it can't be read, rather than only drawing nothing.
    camcore::StockBox box;
    std::wstring problem;
    std::vector<camcore::LineSegment> edges;
    if (HasStock() && !ReadStockShape(m_job.stock, edges, box, problem))
        SetStatus(problem);
    RebuildScene();
}

void CCamTabCommand::SetStockShown(bool shown)
{
    m_scene.stockShown = shown;
    RebuildScene();
}

void CCamTabCommand::BeginPartSelect()
{
    SelectionFilter wanted;
    wanted.solid = ADSelectionFilterOption_AD_PARTS;
    if (!PrepareSelection(wanted) && m_pPartBoxDlg != nullptr)
    {
        m_pPartBoxDlg->StopSelectingParts();
        m_pPartBoxDlg->ShowResult(L"Selecting couldn't start. Click the CAM tab, then click Select Parts again.");
    }
}

void CCamTabCommand::EndPartSelect()
{
    RestoreSelectionFilter();
}

void CCamTabCommand::OnStockPreviewChanged()
{
    RebuildScene();
}

void CCamTabCommand::OnPartBoxDialogDone(CPartBoxStockDlg* pDialog, bool apply)
{
    if (pDialog != m_pPartBoxDlg)
        return;

    m_pPartBoxDlg = nullptr;
    RestoreSelectionFilter();
    if (apply)
        ApplyStock(pDialog->GetStock());
    else
        RebuildScene();
}

void CCamTabCommand::ClosePartBoxDialog(bool callAlibre)
{
    if (m_pPartBoxDlg == nullptr)
        return;

    CPartBoxStockDlg* pDialog = m_pPartBoxDlg;
    m_pPartBoxDlg = nullptr;
    pDialog->DetachFromTab();
    pDialog->DestroyWindow();

    if (callAlibre)
        RestoreSelectionFilter();
    else
        m_filterChanged = false;
}

bool CCamTabCommand::ReadStockShape(const camcore::Stock& stock, std::vector<camcore::LineSegment>& edges,
                                    camcore::StockBox& box, std::wstring& problem,
                                    std::vector<camcore::Vec3>* pPoints)
{
    edges.clear();
    problem.clear();
    if (pPoints != nullptr)
        pPoints->clear();
    switch (stock.kind)
    {
    case camcore::StockKind::Box:
        box = camcore::BoxStockBox(stock);
        edges = camcore::BoxEdges(box);
        if (pPoints != nullptr)
            *pPoints = camcore::BoxCorners(box);
        return true;

    case camcore::StockKind::PartBox:
    {
        camcore::Vec3 lower, upper;
        bool read = false;
        try
        {
            read = ReadPartsBounds(GetSession(), stock.parts, lower, upper, problem);
        }
        catch (const _com_error& error)
        {
            Log(L"[stock] could not find the session: hr=0x%08X", static_cast<unsigned>(error.Error()));
            problem = L"The parts' size couldn't be read.";
        }
        if (!read)
            return false;
        box = camcore::PartBoxStockBox(stock, lower, upper);
        edges = camcore::BoxEdges(box);
        if (pPoints != nullptr)
            *pPoints = camcore::BoxCorners(box);
        return true;
    }

    case camcore::StockKind::Imported:
    {
        // The part's facets are read once, then kept until the file changes.
        if (m_stockFacetsPath != stock.partPath)
        {
            m_stockFacetsPath.clear();
            m_stockPartEdges.clear();
            m_stockPartPoints.clear();
            std::vector<camcore::Vec3> triangles;
            if (!ReadPartFacets(FromUtf8(stock.partPath), triangles, problem))
                return false;
            m_stockPartEdges = camcore::MeshOutlineEdges(triangles, 20.0, 1e-4);
            m_stockPartPoints = triangles;
            m_stockFacetsPath = stock.partPath;
            Log(L"[stock] %zu outline edges", m_stockPartEdges.size());
        }

        const camcore::Frame frame = camcore::ImportedStockFrame(stock);
        for (const camcore::LineSegment& edge : m_stockPartEdges)
            edges.push_back({ camcore::FramePointToWorld(frame, edge.from), camcore::FramePointToWorld(frame, edge.to) });
        std::vector<camcore::Vec3> points;
        points.reserve(m_stockPartPoints.size());
        for (const camcore::Vec3& point : m_stockPartPoints)
            points.push_back(camcore::FramePointToWorld(frame, point));
        if (!camcore::PointsBox(points, box))
        {
            problem = L"The stock part has no solid to read.";
            return false;
        }
        if (pPoints != nullptr)
            *pPoints = std::move(points);
        return true;
    }

    default:
        problem = L"There's no stock.";
        return false;
    }
}

/////////////////////////////////////////////////////////////////////////////
// The operation sequence

bool CCamTabCommand::CanEditSequence() const
{
    return !m_saveBlocked;
}

bool CCamTabCommand::IsDialogOpen() const
{
    return m_pOriginDlg != nullptr || m_pOperationDlg != nullptr || m_pPartBoxDlg != nullptr;
}

camcore::SequenceStep* CCamTabCommand::FindStep(const StepRef& ref)
{
    if (ref.step >= m_job.sequence.size())
        return nullptr;

    camcore::SequenceStep& step = m_job.sequence[ref.step];
    if (!ref.IsOperation())
        return &step;
    return (ref.child < step.children.size()) ? &step.children[ref.child] : nullptr;
}

const camcore::SequenceStep* CCamTabCommand::FindStep(const StepRef& ref) const
{
    return const_cast<CCamTabCommand*>(this)->FindStep(ref);
}

void CCamTabCommand::ShowSequence()
{
    m_sequenceStatus = m_job.sequence.empty() ? L"Right-click Operations to add a Set Origin." : L"";
    ShowStatus();

    // The job may be a different one, so its stock is read again.
    m_stockShapeValid = false;
    if (m_pWnd != nullptr)
    {
        m_pWnd->ShowSequence(m_job, m_selected);
        m_pWnd->ShowStock(m_job.stock);
    }
}

bool CCamTabCommand::FocusOpenDialog()
{
    if (m_pOriginDlg != nullptr)
        m_pOriginDlg->SetForegroundWindow();
    else if (m_pOperationDlg != nullptr)
        m_pOperationDlg->SetForegroundWindow();
    else if (m_pPartBoxDlg != nullptr)
        m_pPartBoxDlg->SetForegroundWindow();
    else
        return false;
    return true;
}

void CCamTabCommand::AddSetOrigin()
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    if (!CanEditSequence() || FocusOpenDialog())
        return;

    camcore::SequenceStep step;
    step.type = camcore::StepType::SetOrigin;
    step.id = camcore::NewStableId();
    step.name = NewStepName(m_job, "Set Origin");
    OpenOriginDialog(step, kNewStep);
}

bool CCamTabCommand::CanAddFolder() const
{
    // The sequence must start with a Set Origin, and folders can't hold one.
    return CanEditSequence() && !m_job.sequence.empty();
}

bool CCamTabCommand::IsFolder(const StepRef& ref) const
{
    const camcore::SequenceStep* pStep = FindStep(ref);
    return pStep != nullptr && pStep->type == camcore::StepType::Group;
}

bool CCamTabCommand::IsOperation(const StepRef& ref) const
{
    const camcore::SequenceStep* pStep = FindStep(ref);
    return pStep != nullptr && camcore::IsOperation(pStep->type);
}

bool CCamTabCommand::CanRename(const StepRef& ref) const
{
    // Set Origin steps are renamed in their dialog.
    return CanEditSequence() && (IsFolder(ref) || IsOperation(ref));
}

bool CCamTabCommand::CanDelete(const StepRef& ref) const
{
    // The first step is the job's starting origin, so it can only be edited.
    return CanEditSequence() && FindStep(ref) != nullptr && (ref.IsOperation() || ref.step > 0);
}

void CCamTabCommand::AddFolder()
{
    if (!CanAddFolder())
        return;

    camcore::SequenceStep folder;
    folder.type = camcore::StepType::Group;
    folder.id = camcore::NewStableId();
    folder.name = NewStepName(m_job, "Folder");
    m_job.sequence.push_back(folder);
    Log(L"[sequence] added %s", FromUtf8(folder.name).c_str());

    m_selected = StepRef{ m_job.sequence.size() - 1 };
    MarkJobChanged();
    ShowSequence();
    RebuildScene();

    // There's no dialog, so the new folder's name is ready to type over.
    if (m_pWnd != nullptr)
        m_pWnd->StartRename(m_selected);
}

void CCamTabCommand::AddOperation(size_t folder, camcore::StepType type)
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    if (!camcore::IsOperation(type) || !CanEditSequence() || !IsFolder(StepRef{ folder }) || FocusOpenDialog())
        return;

    camcore::SequenceStep operation;
    operation.type = type;
    operation.id = camcore::NewStableId();
    operation.name = NewOperationName(m_job, type == camcore::StepType::Facing      ? "Facing"
                                             : type == camcore::StepType::Profiling ? "Profiling"
                                             : type == camcore::StepType::Pocketing ? "Pocketing"
                                             : type == camcore::StepType::HolePocketing ? "Hole Pocketing"
                                                                                    : "Drill");

    // A new operation starts with the tool selected in the tree.
    operation.toolId = m_job.selectedToolId;
    OpenOperationDialog(operation, StepRef{ folder, kNewStep });
}

bool CCamTabCommand::RenameStep(const StepRef& ref, const std::wstring& name)
{
    camcore::SequenceStep* pStep = FindStep(ref);
    if (!CanRename(ref) || pStep == nullptr)
        return false;

    CString trimmed(name.c_str());
    trimmed.Trim();
    if (trimmed.IsEmpty())
        return false;

    const std::string newName = ToUtf8(static_cast<LPCWSTR>(trimmed));
    if (newName == pStep->name)
        return true;

    Log(L"[sequence] renamed %s to %s", FromUtf8(pStep->name).c_str(), static_cast<LPCWSTR>(trimmed));
    pStep->name = newName;
    MarkJobChanged();
    return true;
}

void CCamTabCommand::EditStep(const StepRef& ref)
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    const camcore::SequenceStep* pStep = FindStep(ref);
    if (!CanEditSequence() || pStep == nullptr || FocusOpenDialog())
        return;

    if (pStep->type == camcore::StepType::SetOrigin)
        OpenOriginDialog(*pStep, ref.step);
    else if (camcore::IsOperation(pStep->type))
        OpenOperationDialog(*pStep, ref);
}

void CCamTabCommand::DeleteStep(const StepRef& ref)
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    const camcore::SequenceStep* pStep = FindStep(ref);
    if (!CanDelete(ref) || pStep == nullptr)
        return;
    if (IsDialogOpen())
    {
        ShowError(L"Close the open dialog first.");
        return;
    }

    const std::wstring name = FromUtf8(pStep->name);
    if (pStep->type == camcore::StepType::Group)
    {
        // An empty folder goes without asking.
        const size_t count = pStep->children.size();
        if (count > 0 &&
            AskYesNo(L"Delete " + name + L" and the " + std::to_wstring(count) +
                     (count == 1 ? L" operation" : L" operations") + L" in it?") != IDYES)
            return;
    }
    else if (AskYesNo(L"Delete " + name + L"?") != IDYES)
    {
        return;
    }

    Log(L"[sequence] deleted %s", name.c_str());
    if (ref.IsOperation())
    {
        std::vector<camcore::SequenceStep>& children = m_job.sequence[ref.step].children;
        children.erase(children.begin() + static_cast<ptrdiff_t>(ref.child));
        m_selected = StepRef{ ref.step };
    }
    else
    {
        m_job.sequence.erase(m_job.sequence.begin() + static_cast<ptrdiff_t>(ref.step));
        m_selected = StepRef{ (std::min)(ref.step, m_job.sequence.size() - 1) };
    }

    MarkJobChanged();
    ShowSequence();
    RebuildScene();
}

void CCamTabCommand::SelectStep(const StepRef& ref)
{
    if (FindStep(ref) == nullptr)
        return;

    // The origin, which toolpath is drawn bold, and which features are shown follow the selection.
    const bool changed = (ref.step != m_selected.step || ref.child != m_selected.child);
    m_selected = ref;
    if (changed)
        RebuildScene();
}

std::vector<camcore::PickedGeometry> CCamTabCommand::HighlightedFeatures() const
{
    // The open dialog's list, as it's being edited.
    if (m_pOperationDlg != nullptr)
        return m_pOperationDlg->GetFeatures();

    std::vector<camcore::PickedGeometry> features;
    if (m_selected.step != StepRef::kNone && m_selected.step < m_job.sequence.size())
    {
        const camcore::SequenceStep& step = m_job.sequence[m_selected.step];
        for (size_t c = 0; c < step.children.size(); ++c)
        {
            if (m_selected.IsOperation() && m_selected.child != c)
                continue;
            const std::vector<camcore::PickedGeometry>& own = step.children[c].features;
            features.insert(features.end(), own.begin(), own.end());
        }
    }
    return features;
}

void CCamTabCommand::UpdateHighlights()
{
    // Read again only when the features change.
    const std::vector<camcore::PickedGeometry> features = HighlightedFeatures();
    bool same = features.size() == m_highlighted.size();
    for (size_t i = 0; same && i < features.size(); ++i)
        same = camcore::SameFeature(features[i], m_highlighted[i]);
    if (same)
        return;

    m_highlighted = features;
    m_highlightOutlines.clear();
    IADSessionPtr session;
    try
    {
        session = GetSession();
    }
    catch (const _com_error& error)
    {
        Log(L"[features] could not find the session: hr=0x%08X", static_cast<unsigned>(error.Error()));
        return;
    }

    // A face shows its outer loop, and an edge itself. Points have no outline.
    for (const camcore::PickedGeometry& feature : features)
    {
        if (feature.kind == camcore::PickKind::DesignPoint)
            continue;
        std::vector<camcore::Path> outline;
        std::wstring problem;
        if (!ReadFeatureOutline(session, feature, outline, problem))
            continue;
        for (const camcore::Path& piece : outline)
            m_highlightOutlines.push_back(camcore::PathPoints(piece, false));
    }
}

bool CCamTabCommand::IsSuppressed(const StepRef& ref) const
{
    const camcore::SequenceStep* pStep = FindStep(ref);
    return pStep != nullptr && pStep->suppressed;
}

bool CCamTabCommand::CanSuppress(const StepRef& ref) const
{
    // An open dialog holds a copy of its step, which would undo the change.
    return CanEditSequence() && !IsDialogOpen() && FindStep(ref) != nullptr;
}

void CCamTabCommand::ToggleSuppressed(const StepRef& ref)
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    if (!CanSuppress(ref))
        return;
    camcore::SequenceStep* pStep = FindStep(ref);
    pStep->suppressed = !pStep->suppressed;
    Log(L"[sequence] %s %s", pStep->suppressed ? L"suppressed" : L"unsuppressed", FromUtf8(pStep->name).c_str());

    // A Set Origin changes which origin the steps below it use.
    m_selected = ref;
    MarkJobChanged();
    ShowSequence();
    RebuildScene();
}

void CCamTabCommand::DuplicateStep(const StepRef& ref)
{
    // An open dialog holds a step's position, as for dragging.
    const camcore::SequenceStep* pStep = FindStep(ref);
    if (!CanDrag(ref) || pStep == nullptr)
        return;

    const std::wstring name = FromUtf8(pStep->name);
    size_t newIndex = 0;
    if (!camcore::DuplicateStep(m_job, ref.step, ref.IsOperation() ? ref.child : camcore::kNoStepIndex, newIndex))
        return;

    Log(L"[sequence] duplicated %s", name.c_str());
    m_selected = ref.IsOperation() ? StepRef{ ref.step, newIndex } : StepRef{ newIndex };
    MarkJobChanged();
    ShowSequence();
    RebuildScene();
}

bool CCamTabCommand::CanGenerate() const
{
    // An open dialog may be changing an operation's settings.
    return CanEditSequence() && !IsDialogOpen();
}

void CCamTabCommand::Generate(const StepRef& scope)
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    if (!CanGenerate())
        return;

    const std::vector<StepRef> targets = OperationsIn(scope, false);
    if (targets.empty())
    {
        ShowError(L"There are no operations to generate. Suppressed operations are only generated on their own.");
        return;
    }

    // The clearance plane can depend on the parts' size, which may have changed.
    ReadModelSize();

    size_t generated = 0;
    std::wstring problems;
    for (const StepRef& target : targets)
    {
        camcore::SequenceStep& operation = m_job.sequence[target.step].children[target.child];
        std::string problem;
        if (GenerateOperation(target, problem))
        {
            ++generated;
        }
        else
        {
            operation.toolpath = camcore::Toolpath();
            problems += L"\n" + FromUtf8(operation.name) + L": " + FromUtf8(problem);
        }
        Log(L"[generate] %s: %s", FromUtf8(operation.name).c_str(),
            problem.empty() ? L"ok" : FromUtf8(problem).c_str());
    }

    MarkJobChanged();
    RebuildScene();

    const std::wstring summary = L"Generated " + std::to_wstring(generated) + L" of " +
                                 std::to_wstring(targets.size()) +
                                 (targets.size() == 1 ? L" operation." : L" operations.");
    m_sequenceStatus = summary;
    ShowStatus();
    if (!problems.empty())
        ::MessageBoxW(theApp.m_hAlibreWnd, (summary + L"\n" + problems).c_str(), L"AlibreCAM", MB_OK | MB_ICONWARNING);
}

std::vector<StepRef> CCamTabCommand::OperationsIn(const StepRef& scope, bool forPost) const
{
    // No step means every operation, a folder means its operations, and an operation itself.
    std::vector<StepRef> targets;
    for (size_t i = 0; i < m_job.sequence.size(); ++i)
    {
        if (scope.step != StepRef::kNone && scope.step != i)
            continue;

        // A suppressed folder's operations are only generated when it's the scope, and never posted.
        const bool folderSuppressed = m_job.sequence[i].suppressed;
        if (folderSuppressed && (forPost || scope.step == StepRef::kNone))
            continue;

        for (size_t c = 0; c < m_job.sequence[i].children.size(); ++c)
        {
            if (scope.IsOperation() && scope.child != c)
                continue;
            const bool own = scope.IsOperation();
            if (m_job.sequence[i].children[c].suppressed && (forPost || !own))
                continue;
            targets.push_back(StepRef{ i, c });
        }
    }
    return targets;
}

void CCamTabCommand::Post(const StepRef& scope)
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    if (!CanGenerate())
        return;

    const std::vector<StepRef> targets = OperationsIn(scope, true);
    if (targets.empty())
    {
        ShowError(L"There are no operations to post. Suppressed operations aren't posted.");
        return;
    }

    // Posting uses saved toolpaths, so every operation needs one.
    std::wstring missing;
    for (const StepRef& target : targets)
    {
        const camcore::SequenceStep& operation = m_job.sequence[target.step].children[target.child];
        if (operation.toolpath.moves.empty())
            missing += L"\n" + FromUtf8(operation.name);
    }
    if (!missing.empty())
    {
        ShowError(L"Generate these operations before posting:\n" + missing);
        return;
    }

    // The job's one work zero is its first Set Origin that isn't suppressed,
    // which every point is given from.
    const size_t workZeroIndex = camcore::WorkZeroIndex(m_job);
    if (workZeroIndex == camcore::kNoStepIndex)
    {
        ShowError(L"Every Set Origin is suppressed, so there's no work zero to post from.");
        return;
    }
    const camcore::FrameResult workZero = camcore::BuildFrame(m_job.sequence[workZeroIndex].origin);
    if (!workZero.ok)
    {
        ShowError(L"The first Set Origin that isn't suppressed is incomplete, so there's no work zero to post from.");
        return;
    }

    // The file goes beside the .cam file by default, named after the assembly and folder.
    const bool wholeJob = (scope.step == StepRef::kNone);
    std::wstring scopeText = wholeJob ? L"All operations" : FromUtf8(m_job.sequence[scope.step].name);
    std::wstring fileName = m_camPath.empty() ? L"Program" : m_camPath.stem().wstring();
    if (!wholeJob)
        fileName += L" - " + scopeText;
    fs::path folder = !m_lastPostPath.empty() ? m_lastPostPath.parent_path()
                      : !m_camPath.empty()    ? m_camPath.parent_path()
                                              : fs::path();
    // The post chosen last time, or the first scripted one.
    const std::vector<PostChoice> posts = FindPosts();
    size_t chosen = 0;
    for (size_t i = 0; i < posts.size(); ++i)
    {
        if (!m_lastPostName.empty() && posts[i].name == m_lastPostName)
            chosen = i;
    }
    const std::wstring extension = L"." + posts[chosen].extension;
    const fs::path suggested = folder.empty() ? fs::path(fileName + extension) : folder / (fileName + extension);

    CPostDlg dialog(L"Posting: " + scopeText, suggested, posts, chosen, m_pWnd);
    if (dialog.DoModal() != IDOK)
        return;
    const fs::path output = dialog.GetOutputPath();
    const PostChoice& post = posts[dialog.GetChosen()];
    m_lastPostPath = output;
    m_lastPostName = post.name;

    // Each operation with the origin it runs under and its tool. The tools are
    // kept here so the post can point at them.
    std::vector<camcore::Tool> tools;
    tools.reserve(targets.size());
    std::vector<camcore::PostOperation> entries;
    for (const StepRef& target : targets)
    {
        camcore::PostOperation entry;
        entry.operation = &m_job.sequence[target.step].children[target.child];

        const size_t active = camcore::ActiveOriginIndex(m_job, target.step);
        if (active != camcore::kNoStepIndex)
        {
            entry.origin = &m_job.sequence[active];
            const camcore::FrameResult frame = camcore::BuildFrame(entry.origin->origin);
            if (frame.ok)
                entry.frame = frame.frame;
        }

        if (const camcore::Tool* pTool = camcore::FindToolById(m_library, entry.operation->toolId))
        {
            tools.push_back(camcore::ToolInMillimetres(*pTool));
            entry.tool = &tools.back();
        }
        entries.push_back(entry);
    }

    // A post script, or the built-in APT CL data.
    std::string text;
    if (post.script.empty())
    {
        text = camcore::WriteAptClData(m_job.assembly.name, workZero.frame, entries);
    }
    else
    {
        std::string script;
        if (!camcore::ReadWholeFile(post.script, script))
        {
            ShowError(L"Could not read the post " + post.script.wstring() + L".");
            return;
        }
        const camcore::PostScriptRead read = camcore::ReadPostScript(script);
        if (!read.ok)
        {
            ShowError(L"The post " + post.script.filename().wstring() + L" has a mistake. " + FromUtf8(read.problem));
            return;
        }

        SYSTEMTIME now = {};
        GetLocalTime(&now);
        char date[16];
        std::snprintf(date, sizeof(date), "%04u-%02u-%02u", now.wYear, now.wMonth, now.wDay);
        const camcore::PostOutput posted =
            camcore::WriteScriptedPost(read.script, m_job.assembly.name, workZero.frame, entries, date);
        if (!posted.ok)
        {
            ShowError(FromUtf8(posted.problem));
            return;
        }
        text = posted.text;
    }
    std::string message;
    if (!camcore::WriteFileAtomically(output, text, message))
    {
        ShowError(L"Could not write " + output.filename().wstring() + L". " + FromUtf8(message));
        return;
    }

    Log(L"[post] %zu operations to %s with %s", entries.size(), output.c_str(), post.name.c_str());
    m_sequenceStatus = L"Posted " + std::to_wstring(entries.size()) +
                       (entries.size() == 1 ? L" operation to " : L" operations to ") + output.filename().wstring() + L".";
    ShowStatus();
}

bool CCamTabCommand::GenerateOperation(const StepRef& target, std::string& problem)
{
    camcore::SequenceStep& operation = m_job.sequence[target.step].children[target.child];
    if (operation.type != camcore::StepType::Drill && operation.type != camcore::StepType::Facing &&
        operation.type != camcore::StepType::Profiling && operation.type != camcore::StepType::Pocketing &&
        operation.type != camcore::StepType::HolePocketing)
    {
        problem = "Toolpaths for this operation aren't built yet.";
        return false;
    }

    const size_t active = camcore::ActiveOriginIndex(m_job, target.step);
    const camcore::FrameResult frame = (active != camcore::kNoStepIndex)
                                           ? camcore::BuildFrame(m_job.sequence[active].origin)
                                           : camcore::FrameResult();
    if (!frame.ok)
    {
        problem = "There's no complete Set Origin above its folder.";
        return false;
    }

    const camcore::Tool* pTool = camcore::FindToolById(m_library, operation.toolId);
    camcore::Tool tool;
    if (pTool != nullptr)
        tool = camcore::ToolInMillimetres(*pTool);

    // The parts' highest point in the origin's frame is the highest corner of their box.
    bool havePartMaxZ = false;
    double partMaxZ = 0.0;
    if (m_haveBounds)
    {
        havePartMaxZ = true;
        for (int i = 0; i < 8; ++i)
        {
            const camcore::Vec3 corner{ (i & 1) ? m_boundsUpper.x : m_boundsLower.x,
                                        (i & 2) ? m_boundsUpper.y : m_boundsLower.y,
                                        (i & 4) ? m_boundsUpper.z : m_boundsLower.z };
            const double height = camcore::WorldToFrame(frame.frame, corner).z;
            partMaxZ = (i == 0) ? height : (std::max)(partMaxZ, height);
        }
    }

    // The stock's highest point in the origin's frame. A stock that can't be read
    // stops Generate, as clearing it can't be checked.
    bool haveStockMaxZ = false;
    double stockMaxZ = 0.0;
    if (HasStock())
    {
        std::vector<camcore::LineSegment> edges;
        camcore::StockBox box;
        std::wstring stockProblem;
        std::vector<camcore::Vec3> points;
        if (!ReadStockShape(m_job.stock, edges, box, stockProblem, &points))
        {
            problem = ToUtf8(stockProblem);
            return false;
        }
        haveStockMaxZ = camcore::HighestAlongZ(frame.frame, points, stockMaxZ);
    }

    // Each feature's outline, read again from the model, in the origin's frame
    // with arcs' axes too, in the Features list's order.
    std::vector<std::vector<camcore::Path>> outlines;
    auto readOutlines = [&]() {
        for (const camcore::PickedGeometry& feature : operation.features)
        {
            std::vector<camcore::Path> outline;
            std::wstring outlineProblem;
            if (!ReadFeatureOutline(GetSession(), feature, outline, outlineProblem))
            {
                problem = ToUtf8(outlineProblem);
                return false;
            }
            for (camcore::Path& piece : outline)
            {
                for (camcore::PathVertex& vertex : piece)
                {
                    if (vertex.arc)
                    {
                        vertex.axis = camcore::WorldToFrame(frame.frame, vertex.centre + vertex.axis) -
                                      camcore::WorldToFrame(frame.frame, vertex.centre);
                        vertex.centre = camcore::WorldToFrame(frame.frame, vertex.centre);
                    }
                    vertex.point = camcore::WorldToFrame(frame.frame, vertex.point);
                }
            }
            outlines.push_back(std::move(outline));
        }
        return true;
    };

    // Facing, Profiling and Pocketing sort their features into areas: each face
    // on its own, and edges grouped where they meet, all flat at the highest
    // point, which At Top and At Bottom are measured from.
    camcore::AreaGeometry areas;
    auto gatherAreas = [&](const char* name) {
        const camcore::AreaFeatureCheck check = camcore::CheckAreaFeatures(operation.features, name);
        if (!check.ok)
        {
            problem = check.problem;
            return false;
        }
        if (!readOutlines())
            return false;
        std::vector<bool> isFace;
        for (const camcore::PickedGeometry& feature : operation.features)
            isFace.push_back(feature.kind == camcore::PickKind::PlanarFace);
        areas = camcore::GatherAreas(outlines, isFace);
        if (!areas.ok)
        {
            problem = areas.problem;
            return false;
        }
        Log(L"[generate] %zu areas, at Z %.3f", areas.areas.size(), areas.level);
        return true;
    };
    auto areaName = [&](size_t n, const char* kind) {
        return (areas.areas.size() > 1) ? std::string(kind) + " " + std::to_string(n + 1) + ": " : std::string();
    };

    // Profiling joins each area's pieces end to end into a profile, then follows it.
    if (operation.type == camcore::StepType::Profiling)
    {
        if (!gatherAreas("Profiling"))
            return false;

        camcore::ProfileToolpathInput profileInput;
        profileInput.operation = &operation;
        profileInput.tool = (pTool != nullptr) ? &tool : nullptr;
        profileInput.havePartMaxZ = havePartMaxZ;
        profileInput.partMaxZ = partMaxZ;
        profileInput.haveStockMaxZ = haveStockMaxZ;
        profileInput.stockMaxZ = stockMaxZ;
        for (size_t n = 0; n < areas.areas.size(); ++n)
        {
            camcore::ProfileChain chain = camcore::ChainProfile(areas.areas[n], areas.level);
            if (!chain.ok)
            {
                problem = areaName(n, "Profile") + chain.problem;
                return false;
            }
            profileInput.profiles.push_back(std::move(chain));
        }

        const camcore::ToolpathResult profileResult = camcore::GenerateProfileToolpath(profileInput);
        if (!profileResult.ok)
        {
            problem = profileResult.problem;
            return false;
        }
        operation.toolpath = profileResult.toolpath;
        return true;
    }

    // Pocketing does the same, each area a closed pocket, then clears inside each.
    if (operation.type == camcore::StepType::Pocketing)
    {
        if (!gatherAreas("Pocketing"))
            return false;

        camcore::PocketToolpathInput pocketInput;
        pocketInput.operation = &operation;
        pocketInput.tool = (pTool != nullptr) ? &tool : nullptr;
        pocketInput.havePartMaxZ = havePartMaxZ;
        pocketInput.partMaxZ = partMaxZ;
        pocketInput.haveStockMaxZ = haveStockMaxZ;
        pocketInput.stockMaxZ = stockMaxZ;
        for (size_t n = 0; n < areas.areas.size(); ++n)
        {
            camcore::ProfileChain chain = camcore::ChainProfile(areas.areas[n], areas.level);
            if (!chain.ok)
            {
                problem = areaName(n, "Pocket") + chain.problem;
                return false;
            }
            if (!chain.closed)
            {
                problem = areaName(n, "Pocket") + "the edges must join into a closed outline.";
                return false;
            }
            pocketInput.outlines.push_back(std::move(chain));
        }

        const camcore::ToolpathResult pocketResult = camcore::GeneratePocketToolpath(pocketInput);
        if (!pocketResult.ok)
        {
            problem = pocketResult.problem;
            return false;
        }
        operation.toolpath = pocketResult.toolpath;
        return true;
    }

    // Facing closes each area's pieces into a boundary, bridging gaps, then faces inside each.
    if (operation.type == camcore::StepType::Facing)
    {
        operation.boundaries.clear();
        if (!gatherAreas("Facing"))
            return false;

        for (size_t n = 0; n < areas.areas.size(); ++n)
        {
            camcore::Path boundary = camcore::ClosePath(camcore::AreaPieces(areas.areas[n]), areas.level);
            if (boundary.empty())
            {
                operation.boundaries.clear();
                problem = areaName(n, "Area") + "the features don't enclose an area.";
                return false;
            }
            operation.boundaries.push_back(std::move(boundary));
        }

        camcore::FacingToolpathInput facingInput;
        facingInput.operation = &operation;
        facingInput.tool = (pTool != nullptr) ? &tool : nullptr;
        facingInput.havePartMaxZ = havePartMaxZ;
        facingInput.partMaxZ = partMaxZ;
        facingInput.haveStockMaxZ = haveStockMaxZ;
        facingInput.stockMaxZ = stockMaxZ;
        const camcore::ToolpathResult facingResult = camcore::GenerateFacingToolpath(facingInput);
        if (!facingResult.ok)
        {
            problem = facingResult.problem;
            return false;
        }
        operation.toolpath = facingResult.toolpath;
        return true;
    }

    // Hole pocketing, like drilling, works from each feature's centre in the origin's frame.
    if (operation.type == camcore::StepType::HolePocketing)
    {
        camcore::HolePocketToolpathInput holeInput;
        holeInput.operation = &operation;
        holeInput.frame = frame.frame;
        holeInput.tool = (pTool != nullptr) ? &tool : nullptr;
        holeInput.havePartMaxZ = havePartMaxZ;
        holeInput.partMaxZ = partMaxZ;
        holeInput.haveStockMaxZ = haveStockMaxZ;
        holeInput.stockMaxZ = stockMaxZ;
        const camcore::ToolpathResult holeResult = camcore::GenerateHolePocketToolpath(holeInput);
        if (!holeResult.ok)
        {
            problem = holeResult.problem;
            return false;
        }
        operation.toolpath = holeResult.toolpath;
        return true;
    }

    camcore::DrillToolpathInput input;
    input.operation = &operation;
    input.frame = frame.frame;
    input.tool = (pTool != nullptr) ? &tool : nullptr;
    input.havePartMaxZ = havePartMaxZ;
    input.partMaxZ = partMaxZ;
    input.haveStockMaxZ = haveStockMaxZ;
    input.stockMaxZ = stockMaxZ;

    const camcore::ToolpathResult result = camcore::GenerateDrillToolpath(input);
    if (!result.ok)
    {
        problem = result.problem;
        return false;
    }

    operation.toolpath = result.toolpath;
    return true;
}

bool CCamTabCommand::CanDrag(const StepRef& ref) const
{
    // An open dialog holds a step's position, so nothing moves while one is open.
    return CanEditSequence() && !IsDialogOpen() && FindStep(ref) != nullptr;
}

bool CCamTabCommand::CanMoveStep(const StepRef& from, const StepRef& target, DropSide side) const
{
    if (!CanDrag(from) || FindStep(target) == nullptr)
        return false;

    camcore::Job trial = m_job;
    StepRef moved;
    return ApplyMove(trial, from, target, side, moved);
}

void CCamTabCommand::MoveStep(const StepRef& from, const StepRef& target, DropSide side)
{
    if (!CanDrag(from) || FindStep(target) == nullptr)
        return;

    const std::wstring name = FromUtf8(FindStep(from)->name);
    StepRef moved;
    if (!ApplyMove(m_job, from, target, side, moved))
        return;

    Log(L"[sequence] moved %s", name.c_str());
    m_selected = moved;
    MarkJobChanged();
    ShowSequence();
    RebuildScene();
}

void CCamTabCommand::OpenOriginDialog(const camcore::SequenceStep& step, size_t editIndex)
{
    // The dialog draws a preview from the moment it's created, so set these first.
    m_editIndex = editIndex;
    m_pOriginDlg = new COriginDlg(this, step);
    if (!m_pOriginDlg->CreateModeless(CWnd::FromHandle(theApp.m_hAlibreWnd)))
    {
        Log(L"[origin] could not create the Set Origin dialog");
        delete m_pOriginDlg;
        m_pOriginDlg = nullptr;
        RebuildScene();
        return;
    }
    m_pOriginDlg->ShowWindow(SW_SHOW);
    Log(L"[origin] editing %s", editIndex == kNewStep ? L"a new step" : FromUtf8(step.name).c_str());
}

void CCamTabCommand::CloseOriginDialog(bool callAlibre)
{
    if (m_pOriginDlg == nullptr)
        return;

    COriginDlg* pDialog = m_pOriginDlg;
    m_pOriginDlg = nullptr;
    pDialog->DetachFromTab();
    pDialog->DestroyWindow();

    if (callAlibre)
        RestoreSelectionFilter();
    else
        m_filterChanged = false;
}

void CCamTabCommand::OnOriginDialogDone(COriginDlg* pDialog, bool apply)
{
    if (pDialog != m_pOriginDlg)
        return;

    m_pOriginDlg = nullptr;
    RestoreSelectionFilter();

    if (apply && CanEditSequence())
    {
        const camcore::SequenceStep& step = pDialog->GetStep();
        if (m_editIndex == kNewStep)
        {
            m_job.sequence.push_back(step);
            m_selected = StepRef{ m_job.sequence.size() - 1 };
        }
        else if (m_editIndex < m_job.sequence.size())
        {
            m_job.sequence[m_editIndex] = step;
            m_selected = StepRef{ m_editIndex };
        }
        Log(L"[origin] applied %s", FromUtf8(step.name).c_str());
        MarkJobChanged();
        ShowSequence();
    }

    RebuildScene();
}

void CCamTabCommand::OnOriginPreviewChanged()
{
    RebuildScene();
}

void CCamTabCommand::OpenOperationDialog(const camcore::SequenceStep& step, const StepRef& target)
{
    m_operationTarget = target;
    m_pOperationDlg = new COperationDlg(this, step);
    if (!m_pOperationDlg->CreateModeless(CWnd::FromHandle(theApp.m_hAlibreWnd)))
    {
        Log(L"[operation] could not create the operation dialog");
        delete m_pOperationDlg;
        m_pOperationDlg = nullptr;
        return;
    }
    m_pOperationDlg->ShowWindow(SW_SHOW);
    Log(L"[operation] editing %s", target.child == kNewStep ? L"a new operation" : FromUtf8(step.name).c_str());

    // Its own features are outlined from the start, in place of the tree's.
    RebuildScene();
}

void CCamTabCommand::CloseOperationDialog()
{
    if (m_pOperationDlg == nullptr)
        return;

    COperationDlg* pDialog = m_pOperationDlg;
    m_pOperationDlg = nullptr;
    pDialog->DetachFromTab();
    pDialog->DestroyWindow();
}

void CCamTabCommand::OnOperationDialogDone(COperationDlg* pDialog, bool apply)
{
    if (pDialog != m_pOperationDlg)
        return;

    m_pOperationDlg = nullptr;
    RestoreSelectionFilter();

    // Rows chosen in the Features list were selected in the view, so that goes too.
    if (m_showingFeatures)
    {
        m_showingFeatures = false;
        ClearPickedSelection();
    }

    if (!apply || !ApplyOperationStep(pDialog->GetStep()))
    {
        RebuildScene();     // the tree's selection is outlined again
        return;
    }

    Log(L"[operation] applied %s", FromUtf8(pDialog->GetStep().name).c_str());
    MarkJobChanged();
    ShowSequence();
    RebuildScene();
}

bool CCamTabCommand::ApplyOperationStep(camcore::SequenceStep step)
{
    if (!CanEditSequence() || !IsFolder(StepRef{ m_operationTarget.step }))
        return false;

    std::vector<camcore::SequenceStep>& children = m_job.sequence[m_operationTarget.step].children;
    if (m_operationTarget.child == kNewStep)
    {
        // A new operation goes in the first time, and the dialog edits it in place after that.
        step.toolpath = camcore::Toolpath();
        step.boundaries.clear();
        children.push_back(step);
        m_operationTarget.child = children.size() - 1;
    }
    else if (m_operationTarget.child < children.size())
    {
        // The toolpath still fits if the settings haven't changed, as after the
        // dialog's own Generate. Otherwise it's discarded.
        const camcore::SequenceStep& current = children[m_operationTarget.child];
        if (camcore::SameSettings(current, step))
        {
            step.toolpath = current.toolpath;
            step.boundaries = current.boundaries;
        }
        else
        {
            step.toolpath = camcore::Toolpath();
            step.boundaries.clear();
        }
        children[m_operationTarget.child] = step;
    }
    else
    {
        return false;
    }

    m_selected = m_operationTarget;
    return true;
}

void CCamTabCommand::OnOperationDialogGenerate(COperationDlg* pDialog)
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    if (pDialog != m_pOperationDlg)
        return;
    if (!ApplyOperationStep(pDialog->GetStep()))
    {
        ::MessageBoxW(pDialog->GetSafeHwnd(), L"The operation can't be changed now.", L"AlibreCAM",
                      MB_OK | MB_ICONWARNING);
        return;
    }

    // The clearance plane can depend on the parts' size, which may have changed.
    ReadModelSize();

    std::string problem;
    const bool ok = GenerateOperation(m_operationTarget, problem);
    camcore::SequenceStep& operation = m_job.sequence[m_operationTarget.step].children[m_operationTarget.child];
    if (!ok)
        operation.toolpath = camcore::Toolpath();
    const std::wstring name = FromUtf8(operation.name);
    Log(L"[generate] %s from its dialog: %s", name.c_str(), ok ? L"ok" : FromUtf8(problem).c_str());

    MarkJobChanged();
    ShowSequence();
    RebuildScene();
    m_sequenceStatus = ok ? L"Generated " + name + L"." : L"Couldn't generate " + name + L".";
    ShowStatus();
    if (!ok)
        ::MessageBoxW(pDialog->GetSafeHwnd(), (name + L": " + FromUtf8(problem)).c_str(), L"AlibreCAM",
                      MB_OK | MB_ICONWARNING);
}

/////////////////////////////////////////////////////////////////////////////
// The view command and picking

bool CCamTabCommand::StartView(const wchar_t* reason)
{
    if (m_pView != nullptr)
        return true;
    if (m_site == nullptr)
    {
        Log(L"[tab] can't start the view, no command site (%s)", reason);
        return false;
    }

    ReadModelSize();
    RebuildScene();

    CCamViewCommand* pView = new CCamViewCommand(this);
    pView->AddRef();
    m_pView = pView;

    const HRESULT hr = m_site->InvokeNewCommand(pView);
    Log(L"[tab] InvokeNewCommand (%s) hr=0x%08X", reason, static_cast<unsigned>(hr));

    if (FAILED(hr))
    {
        m_pView = nullptr;
        pView->Release();
        return false;
    }

    if (m_pView != nullptr)
        m_pView->Refresh();
    return m_pView != nullptr;
}

void CCamTabCommand::StopView(const wchar_t* reason)
{
    if (m_pView == nullptr)
        return;

    Log(L"[tab] stopping the view (%s)", reason);

    // Keep the command alive across Terminate, which calls OnViewTerminated.
    CCamViewCommand* pView = m_pView;
    pView->AddRef();
    pView->Refresh();
    if (pView->Site() != nullptr)
    {
        const HRESULT hr = pView->Site()->Terminate();
        Log(L"[tab] view Terminate hr=0x%08X", static_cast<unsigned>(hr));
    }
    pView->Release();
}

void CCamTabCommand::OnViewTerminated(CCamViewCommand* pView)
{
    if (m_pView != pView)
        return;

    m_pView = nullptr;
    pView->Release();

    // A native command, or leaving the tab, ends picking too.
    if (m_pPartBoxDlg != nullptr && m_pPartBoxDlg->IsSelectingParts())
    {
        m_pPartBoxDlg->StopSelectingParts();
        m_pPartBoxDlg->ShowResult(L"Selecting stopped because another command started. Click Select Parts to "
                                  L"carry on.");
        RestoreSelectionFilter();
    }
    if (m_pOriginDlg != nullptr && m_pOriginDlg->GetPickSlot() != PickSlot::None)
    {
        m_pOriginDlg->StopPicking();
        m_pOriginDlg->ShowPickProblem(L"Picking stopped because another command started. Click Pick to try again.");
        RestoreSelectionFilter();
    }
    if (m_pOperationDlg != nullptr && m_pOperationDlg->IsSelectingFeatures())
    {
        m_pOperationDlg->StopSelectingFeatures();
        m_pOperationDlg->ShowFeatureNote(L"Selecting stopped because another command started. Click Select "
                                         L"Features to carry on.");
        RestoreSelectionFilter();
    }
    if (m_pOperationDlg != nullptr && m_pOperationDlg->IsLocating())
    {
        m_pOperationDlg->StopLocating();
        m_pOperationDlg->ShowLocateProblem(L"Locate stopped because another command started. Click Locate to try again.");
        RestoreSelectionFilter();
    }
}

void CCamTabCommand::OnTabVisibilityChanged(bool visible)
{
    Log(L"[focus] tab %s", visible ? L"gained focus" : L"lost focus");
    if (visible)
        StartView(L"tab gained focus");
    else
        StopView(L"tab lost focus");
}

void CCamTabCommand::RefreshView()
{
    if (m_pView != nullptr)
    {
        ReadModelSize();
        RebuildScene();
    }
    else
    {
        StartView(L"refresh button");
    }
}

void CCamTabCommand::SetForeground(bool foreground)
{
    m_scene.foreground = foreground;
    RebuildScene();
}

void CCamTabCommand::SetToolpathsShown(bool shown)
{
    m_scene.toolpathsShown = shown;
    RebuildScene();
}

void CCamTabCommand::ReadModelSize()
{
    try
    {
        IADSessionPtr session = GetSession();
        IADDesignSessionPtr design;
        session->QueryInterface(__uuidof(IADDesignSession), reinterpret_cast<void**>(&design));
        m_scene.millimetresPerUnit = MillimetresPerModelUnit(design);
        m_haveBounds = ReadAssemblyBounds(session, m_boundsLower, m_boundsUpper);

        // Parts may have moved, which moves a part box stock.
        m_stockShapeValid = false;
    }
    catch (const _com_error& error)
    {
        Log(L"[tab] could not read the assembly's size: hr=0x%08X", static_cast<unsigned>(error.Error()));
        m_haveBounds = false;
    }
}

void CCamTabCommand::RebuildScene()
{
    m_scene.hasOrigin = false;
    m_scene.showPlane = !IsPicking();

    // While the dialog is open it shows its own step. Otherwise the selected
    // step shows the origin it runs under.
    const camcore::OriginDefinition* pDefinition = nullptr;
    if (m_pOriginDlg != nullptr)
    {
        pDefinition = &m_pOriginDlg->GetStep().origin;
    }
    else
    {
        const size_t active = camcore::ActiveOriginIndex(m_job, m_selected.step);
        if (active != camcore::kNoStepIndex)
            pDefinition = &m_job.sequence[active].origin;
    }

    if (pDefinition != nullptr)
    {
        const camcore::FrameResult result = camcore::BuildFrame(*pDefinition);
        if (result.ok)
        {
            const camcore::Vec3 lower = m_haveBounds ? m_boundsLower : result.frame.origin;
            const camcore::Vec3 upper = m_haveBounds ? m_boundsUpper : result.frame.origin;
            m_scene.origin = result.frame;
            m_scene.plane = camcore::PlaneCoveringBox(result.frame, lower, upper, kOriginPlaneMargin);
            m_scene.hasOrigin = true;
        }
    }

    // The stock: the Part Box dialog's while it's open, otherwise the job's.
    m_scene.showStock = !IsPicking();
    if (m_pPartBoxDlg != nullptr)
    {
        camcore::StockBox box;
        std::wstring problem;
        std::vector<camcore::LineSegment> edges;
        if (ReadStockShape(m_pPartBoxDlg->GetStock(), edges, box, problem))
        {
            wchar_t text[160];
            swprintf_s(text, L"The stock is %.2f x %.2f x %.2f mm.", box.upper.x - box.lower.x,
                       box.upper.y - box.lower.y, box.upper.z - box.lower.z);
            m_pPartBoxDlg->ShowResult(text);
        }
        else
        {
            m_pPartBoxDlg->ShowResult(problem);
        }
        m_scene.stockEdges = edges;
        m_stockShapeValid = false;      // the job's is read again once the dialog closes
    }
    else
    {
        if (!m_stockShapeValid)
        {
            camcore::StockBox box;
            std::wstring problem;
            m_stockEdges.clear();
            if (HasStock() && !ReadStockShape(m_job.stock, m_stockEdges, box, problem))
                Log(L"[stock] %s", problem.c_str());
            m_stockShapeValid = true;
        }
        m_scene.stockEdges = m_stockEdges;
    }

    // The chosen operation's features, outlined in orange.
    UpdateHighlights();
    m_scene.highlights = m_highlightOutlines;

    // Every generated toolpath, brought from its origin's frame into the assembly.
    m_scene.showToolpaths = !IsPicking();
    m_scene.toolpaths.clear();
    m_scene.boundaries.clear();
    for (size_t i = 0; i < m_job.sequence.size(); ++i)
    {
        const size_t active = camcore::ActiveOriginIndex(m_job, i);
        const camcore::FrameResult frame = (active != camcore::kNoStepIndex)
                                               ? camcore::BuildFrame(m_job.sequence[active].origin)
                                               : camcore::FrameResult();
        if (!frame.ok)
            continue;

        for (size_t c = 0; c < m_job.sequence[i].children.size(); ++c)
        {
            // A facing's boundary, closed back to its start.
            // Arcs are drawn as short straight segments.
            // Only the selected operation's, or its folder's, or the dialog's, like the bold toolpath.
            const bool chosen = (m_selected.step == i && (!m_selected.IsOperation() || m_selected.child == c)) ||
                                (m_pOperationDlg != nullptr && m_operationTarget.step == i &&
                                 m_operationTarget.child == c);
            for (const camcore::Path& boundary : m_job.sequence[i].children[c].boundaries)
            {
                if (boundary.empty() || !chosen)
                    continue;
                std::vector<camcore::Vec3> outline;
                for (const camcore::Vec3& point : camcore::PathPoints(boundary, true))
                    outline.push_back(camcore::FramePointToWorld(frame.frame, point));
                m_scene.boundaries.push_back(std::move(outline));
            }

            const std::vector<camcore::ToolpathMove>& moves = m_job.sequence[i].children[c].toolpath.moves;
            if (moves.empty())
                continue;

            CamScene::Toolpath drawn;
            // Bold when its operation is selected, or its folder is, which selects them all.
            drawn.selected = (m_selected.step == i && (!m_selected.IsOperation() || m_selected.child == c));
            drawn.suppressed = camcore::IsOperationSuppressed(m_job, i, c);

            // A run carries on while moves are the same kind: rapids, straight feeds,
            // or arcs, which have their own colour. The first move only says where it
            // starts. Arcs are drawn as short straight segments.
            enum class RunKind { None, Rapid, Feed, Arc };
            camcore::Vec3 local = moves.front().to;
            camcore::Vec3 at = camcore::FramePointToWorld(frame.frame, local);
            RunKind runKind = RunKind::None;
            for (size_t m = 1; m < moves.size(); ++m)
            {
                const camcore::ToolpathMove& move = moves[m];
                if (move.kind == camcore::MoveKind::Dwell)
                    continue;

                const RunKind kind = (move.kind == camcore::MoveKind::Rapid) ? RunKind::Rapid
                                     : move.arc                              ? RunKind::Arc
                                                                             : RunKind::Feed;
                std::vector<std::vector<camcore::Vec3>>& runs = (kind == RunKind::Rapid) ? drawn.rapids
                                                                : (kind == RunKind::Arc) ? drawn.arcs
                                                                                         : drawn.feeds;
                if (kind != runKind || runs.empty())
                    runs.push_back({ at });
                for (const camcore::Vec3& point : camcore::MovePoints(local, move))
                    runs.back().push_back(camcore::FramePointToWorld(frame.frame, point));
                runKind = kind;
                local = move.to;
                at = runs.back().back();
            }
            m_scene.toolpaths.push_back(std::move(drawn));
        }
    }

    if (m_pView != nullptr)
        m_pView->Refresh();
}

bool CCamTabCommand::PrepareSelection(const SelectionFilter& wanted)
{
    if (!StartView(L"picking"))
        return false;

    try
    {
        IADSessionPtr session = GetSession();
        const SelectionFilter previous = SetSelectionFilter(session, wanted);
        if (!m_filterChanged)
        {
            m_previousFilter = previous;
            m_filterChanged = true;
        }
        ClearSelection(session);
    }
    catch (const _com_error& error)
    {
        Log(L"[pick] could not prepare the pick: hr=0x%08X", static_cast<unsigned>(error.Error()));
    }

    RebuildScene();
    return true;
}

bool CCamTabCommand::IsPicking() const
{
    return (m_pPartBoxDlg != nullptr && m_pPartBoxDlg->IsSelectingParts()) ||
           (m_pOriginDlg != nullptr && m_pOriginDlg->GetPickSlot() != PickSlot::None) ||
           (m_pOperationDlg != nullptr && (m_pOperationDlg->IsLocating() || m_pOperationDlg->IsSelectingFeatures()));
}

void CCamTabCommand::BeginPick(PickSlot slot)
{
    // Each pick also takes Alibre's design items: planes and axes for the plane,
    // points for the position, and axes for X.
    SelectionFilter wanted;
    wanted.solid = (slot == PickSlot::Plane) ? ADSelectionFilterOption_AD_FACES_EDGES : ADSelectionFilterOption_AD_EDGES;
    wanted.planes = (slot == PickSlot::Plane);
    wanted.axes = (slot == PickSlot::Plane || slot == PickSlot::XDirection);
    wanted.points = (slot == PickSlot::Position);
    if (!PrepareSelection(wanted) && m_pOriginDlg != nullptr)
    {
        m_pOriginDlg->StopPicking();
        m_pOriginDlg->ShowPickProblem(L"Picking couldn't start. Click the CAM tab, then click Pick again.");
    }
}

void CCamTabCommand::EndPick()
{
    RestoreSelectionFilter();
}

void CCamTabCommand::BeginLocate()
{
    SelectionFilter wanted;
    wanted.solid = ADSelectionFilterOption_AD_FACES_EDGES;
    wanted.planes = true;
    wanted.points = true;
    if (!PrepareSelection(wanted) && m_pOperationDlg != nullptr)
    {
        m_pOperationDlg->StopLocating();
        m_pOperationDlg->ShowLocateProblem(L"Locate couldn't start. Click the CAM tab, then click Locate again.");
    }
}

void CCamTabCommand::EndLocate()
{
    RestoreSelectionFilter();
}

void CCamTabCommand::BeginFeatureSelect()
{
    if (m_pOperationDlg == nullptr)
        return;

    SelectionFilter wanted;
    wanted.solid = ADSelectionFilterOption_AD_FACES_EDGES;
    wanted.points = true;
    m_showingFeatures = false;
    if (!PrepareSelection(wanted))
    {
        m_pOperationDlg->StopSelectingFeatures();
        m_pOperationDlg->ShowFeatureNote(L"Selecting couldn't start. Click the CAM tab, then try again.");
        return;
    }

    // The list's features start out selected, so the user adds to them or removes them.
    try
    {
        const std::vector<camcore::PickedGeometry> features = m_pOperationDlg->GetFeatures();
        const int selected = SelectFeatures(GetSession(), features);
        const int missing = static_cast<int>(features.size()) - selected;
        if (missing > 0)
            Log(L"[features] %d listed features couldn't be selected", missing);
    }
    catch (const _com_error& error)
    {
        Log(L"[features] could not find the session: hr=0x%08X", static_cast<unsigned>(error.Error()));
    }
}

void CCamTabCommand::EndFeatureSelect()
{
    m_showingFeatures = false;
    try
    {
        IADSessionPtr session = GetSession();
        HighlightFeature(session, nullptr);
        ClearSelection(session);
    }
    catch (const _com_error& error)
    {
        Log(L"[features] could not find the session: hr=0x%08X", static_cast<unsigned>(error.Error()));
    }
    RestoreSelectionFilter();

    // The list may have changed, so its outlines are drawn again.
    RebuildScene();
}

void CCamTabCommand::ShowFeaturesInView(const std::vector<camcore::PickedGeometry>& features)
{
    if (m_pOperationDlg == nullptr)
        return;

    try
    {
        IADSessionPtr session = GetSession();
        if (m_pOperationDlg->IsSelectingFeatures())
        {
            // The selection is the whole list then, so the chosen row is highlighted instead.
            HighlightFeature(session, features.empty() ? nullptr : &features.front());
        }
        else
        {
            SelectFeatures(session, features);
            m_showingFeatures = !features.empty();
        }
    }
    catch (const _com_error& error)
    {
        Log(L"[features] could not find the session: hr=0x%08X", static_cast<unsigned>(error.Error()));
    }

    RebuildScene();     // rows may have been deleted or moved
}

bool CCamTabCommand::GetOperationFrame(camcore::Frame& frame) const
{
    // An operation runs under the origin its folder does.
    const size_t active = camcore::ActiveOriginIndex(m_job, m_operationTarget.step);
    if (active == camcore::kNoStepIndex)
        return false;

    const camcore::FrameResult result = camcore::BuildFrame(m_job.sequence[active].origin);
    if (result.ok)
        frame = result.frame;
    return result.ok;
}

void CCamTabCommand::RestoreSelectionFilter()
{
    // Picking has ended, so the plane can be drawn again.
    RebuildScene();

    if (!m_filterChanged)
        return;

    m_filterChanged = false;
    try
    {
        SetSelectionFilter(GetSession(), m_previousFilter);
    }
    catch (const _com_error& error)
    {
        Log(L"[pick] could not restore the selection filter: hr=0x%08X", static_cast<unsigned>(error.Error()));
    }
}

void CCamTabCommand::HandleSelection()
{
    if (m_pPartBoxDlg != nullptr && m_pPartBoxDlg->IsSelectingParts())
    {
        // Each click adds a part, or takes a listed one off. The selection is then
        // cleared, which comes back here with nothing selected.
        std::vector<camcore::StockPart> parts;
        try
        {
            if (ReadSelectedParts(GetSession(), parts) && !parts.empty())
            {
                m_pPartBoxDlg->ToggleParts(parts);
                if (m_pWnd != nullptr)
                    m_pWnd->PostClearSelection();
            }
        }
        catch (const _com_error& error)
        {
            Log(L"[stock] could not find the session: hr=0x%08X", static_cast<unsigned>(error.Error()));
        }
        return;
    }

    if (m_pOperationDlg != nullptr && m_pOperationDlg->IsLocating())
    {
        HandleLocate();
        return;
    }

    if (m_pOperationDlg != nullptr && m_pOperationDlg->IsSelectingFeatures())
    {
        // The whole selection is the feature list, so it's read again on every change.
        std::vector<camcore::PickedGeometry> features;
        int ignored = 0;
        try
        {
            if (ReadSelectedFeatures(GetSession(), m_pOperationDlg->GetFeatureFilter(), features, ignored))
                m_pOperationDlg->SetSelectedFeatures(features, ignored);
        }
        catch (const _com_error& error)
        {
            Log(L"[features] could not find the session: hr=0x%08X", static_cast<unsigned>(error.Error()));
        }
        return;
    }

    if (m_pOriginDlg == nullptr)
        return;

    const PickSlot slot = m_pOriginDlg->GetPickSlot();
    if (slot == PickSlot::None)
        return;

    camcore::PickedGeometry pick;
    std::wstring problem;
    bool read = false;
    try
    {
        const PickFilter filter = (slot == PickSlot::Plane)      ? PickFilter::OriginPlane
                                  : (slot == PickSlot::Position) ? PickFilter::OriginPosition
                                                                 : PickFilter::OriginX;
        read = ReadSelectedPick(GetSession(), filter, pick, problem);
    }
    catch (const _com_error& error)
    {
        Log(L"[pick] could not find the session: hr=0x%08X", static_cast<unsigned>(error.Error()));
    }

    // Clearing the selection calls this again with nothing selected.
    if (!read && problem.empty())
        return;

    if (read && slot == PickSlot::XDirection && pick.kind != camcore::PickKind::LineEdge &&
        pick.kind != camcore::PickKind::DesignAxis)
    {
        read = false;
        problem = L"The X direction needs a straight edge or a design axis.";
    }

    if (read)
    {
        m_pOriginDlg->SetPick(slot, pick);
        RestoreSelectionFilter();
    }
    else
    {
        m_pOriginDlg->ShowPickProblem(problem);
    }

    // Clear the selection after Alibre has finished with this one, so the same
    // item can be picked again.
    if (m_pWnd != nullptr)
        m_pWnd->PostClearSelection();
}

void CCamTabCommand::HandleLocate()
{
    camcore::LocatedGeometry located;
    std::wstring problem;
    bool read = false;
    try
    {
        read = ReadSelectedLocate(GetSession(), located, problem);
    }
    catch (const _com_error& error)
    {
        Log(L"[locate] could not find the session: hr=0x%08X", static_cast<unsigned>(error.Error()));
    }

    // Clearing the selection calls this again with nothing selected.
    if (!read && problem.empty())
        return;

    if (read)
    {
        // Measured from the origin the operation's folder runs under.
        const size_t active = camcore::ActiveOriginIndex(m_job, m_operationTarget.step);
        const camcore::FrameResult frame = (active != camcore::kNoStepIndex)
                                               ? camcore::BuildFrame(m_job.sequence[active].origin)
                                               : camcore::FrameResult();
        if (frame.ok)
        {
            double height = 0.0;
            const bool found = camcore::HeightAlongZ(frame.frame, located, height);
            Log(L"[locate] %s %.4f mm from %s", found ? L"height" : L"no height", height,
                FromUtf8(m_job.sequence[active].name).c_str());
            m_pOperationDlg->SetLocatedHeight(found, height);
            RestoreSelectionFilter();
        }
        else
        {
            m_pOperationDlg->ShowLocateProblem(L"There's no complete Set Origin above this folder to measure from.");
        }
    }
    else
    {
        m_pOperationDlg->ShowLocateProblem(problem);
    }

    if (m_pWnd != nullptr)
        m_pWnd->PostClearSelection();
}

void CCamTabCommand::ClearPickedSelection()
{
    try
    {
        ClearSelection(GetSession());
    }
    catch (const _com_error& error)
    {
        Log(L"[pick] could not find the session: hr=0x%08X", static_cast<unsigned>(error.Error()));
    }
}

/////////////////////////////////////////////////////////////////////////////
// CCamTabWnd

BEGIN_MESSAGE_MAP(CCamTabWnd, CWnd)
    ON_WM_CREATE()
    ON_WM_DESTROY()
    ON_WM_SIZE()
    ON_WM_TIMER()
    ON_BN_CLICKED(IDC_CAM_SAVE, &CCamTabWnd::OnSave)
    ON_BN_CLICKED(IDC_CAM_REFRESH, &CCamTabWnd::OnRefresh)
    ON_BN_CLICKED(IDC_CAM_FOREGROUND, &CCamTabWnd::OnForeground)
    ON_BN_CLICKED(IDC_CAM_SHOW_TOOLPATHS, &CCamTabWnd::OnShowToolpaths)
    ON_BN_CLICKED(IDC_CAM_SHOW_STOCK, &CCamTabWnd::OnShowStock)
    ON_NOTIFY(TBN_GETINFOTIP, IDC_CAM_TOOLBAR, &CCamTabWnd::OnToolbarTip)
    ON_NOTIFY(TBN_GETINFOTIP, IDC_CAM_SETTINGS_BAR, &CCamTabWnd::OnToolbarTip)
    ON_BN_CLICKED(IDC_CAM_SETTINGS, &CCamTabWnd::OnSettings)
    ON_BN_CLICKED(IDC_CAM_HELP, &CCamTabWnd::OnHelp)
    ON_NOTIFY(TVN_SELCHANGED, IDC_CAM_TREE, &CCamTabWnd::OnTreeSelectionChanged)
    ON_NOTIFY(NM_DBLCLK, IDC_CAM_TREE, &CCamTabWnd::OnTreeDoubleClick)
    ON_NOTIFY(NM_RCLICK, IDC_CAM_TREE, &CCamTabWnd::OnTreeRightClick)
    ON_NOTIFY(TVN_BEGINLABELEDIT, IDC_CAM_TREE, &CCamTabWnd::OnTreeBeginRename)
    ON_NOTIFY(TVN_ENDLABELEDIT, IDC_CAM_TREE, &CCamTabWnd::OnTreeEndRename)
    ON_NOTIFY(TVN_BEGINDRAG, IDC_CAM_TREE, &CCamTabWnd::OnTreeBeginDrag)
    ON_NOTIFY(NM_CUSTOMDRAW, IDC_CAM_TREE, &CCamTabWnd::OnTreeCustomDraw)
    ON_WM_MOUSEMOVE()
    ON_WM_LBUTTONUP()
    ON_WM_RBUTTONDOWN()
    ON_WM_CAPTURECHANGED()
    ON_MESSAGE(WM_APP_LOAD_JOB, &CCamTabWnd::OnLoadJob)
    ON_MESSAGE(WM_APP_CLEAR_SELECTION, &CCamTabWnd::OnClearSelection)
END_MESSAGE_MAP()

CCamTabWnd::CCamTabWnd(CCamTabCommand* pTab)
    : m_pTab(pTab)
    , m_hMachine(nullptr)
    , m_hTools(nullptr)
    , m_hMaterial(nullptr)
    , m_hStock(nullptr)
    , m_hOperations(nullptr)
    , m_fillingTree(false)
    , m_visible(false)
    , m_settingsWritable(true)
    , m_dragging(false)
{
}

bool CCamTabWnd::CreateInTab(HWND hParent)
{
    CRect client;
    ::GetClientRect(hParent, &client);

    const CString windowClass = AfxRegisterWndClass(CS_DBLCLKS, ::LoadCursor(nullptr, IDC_ARROW),
                                                    reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1)));

    return CreateEx(0, windowClass, L"CAM", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, client,
                    CWnd::FromHandle(hParent), 1) != FALSE;
}

int CCamTabWnd::OnCreate(LPCREATESTRUCT pCreate)
{
    if (CWnd::OnCreate(pCreate) == -1)
        return -1;

    CFont* pFont = CFont::FromHandle(static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT)));

    // Labels can be edited, but OnTreeBeginRename allows it only for folders and operations.
    m_tree.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES |
                      TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_EDITLABELS,
                  CRect(0, 0, 10, 10), this, IDC_CAM_TREE);
    m_tree.SetFont(pFont);

    HTREEITEM hSetup = m_tree.InsertItem(L"Setup");
    m_hMachine = m_tree.InsertItem(L"Machine", hSetup);
    m_hTools = m_tree.InsertItem(L"Tools", hSetup);
    m_hMaterial = m_tree.InsertItem(L"Material", hSetup);
    m_hStock = m_tree.InsertItem(L"Stock", hSetup);
    m_hOperations = m_tree.InsertItem(L"Operations");
    m_tree.Expand(hSetup, TVE_EXPAND);

    m_status.Create(L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, CRect(0, 0, 10, 10), this, IDC_CAM_STATUS);
    m_status.SetFont(pFont);

    m_refresh.Create(L"Refresh view", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, CRect(0, 0, 10, 10), this,
                     IDC_CAM_REFRESH);
    m_refresh.SetFont(pFont);

    // The view toggles: small image buttons, pushed in while on, each with a tooltip.
    m_toolbar.Create(WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_NODIVIDER | CCS_NORESIZE |
                         CCS_NOPARENTALIGN,
                     CRect(0, 0, 10, 10), this, IDC_CAM_TOOLBAR);
    TBBUTTON buttons[3] = {};
    const int commands[3] = { IDC_CAM_FOREGROUND, IDC_CAM_SHOW_TOOLPATHS, IDC_CAM_SHOW_STOCK };
    const int images[3] = { 0, 1, 4 };
    for (int i = 0; i < 3; ++i)
    {
        buttons[i].iBitmap = images[i];
        buttons[i].idCommand = commands[i];
        buttons[i].fsState = TBSTATE_ENABLED | TBSTATE_CHECKED;     // all start on
        buttons[i].fsStyle = BTNS_CHECK;
    }
    m_toolbar.AddButtons(3, buttons);

    // Help, and the gear that opens the Settings dialog, are a toolbar of their own, kept at the right.
    m_settingsBar.Create(WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_NODIVIDER | CCS_NORESIZE |
                             CCS_NOPARENTALIGN,
                         CRect(0, 0, 10, 10), this, IDC_CAM_SETTINGS_BAR);
    TBBUTTON right[2] = {};
    const int rightCommands[2] = { IDC_CAM_HELP, IDC_CAM_SETTINGS };
    const int rightImages[2] = { 3, 2 };
    for (int i = 0; i < 2; ++i)
    {
        right[i].iBitmap = rightImages[i];
        right[i].idCommand = rightCommands[i];
        right[i].fsState = TBSTATE_ENABLED;
        right[i].fsStyle = BTNS_BUTTON;
    }
    m_settingsBar.AddButtons(2, right);


    m_save.Create(L"Save", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, CRect(0, 0, 10, 10), this, IDC_CAM_SAVE);
    m_save.SetFont(pFont);

    // The view size, and the text and icons at it.
    const camcore::ProgramSettingsLoadResult loaded = camcore::LoadProgramSettings(ProgramSettingsPath());
    m_settings = loaded.settings;
    m_settingsWritable = loaded.status == camcore::LoadStatus::Ok || loaded.status == camcore::LoadStatus::NotFound;
    if (!m_settingsWritable)
        Log(L"[settings] %s", FromUtf8(loaded.message).c_str());
    ApplyViewSize();


    // The window Alibre creates for the tab doesn't always resize this one.
    SetTimer(kResizeTimer, kResizeMilliseconds, nullptr);

    // Loading can prompt, so wait until Alibre has finished showing the tab.
    PostMessage(WM_APP_LOAD_JOB);
    return 0;
}

void CCamTabWnd::OnDestroy()
{
    KillTimer(kResizeTimer);

    if (m_pTab != nullptr)
        m_pTab->OnWindowClosing();

    CWnd::OnDestroy();
}

void CCamTabWnd::PostNcDestroy()
{
    if (m_pTab != nullptr)
        m_pTab->OnWindowGone();
    delete this;
}

void CCamTabWnd::LayoutControls(int cx, int cy)
{
    if (m_tree.GetSafeHwnd() == nullptr)
        return;

    const double scale = camcore::ViewTextScale(m_settings.viewSize);
    const int margin = 4;
    const int buttonHeight = static_cast<int>(23 * scale + 0.5);
    const int statusHeight = static_cast<int>(60 * scale + 0.5);
    const int width = (cx > margin * 2) ? cx - margin * 2 : 1;

    // The toolbars across the top (help and the gear at the right), then the tree, the
    // status, and the buttons at the bottom.
    const int icon = camcore::ViewIconSize(m_settings.viewSize);
    const int toolbarHeight = icon + 8;
    const int gearWidth = (icon + 8) * 2 + 2;     // help and the gear
    const int treeY = margin + toolbarHeight + margin;
    const int saveY = cy - margin - buttonHeight;
    const int refreshY = saveY - margin - buttonHeight;
    const int statusY = refreshY - margin - statusHeight;
    const int treeHeight = statusY - margin - treeY;

    m_toolbar.MoveWindow(margin, margin, (width > gearWidth) ? width - gearWidth : 1, toolbarHeight);
    m_settingsBar.MoveWindow(margin + width - gearWidth, margin, gearWidth, toolbarHeight);
    m_tree.MoveWindow(margin, treeY, width, (treeHeight > 40) ? treeHeight : 40);
    m_status.MoveWindow(margin, statusY, width, statusHeight);
    m_refresh.MoveWindow(margin, refreshY, width, buttonHeight);
    m_save.MoveWindow(margin, saveY, width, buttonHeight);
}

void CCamTabWnd::OnSize(UINT type, int cx, int cy)
{
    CWnd::OnSize(type, cx, cy);
    LayoutControls(cx, cy);
}

void CCamTabWnd::OnTimer(UINT_PTR id)
{
    if (id != kResizeTimer)
    {
        CWnd::OnTimer(id);
        return;
    }

    CWnd* pParent = GetParent();
    if (pParent == nullptr)
        return;

    CRect parentClient, own;
    pParent->GetClientRect(&parentClient);
    GetClientRect(&own);
    if (parentClient.Width() != own.Width() || parentClient.Height() != own.Height())
        MoveWindow(0, 0, parentClient.Width(), parentClient.Height());

    // The tab has focus while its window is visible.
    const bool visible = (::IsWindowVisible(m_hWnd) != FALSE);
    if (visible != m_visible)
    {
        m_visible = visible;
        if (m_pTab != nullptr)
            m_pTab->OnTabVisibilityChanged(visible);
    }
}

void CCamTabWnd::OnSave()
{
    if (m_pTab != nullptr)
        m_pTab->SaveJobFile();
}

void CCamTabWnd::OnRefresh()
{
    if (m_pTab != nullptr)
        m_pTab->RefreshView();
}

void CCamTabWnd::OnForeground()
{
    if (m_pTab != nullptr)
        m_pTab->SetForeground(m_toolbar.IsButtonChecked(IDC_CAM_FOREGROUND) != FALSE);
}

void CCamTabWnd::OnSettings()
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    CSettingsDlg dialog(m_settings, this);
    if (dialog.DoModal() != IDOK)
        return;

    m_settings = dialog.GetSettings();
    ApplyViewSize();

    // The last machine is kept by the Machine dialog, so this doesn't put back an older one.
    const camcore::ProgramSettingsLoadResult current = camcore::LoadProgramSettings(ProgramSettingsPath());
    if (current.status == camcore::LoadStatus::Ok)
        m_settings.lastMachineId = current.settings.lastMachineId;

    // A damaged settings file, or one from a newer add-on, isn't overwritten.
    if (!m_settingsWritable)
    {
        MessageBoxW(L"The settings file couldn't be read, or is from a newer AlibreCAM, so these settings "
                    L"apply until Alibre closes but aren't saved.",
                    L"AlibreCAM", MB_OK | MB_ICONINFORMATION);
        return;
    }
    const camcore::SaveResult saved = camcore::SaveProgramSettings(m_settings, ProgramSettingsPath());
    if (!saved.ok)
        MessageBoxW(FromUtf8(saved.message).c_str(), L"AlibreCAM", MB_OK | MB_ICONWARNING);
}

void CCamTabWnd::ApplyViewSize()
{
    // The tab's text: Windows' default GUI font, bigger for Medium and Large.
    LOGFONT logFont = {};
    ::GetObject(::GetStockObject(DEFAULT_GUI_FONT), sizeof(logFont), &logFont);
    logFont.lfHeight = static_cast<LONG>(logFont.lfHeight * camcore::ViewTextScale(m_settings.viewSize) +
                                         (logFont.lfHeight < 0 ? -0.5 : 0.5));
    m_font.DeleteObject();
    m_font.CreateFontIndirect(&logFont);
    for (CWnd* pControl : { static_cast<CWnd*>(&m_tree), static_cast<CWnd*>(&m_status), static_cast<CWnd*>(&m_refresh),
                            static_cast<CWnd*>(&m_save) })
        pControl->SetFont(&m_font);

    // The icons, drawn at each size rather than stretched.
    const int icon = camcore::ViewIconSize(m_settings.viewSize);
    const UINT strip = (icon == 32) ? IDB_CAM_TOOLBAR_32 : (icon == 24) ? IDB_CAM_TOOLBAR_24 : IDB_CAM_TOOLBAR_16;
    m_toolbarImages.DeleteImageList();
    if (!m_toolbarImages.Create(strip, icon, 0, RGB(255, 0, 255)))
        Log(L"[tab] could not load the %d pixel toolbar images", icon);
    for (CToolBarCtrl* pBar : { &m_toolbar, &m_settingsBar })
    {
        pBar->SetImageList(&m_toolbarImages);
        pBar->SetBitmapSize(CSize(icon, icon));
        pBar->SetButtonSize(CSize(icon + 8, icon + 6));
        pBar->Invalidate();
    }

    CRect client;
    GetClientRect(&client);
    LayoutControls(client.Width(), client.Height());
}

void CCamTabWnd::OnHelp()
{
    ShowHelp(L"cam-tab");
}

void CCamTabWnd::OnShowStock()
{
    if (m_pTab != nullptr)
        m_pTab->SetStockShown(m_toolbar.IsButtonChecked(IDC_CAM_SHOW_STOCK) != FALSE);
}

void CCamTabWnd::ShowStock(const camcore::Stock& stock)
{
    if (m_hStock == nullptr)
        return;
    const std::string title = camcore::StockKindTitle(stock.kind);
    const std::wstring text = title.empty() ? L"Stock" : L"Stock - " + FromUtf8(title);
    m_tree.SetItemText(m_hStock, text.c_str());
}

void CCamTabWnd::OnShowToolpaths()
{
    if (m_pTab != nullptr)
        m_pTab->SetToolpathsShown(m_toolbar.IsButtonChecked(IDC_CAM_SHOW_TOOLPATHS) != FALSE);
}

void CCamTabWnd::OnToolbarTip(NMHDR* pNotify, LRESULT* pResult)
{
    NMTBGETINFOTIP* pTip = reinterpret_cast<NMTBGETINFOTIP*>(pNotify);
    const wchar_t* text = (pTip->iItem == IDC_CAM_FOREGROUND)       ? L"Draw over model"
                          : (pTip->iItem == IDC_CAM_SHOW_TOOLPATHS) ? L"Show toolpaths"
                          : (pTip->iItem == IDC_CAM_SETTINGS)       ? L"Settings"
                          : (pTip->iItem == IDC_CAM_HELP)           ? L"Help"
                          : (pTip->iItem == IDC_CAM_SHOW_STOCK)     ? L"Show stock"
                                                                    : L"";
    if (pTip->pszText != nullptr && pTip->cchTextMax > 0)
        wcsncpy_s(pTip->pszText, pTip->cchTextMax, text, _TRUNCATE);
    *pResult = 0;
}

bool CCamTabWnd::IsStepItem(HTREEITEM hItem) const
{
    // A top-level step, or an operation in a folder.
    if (hItem == nullptr)
        return false;
    const HTREEITEM hParent = m_tree.GetParentItem(hItem);
    return hParent == m_hOperations || (hParent != nullptr && m_tree.GetParentItem(hParent) == m_hOperations);
}

StepRef CCamTabWnd::StepOfItem(HTREEITEM hItem) const
{
    return IsStepItem(hItem) ? StepFromItemData(m_tree.GetItemData(hItem)) : StepRef();
}

HTREEITEM CCamTabWnd::FindStepItem(const StepRef& ref) const
{
    for (HTREEITEM hItem = m_tree.GetChildItem(m_hOperations); hItem != nullptr;
         hItem = m_tree.GetNextSiblingItem(hItem))
    {
        if (StepOfItem(hItem).step != ref.step)
            continue;
        if (!ref.IsOperation())
            return hItem;

        for (HTREEITEM hChild = m_tree.GetChildItem(hItem); hChild != nullptr;
             hChild = m_tree.GetNextSiblingItem(hChild))
        {
            if (StepOfItem(hChild).child == ref.child)
                return hChild;
        }
        return nullptr;
    }
    return nullptr;
}

HTREEITEM CCamTabWnd::ItemUnderCursor(bool select)
{
    CPoint point;
    ::GetCursorPos(&point);
    m_tree.ScreenToClient(&point);

    const HTREEITEM hItem = m_tree.HitTest(point);
    if (select && hItem != nullptr)
        m_tree.SelectItem(hItem);
    return hItem;
}

void CCamTabWnd::OnTreeSelectionChanged(NMHDR* /*pNotify*/, LRESULT* pResult)
{
    *pResult = 0;

    if (m_fillingTree || m_pTab == nullptr)
        return;

    const HTREEITEM hItem = m_tree.GetSelectedItem();
    if (hItem != nullptr && m_tree.GetParentItem(hItem) == m_hTools)
        m_pTab->SelectToolByIndex(static_cast<size_t>(m_tree.GetItemData(hItem)));
    else if (IsStepItem(hItem))
        m_pTab->SelectStep(StepOfItem(hItem));
}

void CCamTabWnd::OnTreeRightClick(NMHDR* /*pNotify*/, LRESULT* pResult)
{
    *pResult = 1;

    if (m_pTab == nullptr)
        return;

    const HTREEITEM hItem = ItemUnderCursor(true);
    if (hItem != nullptr && (hItem == m_hMachine || hItem == m_hMaterial))
    {
        const UINT editable = m_pTab->CanEditSequence() ? MF_ENABLED : MF_GRAYED;
        CMenu setupMenu;
        setupMenu.CreatePopupMenu();
        if (hItem == m_hMachine)
        {
            setupMenu.AppendMenu(MF_STRING | editable, ID_CAM_MACHINE_EDIT, L"Edit Machine...");
        }
        else
        {
            // The materials to choose from, the job's ticked.
            const camcore::MaterialList& materials = m_pTab->GetMaterials();
            const std::string& current = m_pTab->GetMaterialId();
            for (size_t i = 0; i < materials.materials.size() && ID_CAM_MATERIAL_FIRST + i <= ID_CAM_MATERIAL_LAST; ++i)
            {
                const camcore::Material& material = materials.materials[i];
                const UINT check = (material.id == current) ? MF_CHECKED : MF_UNCHECKED;
                setupMenu.AppendMenu(MF_STRING | editable | check, ID_CAM_MATERIAL_FIRST + static_cast<UINT>(i),
                                     FromUtf8(material.name).c_str());
            }
            setupMenu.AppendMenu(MF_STRING | editable | (current.empty() ? MF_CHECKED : MF_UNCHECKED),
                                 ID_CAM_MATERIAL_NONE, L"No Material");
            setupMenu.AppendMenu(MF_SEPARATOR);
            setupMenu.AppendMenu(MF_STRING | editable, ID_CAM_MATERIAL_EDIT, L"Edit Materials...");
        }

        CPoint at;
        ::GetCursorPos(&at);
        const UINT chosen = setupMenu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                                     at.x, at.y, this);
        if (m_pTab == nullptr || chosen == 0)
            return;
        if (chosen == ID_CAM_MACHINE_EDIT)
            m_pTab->OpenMachineDialog(CMachineDlg::MachineTab);
        else if (chosen == ID_CAM_MATERIAL_EDIT)
            m_pTab->OpenMachineDialog(CMachineDlg::MaterialsTab);
        else if (chosen == ID_CAM_MATERIAL_NONE)
            m_pTab->ChooseMaterial(std::string());
        else if (chosen >= ID_CAM_MATERIAL_FIRST && chosen <= ID_CAM_MATERIAL_LAST)
        {
            const size_t index = chosen - ID_CAM_MATERIAL_FIRST;
            const camcore::MaterialList& materials = m_pTab->GetMaterials();
            if (index < materials.materials.size())
                m_pTab->ChooseMaterial(materials.materials[index].id);
        }
        return;
    }

    if (hItem != nullptr && hItem == m_hStock)
    {
        // One stock: the kinds while there's none, then Edit and Delete.
        const bool editable = m_pTab->CanEditSequence();
        const UINT makeable = (editable && !m_pTab->HasStock()) ? MF_ENABLED : MF_GRAYED;
        const UINT changeable = (editable && m_pTab->HasStock()) ? MF_ENABLED : MF_GRAYED;
        CMenu stockMenu;
        stockMenu.CreatePopupMenu();
        stockMenu.AppendMenu(MF_STRING | makeable, ID_CAM_STOCK_BOX, L"Box Stock...");
        stockMenu.AppendMenu(MF_STRING | makeable, ID_CAM_STOCK_PART_BOX, L"Part Box Stock...");
        stockMenu.AppendMenu(MF_STRING | makeable, ID_CAM_STOCK_IMPORT, L"Import Stock...");
        stockMenu.AppendMenu(MF_SEPARATOR);
        stockMenu.AppendMenu(MF_STRING | changeable, ID_CAM_STOCK_EDIT, L"Edit...");
        stockMenu.AppendMenu(MF_STRING | changeable, ID_CAM_STOCK_DELETE, L"Delete");

        CPoint at;
        ::GetCursorPos(&at);
        const UINT chosen = stockMenu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                                     at.x, at.y, this);
        if (m_pTab == nullptr)
            return;
        switch (chosen)
        {
        case ID_CAM_STOCK_BOX:      m_pTab->MakeStock(camcore::StockKind::Box); break;
        case ID_CAM_STOCK_PART_BOX: m_pTab->MakeStock(camcore::StockKind::PartBox); break;
        case ID_CAM_STOCK_IMPORT:   m_pTab->MakeStock(camcore::StockKind::Imported); break;
        case ID_CAM_STOCK_EDIT:     m_pTab->EditStock(); break;
        case ID_CAM_STOCK_DELETE:   m_pTab->DeleteStock(); break;
        default: break;
        }
        return;
    }

    const bool onStep = IsStepItem(hItem);
    if (hItem != m_hOperations && !onStep)
        return;

    const StepRef ref = StepOfItem(hItem);
    const UINT editable = m_pTab->CanEditSequence() ? MF_ENABLED : MF_GRAYED;
    const UINT renamable = m_pTab->CanRename(ref) ? MF_ENABLED : MF_GRAYED;
    const UINT deletable = m_pTab->CanDelete(ref) ? MF_ENABLED : MF_GRAYED;
    const UINT duplicable = m_pTab->CanDrag(ref) ? MF_ENABLED : MF_GRAYED;
    const UINT generatable = m_pTab->CanGenerate() ? MF_ENABLED : MF_GRAYED;
    const UINT suppressible = (onStep && m_pTab->CanSuppress(ref)) ? MF_ENABLED : MF_GRAYED;
    const UINT suppressedCheck = (onStep && m_pTab->IsSuppressed(ref)) ? MF_CHECKED : MF_UNCHECKED;

    CMenu menu;
    menu.CreatePopupMenu();
    CMenu holes;
    CMenu twoAndAHalfAxis;
    if (onStep && m_pTab->IsFolder(ref))
    {
        menu.AppendMenu(MF_STRING | renamable, ID_CAM_RENAME_STEP, L"Rename");
        menu.AppendMenu(MF_STRING | deletable, ID_CAM_DELETE_STEP, L"Delete");
        menu.AppendMenu(MF_STRING | duplicable, ID_CAM_DUPLICATE_STEP, L"Duplicate");
        menu.AppendMenu(MF_STRING | suppressible | suppressedCheck, ID_CAM_SUPPRESS_STEP, L"Suppress");
        menu.AppendMenu(MF_SEPARATOR);

        holes.CreatePopupMenu();
        holes.AppendMenu(MF_STRING | editable, ID_CAM_ADD_DRILL, L"Drill...");
        holes.AppendMenu(MF_STRING | editable, ID_CAM_ADD_HOLE_POCKETING, L"Hole Pocketing...");
        menu.AppendMenu(MF_POPUP | editable, reinterpret_cast<UINT_PTR>(holes.GetSafeHmenu()), L"Add Holes");

        twoAndAHalfAxis.CreatePopupMenu();
        twoAndAHalfAxis.AppendMenu(MF_STRING | editable, ID_CAM_ADD_FACING, L"Facing...");
        twoAndAHalfAxis.AppendMenu(MF_STRING | editable, ID_CAM_ADD_PROFILING, L"Profiling...");
        twoAndAHalfAxis.AppendMenu(MF_STRING | editable, ID_CAM_ADD_POCKETING, L"Pocketing...");
        menu.AppendMenu(MF_POPUP | editable, reinterpret_cast<UINT_PTR>(twoAndAHalfAxis.GetSafeHmenu()),
                        L"Add 2 1/2 Axis");
        menu.AppendMenu(MF_SEPARATOR);
        menu.AppendMenu(MF_STRING | generatable, ID_CAM_GENERATE, L"Generate");
        menu.AppendMenu(MF_STRING | generatable, ID_CAM_POST, L"Post...");
    }
    else if (onStep && m_pTab->IsOperation(ref))
    {
        menu.AppendMenu(MF_STRING | editable, ID_CAM_EDIT_STEP, L"Edit...");
        menu.AppendMenu(MF_STRING | renamable, ID_CAM_RENAME_STEP, L"Rename");
        menu.AppendMenu(MF_STRING | deletable, ID_CAM_DELETE_STEP, L"Delete");
        menu.AppendMenu(MF_STRING | duplicable, ID_CAM_DUPLICATE_STEP, L"Duplicate");
        menu.AppendMenu(MF_STRING | suppressible | suppressedCheck, ID_CAM_SUPPRESS_STEP, L"Suppress");
        menu.AppendMenu(MF_SEPARATOR);
        menu.AppendMenu(MF_STRING | generatable, ID_CAM_GENERATE, L"Generate");
    }
    else
    {
        menu.AppendMenu(MF_STRING | editable, ID_CAM_ADD_ORIGIN, L"Add Set Origin...");
        menu.AppendMenu(MF_STRING | (m_pTab->CanAddFolder() ? MF_ENABLED : MF_GRAYED), ID_CAM_ADD_FOLDER,
                        L"Add Folder");
        if (onStep)
        {
            menu.AppendMenu(MF_SEPARATOR);
            menu.AppendMenu(MF_STRING | editable, ID_CAM_EDIT_STEP, L"Edit...");
            menu.AppendMenu(MF_STRING | deletable, ID_CAM_DELETE_STEP, L"Delete");
            menu.AppendMenu(MF_STRING | duplicable, ID_CAM_DUPLICATE_STEP, L"Duplicate");
            menu.AppendMenu(MF_STRING | suppressible | suppressedCheck, ID_CAM_SUPPRESS_STEP, L"Suppress");
        }
        else
        {
            menu.AppendMenu(MF_SEPARATOR);
            menu.AppendMenu(MF_STRING | generatable, ID_CAM_GENERATE, L"Generate");
            menu.AppendMenu(MF_STRING | generatable, ID_CAM_POST, L"Post...");
        }
    }

    CPoint point;
    ::GetCursorPos(&point);
    const UINT command = menu.TrackPopupMenu(TPM_LEFTALIGN | TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                             point.x, point.y, this);

    // The menu can outlive the command, if Alibre closed the tab meanwhile.
    if (m_pTab == nullptr)
        return;

    switch (command)
    {
    case ID_CAM_ADD_ORIGIN:  m_pTab->AddSetOrigin(); break;
    case ID_CAM_ADD_FOLDER:  m_pTab->AddFolder(); break;
    case ID_CAM_ADD_DRILL:   m_pTab->AddOperation(ref.step, camcore::StepType::Drill); break;
    case ID_CAM_ADD_HOLE_POCKETING: m_pTab->AddOperation(ref.step, camcore::StepType::HolePocketing); break;
    case ID_CAM_ADD_FACING:  m_pTab->AddOperation(ref.step, camcore::StepType::Facing); break;
    case ID_CAM_ADD_PROFILING: m_pTab->AddOperation(ref.step, camcore::StepType::Profiling); break;
    case ID_CAM_ADD_POCKETING: m_pTab->AddOperation(ref.step, camcore::StepType::Pocketing); break;
    case ID_CAM_EDIT_STEP:   m_pTab->EditStep(ref); break;
    case ID_CAM_RENAME_STEP: m_tree.EditLabel(hItem); break;
    case ID_CAM_DELETE_STEP: m_pTab->DeleteStep(ref); break;
    case ID_CAM_DUPLICATE_STEP: m_pTab->DuplicateStep(ref); break;
    case ID_CAM_SUPPRESS_STEP: m_pTab->ToggleSuppressed(ref); break;
    case ID_CAM_GENERATE:    m_pTab->Generate(ref); break;
    case ID_CAM_POST:        m_pTab->Post(ref); break;
    default:                 break;
    }
}

void CCamTabWnd::OnTreeBeginRename(NMHDR* pNotify, LRESULT* pResult)
{
    // Non-zero cancels the edit. Only folders and operations are renamed in place.
    const NMTVDISPINFO* pInfo = reinterpret_cast<NMTVDISPINFO*>(pNotify);
    const bool allowed = m_pTab != nullptr && m_pTab->CanRename(StepOfItem(pInfo->item.hItem));
    *pResult = allowed ? 0 : 1;
}

void CCamTabWnd::OnTreeEndRename(NMHDR* pNotify, LRESULT* pResult)
{
    // Non-zero keeps the typed text as the label. No text means it was cancelled.
    const NMTVDISPINFO* pInfo = reinterpret_cast<NMTVDISPINFO*>(pNotify);
    *pResult = 0;
    if (m_pTab == nullptr || pInfo->item.pszText == nullptr || !IsStepItem(pInfo->item.hItem))
        return;

    if (m_pTab->RenameStep(StepOfItem(pInfo->item.hItem), pInfo->item.pszText))
    {
        // The tree shows the text as typed, so trim it to match the job.
        CString name(pInfo->item.pszText);
        name.Trim();
        m_tree.SetItemText(pInfo->item.hItem, name);
    }
}

void CCamTabWnd::OnTreeBeginDrag(NMHDR* pNotify, LRESULT* pResult)
{
    *pResult = 0;

    const NMTREEVIEW* pInfo = reinterpret_cast<NMTREEVIEW*>(pNotify);
    const HTREEITEM hItem = pInfo->itemNew.hItem;
    if (m_pTab == nullptr || !IsStepItem(hItem) || !m_pTab->CanDrag(StepOfItem(hItem)))
        return;

    m_dragFrom = StepOfItem(hItem);
    m_tree.SelectItem(hItem);
    m_dragging = true;
    SetCapture();
}

CCamTabWnd::DropPlace CCamTabWnd::FindDropPlace(CPoint treePoint) const
{
    DropPlace place;
    if (m_pTab == nullptr)
        return place;

    HTREEITEM hItem = m_tree.HitTest(treePoint);
    bool lowerHalf = false;
    if (hItem != nullptr)
    {
        CRect rect;
        m_tree.GetItemRect(hItem, &rect, FALSE);
        lowerHalf = treePoint.y >= rect.CenterPoint().y;
    }
    else
    {
        // Below the last item counts as after the last top-level step.
        CRect client;
        m_tree.GetClientRect(&client);
        const HTREEITEM hLast = m_tree.GetLastVisibleItem();
        CRect lastRect;
        if (hLast == nullptr || !m_tree.GetItemRect(hLast, &lastRect, FALSE) || treePoint.y < lastRect.bottom)
            return place;

        for (HTREEITEM hStep = m_tree.GetChildItem(m_hOperations); hStep != nullptr;
             hStep = m_tree.GetNextSiblingItem(hStep))
            hItem = hStep;
        lowerHalf = true;
    }

    if (!IsStepItem(hItem))
        return place;

    const StepRef over = StepOfItem(hItem);
    if (!m_dragFrom.IsOperation())
    {
        // A top-level step only goes beside another one. Over an operation means
        // after that operation's folder.
        if (over.IsOperation())
        {
            place.target = StepRef{ over.step };
            place.side = DropSide::After;
        }
        else
        {
            place.target = over;
            place.side = lowerHalf ? DropSide::After : DropSide::Before;
        }

        // After a folder, the mark goes below its operations.
        const HTREEITEM hStep = FindStepItem(place.target);
        place.hMarkItem = hStep;
        if (place.side == DropSide::After && hStep != nullptr && (m_tree.GetItemState(hStep, TVIS_EXPANDED) & TVIS_EXPANDED))
        {
            for (HTREEITEM hChild = m_tree.GetChildItem(hStep); hChild != nullptr;
                 hChild = m_tree.GetNextSiblingItem(hChild))
                place.hMarkItem = hChild;
        }
    }
    else if (!over.IsOperation())
    {
        // An operation dropped on a folder goes at its end.
        place.target = over;
        place.side = DropSide::Into;
        place.hMarkItem = hItem;
    }
    else
    {
        place.target = over;
        place.side = lowerHalf ? DropSide::After : DropSide::Before;
        place.hMarkItem = hItem;
    }

    place.valid = m_pTab->CanMoveStep(m_dragFrom, place.target, place.side);
    return place;
}

void CCamTabWnd::ShowDropPlace(const DropPlace& place)
{
    if (place.valid && place.side == DropSide::Into)
    {
        m_tree.SetInsertMark(nullptr);
        m_tree.SelectDropTarget(place.hMarkItem);
    }
    else
    {
        m_tree.SelectDropTarget(nullptr);
        m_tree.SetInsertMark(place.valid ? place.hMarkItem : nullptr, place.side == DropSide::After);
    }
    ::SetCursor(::LoadCursor(nullptr, place.valid ? IDC_ARROW : IDC_NO));
}

void CCamTabWnd::OnMouseMove(UINT flags, CPoint point)
{
    if (!m_dragging)
    {
        CWnd::OnMouseMove(flags, point);
        return;
    }

    ClientToScreen(&point);
    m_tree.ScreenToClient(&point);

    // Scroll when the mouse is near the top or bottom of the tree.
    CRect client;
    m_tree.GetClientRect(&client);
    const int edge = 12;
    if (point.y < client.top + edge)
        m_tree.SendMessage(WM_VSCROLL, SB_LINEUP);
    else if (point.y > client.bottom - edge)
        m_tree.SendMessage(WM_VSCROLL, SB_LINEDOWN);

    ShowDropPlace(FindDropPlace(point));
}

void CCamTabWnd::OnLButtonUp(UINT flags, CPoint point)
{
    if (!m_dragging)
    {
        CWnd::OnLButtonUp(flags, point);
        return;
    }

    ClientToScreen(&point);
    m_tree.ScreenToClient(&point);
    const DropPlace place = FindDropPlace(point);
    const StepRef from = m_dragFrom;
    EndDrag();

    if (place.valid && m_pTab != nullptr)
        m_pTab->MoveStep(from, place.target, place.side);
}

void CCamTabWnd::OnRButtonDown(UINT flags, CPoint point)
{
    // A right click cancels a drag.
    if (m_dragging)
        EndDrag();
    else
        CWnd::OnRButtonDown(flags, point);
}

void CCamTabWnd::OnCaptureChanged(CWnd* pWnd)
{
    // Losing the mouse to another window cancels a drag.
    if (m_dragging)
        EndDrag();
    CWnd::OnCaptureChanged(pWnd);
}

void CCamTabWnd::EndDrag()
{
    if (!m_dragging)
        return;

    m_dragging = false;
    m_tree.SetInsertMark(nullptr);
    m_tree.SelectDropTarget(nullptr);
    if (GetCapture() == this)
        ReleaseCapture();
}

void CCamTabWnd::StartRename(const StepRef& ref)
{
    const HTREEITEM hItem = FindStepItem(ref);
    if (hItem == nullptr)
        return;

    m_tree.SetFocus();
    m_tree.SelectItem(hItem);
    m_tree.EditLabel(hItem);
}

void CCamTabWnd::OnTreeDoubleClick(NMHDR* /*pNotify*/, LRESULT* pResult)
{
    *pResult = 0;

    if (m_pTab == nullptr)
        return;

    CPoint point;
    ::GetCursorPos(&point);
    m_tree.ScreenToClient(&point);

    const HTREEITEM hItem = m_tree.HitTest(point);
    const StepRef ref = StepOfItem(hItem);
    if (hItem != nullptr && (hItem == m_hTools || m_tree.GetParentItem(hItem) == m_hTools))
    {
        // Handled, so the tree doesn't also expand or collapse the item.
        *pResult = 1;
        m_pTab->OpenMachineDialog(CMachineDlg::ToolsTab);
    }
    else if (hItem != nullptr && hItem == m_hMachine)
    {
        *pResult = 1;
        m_pTab->OpenMachineDialog(CMachineDlg::MachineTab);
    }
    else if (hItem != nullptr && hItem == m_hMaterial)
    {
        *pResult = 1;
        m_pTab->OpenMachineDialog(CMachineDlg::MaterialsTab);
    }
    else if (hItem != nullptr && hItem == m_hStock)
    {
        *pResult = 1;
        if (m_pTab->HasStock())
            m_pTab->EditStock();
    }
    else if (IsStepItem(hItem) && !m_pTab->IsFolder(ref))
    {
        *pResult = 1;
        m_pTab->EditStep(ref);
    }
    else if (hItem == m_hOperations && m_tree.GetChildItem(m_hOperations) == nullptr)
    {
        // An empty Operations item has nothing to expand, so start the first step.
        *pResult = 1;
        m_pTab->AddSetOrigin();
    }
}

void CCamTabWnd::ShowSequence(const camcore::Job& job, const StepRef& selected)
{
    m_fillingTree = true;

    HTREEITEM hChild = m_tree.GetChildItem(m_hOperations);
    while (hChild != nullptr)
    {
        const HTREEITEM hNext = m_tree.GetNextSiblingItem(hChild);
        m_tree.DeleteItem(hChild);
        hChild = hNext;
    }
    m_greyItems.clear();

    for (size_t i = 0; i < job.sequence.size(); ++i)
    {
        const camcore::SequenceStep& step = job.sequence[i];
        std::wstring text = FromUtf8(step.name);
        if (step.type == camcore::StepType::SetOrigin && !camcore::BuildFrame(step.origin).ok)
            text += L" (incomplete)";

        const HTREEITEM hItem = m_tree.InsertItem(text.c_str(), m_hOperations);
        m_tree.SetItemData(hItem, StepItemData(StepRef{ i }));
        if (step.suppressed)
            m_greyItems.insert(hItem);

        // A folder's operations sit under it, grey if it's suppressed.
        for (size_t c = 0; c < step.children.size(); ++c)
        {
            const HTREEITEM hOperation = m_tree.InsertItem(FromUtf8(step.children[c].name).c_str(), hItem);
            m_tree.SetItemData(hOperation, StepItemData(StepRef{ i, c }));
            if (step.suppressed || step.children[c].suppressed)
                m_greyItems.insert(hOperation);
        }
        m_tree.Expand(hItem, TVE_EXPAND);
    }

    m_tree.Expand(m_hOperations, TVE_EXPAND);
    if (const HTREEITEM hSelect = FindStepItem(selected))
        m_tree.SelectItem(hSelect);

    m_fillingTree = false;
}

void CCamTabWnd::OnTreeCustomDraw(NMHDR* pNotify, LRESULT* pResult)
{
    // Suppressed steps are drawn with grey text, unless they're selected.
    NMTVCUSTOMDRAW* pDraw = reinterpret_cast<NMTVCUSTOMDRAW*>(pNotify);
    *pResult = CDRF_DODEFAULT;
    switch (pDraw->nmcd.dwDrawStage)
    {
    case CDDS_PREPAINT:
        *pResult = CDRF_NOTIFYITEMDRAW;
        break;
    case CDDS_ITEMPREPAINT:
    {
        const HTREEITEM hItem = reinterpret_cast<HTREEITEM>(pDraw->nmcd.dwItemSpec);
        if (m_greyItems.count(hItem) != 0 && (pDraw->nmcd.uItemState & CDIS_SELECTED) == 0)
            pDraw->clrText = ::GetSysColor(COLOR_GRAYTEXT);
        break;
    }
    default:
        break;
    }
}

void CCamTabWnd::PostClearSelection()
{
    PostMessage(WM_APP_CLEAR_SELECTION);
}

LRESULT CCamTabWnd::OnClearSelection(WPARAM, LPARAM)
{
    if (m_pTab != nullptr)
        m_pTab->ClearPickedSelection();
    return 0;
}

void CCamTabWnd::ShowTools(const camcore::ToolLibrary& library, const std::string& selectedToolId)
{
    m_fillingTree = true;

    HTREEITEM hChild = m_tree.GetChildItem(m_hTools);
    while (hChild != nullptr)
    {
        const HTREEITEM hNext = m_tree.GetNextSiblingItem(hChild);
        m_tree.DeleteItem(hChild);
        hChild = hNext;
    }

    for (size_t i = 0; i < library.tools.size(); ++i)
    {
        const camcore::Tool& tool = library.tools[i];
        const std::wstring text = L"T" + std::to_wstring(tool.number) + L" " + FromUtf8(tool.name);
        const HTREEITEM hItem = m_tree.InsertItem(text.c_str(), m_hTools);
        m_tree.SetItemData(hItem, static_cast<DWORD_PTR>(i));
    }

    MarkSelectedTool(library, selectedToolId);
    m_tree.Expand(m_hTools, TVE_EXPAND);

    m_fillingTree = false;
}

void CCamTabWnd::ShowMachine(const std::wstring& name)
{
    if (m_hMachine != nullptr)
        m_tree.SetItemText(m_hMachine, (L"Machine - " + name).c_str());
}

void CCamTabWnd::ShowMaterial(const std::wstring& name)
{
    if (m_hMaterial != nullptr)
        m_tree.SetItemText(m_hMaterial, (L"Material - " + name).c_str());
}

void CCamTabWnd::MarkSelectedTool(const camcore::ToolLibrary& library, const std::string& selectedToolId)
{
    for (HTREEITEM hItem = m_tree.GetChildItem(m_hTools); hItem != nullptr; hItem = m_tree.GetNextSiblingItem(hItem))
    {
        const size_t index = static_cast<size_t>(m_tree.GetItemData(hItem));
        const bool selected = index < library.tools.size() && library.tools[index].id == selectedToolId;
        m_tree.SetItemState(hItem, selected ? TVIS_BOLD : 0, TVIS_BOLD);
    }
}

LRESULT CCamTabWnd::OnLoadJob(WPARAM, LPARAM)
{
    if (m_pTab != nullptr)
        m_pTab->LoadJobFile();
    return 0;
}

void CCamTabWnd::SetStatusText(const std::wstring& text)
{
    m_status.SetWindowText(text.c_str());
}

void CCamTabWnd::EnableSave(bool enable)
{
    m_save.EnableWindow(enable ? TRUE : FALSE);
}
