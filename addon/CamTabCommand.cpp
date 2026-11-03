// CamTabCommand.cpp : the two-way toggle command that owns the CAM tab.

#include "stdafx.h"
#include "AlibreCAM.h"
#include "AddOnInterface.h"
#include "CamTabCommand.h"
#include "DisplaySettings.h"
#include "GeometryReader.h"
#include "ModelReader.h"
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

        // Tree menu commands
        ID_CAM_ADD_ORIGIN,
        ID_CAM_ADD_FOLDER,
        ID_CAM_ADD_DRILL,
        ID_CAM_ADD_FACING,
        ID_CAM_EDIT_STEP,
        ID_CAM_RENAME_STEP,
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
    LeaveAddOn();
}

/////////////////////////////////////////////////////////////////////////////
// Loading and saving the .cam file

fs::path CCamTabCommand::CurrentLibraryPath() const
{
    return m_job.toolLibraryPath.empty() ? DefaultToolLibraryPath()
                                         : camcore::ResolveLibraryPath(m_camPath, m_job.toolLibraryPath);
}

void CCamTabCommand::ReloadTools()
{
    const fs::path path = CurrentLibraryPath();
    const camcore::ToolLibraryLoadResult loaded = camcore::LoadToolLibrary(path);

    m_library = camcore::ToolLibrary();
    if (loaded.status == camcore::LoadStatus::Ok)
    {
        m_library = loaded.library;

        if (m_library.tools.empty())
        {
            m_toolStatus = L"The tool library is empty. Double-click Tools to add one.";
        }
        else if (m_job.selectedToolId.empty())
        {
            m_toolStatus = L"No tool selected.";
        }
        else if (const camcore::Tool* pTool = camcore::FindToolById(m_library, m_job.selectedToolId))
        {
            m_toolStatus = L"Selected tool: T" + std::to_wstring(pTool->number) + L" " + FromUtf8(pTool->name);
        }
        else
        {
            m_toolStatus = L"The selected tool isn't in this library. Select another.";
        }
    }
    else if (loaded.status == camcore::LoadStatus::NotFound)
    {
        // A new default library is expected. A job's own library should exist.
        m_toolStatus = m_job.toolLibraryPath.empty()
                           ? L"The tool library is empty. Double-click Tools to add one."
                           : L"This job's tool library is missing: " + path.wstring();
    }
    else
    {
        m_toolStatus = L"The tool library can't be used. " + FromUtf8(loaded.message);
    }

    Log(L"[tools] library '%s': %zu tools. %s", path.c_str(), m_library.tools.size(), m_toolStatus.c_str());

    if (m_pWnd != nullptr)
        m_pWnd->ShowTools(m_library, m_job.selectedToolId);

    SetStatus(m_jobStatus);
}

void CCamTabCommand::OpenToolLibrary()
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    if (m_saveBlocked)
        return;

    const fs::path before = CurrentLibraryPath();

    CToolLibraryDlg dialog(before, m_job.selectedToolId, m_pWnd);
    dialog.DoModal();

    const fs::path after = dialog.GetLibraryPath();
    if (after != before)
    {
        m_job.toolLibraryPath = (after == DefaultToolLibraryPath())
                                    ? std::string()
                                    : camcore::StoredLibraryPath(m_camPath, after);
        Log(L"[tools] job library is now '%s'", after.c_str());
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

    // A library kept beside the job must still resolve from the new location.
    if (target != m_camPath && !m_job.toolLibraryPath.empty())
    {
        const fs::path library = camcore::ResolveLibraryPath(m_camPath, m_job.toolLibraryPath);
        m_job.toolLibraryPath = camcore::StoredLibraryPath(target, library);
    }

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
// The operation sequence

bool CCamTabCommand::CanEditSequence() const
{
    return !m_saveBlocked;
}

bool CCamTabCommand::IsDialogOpen() const
{
    return m_pOriginDlg != nullptr || m_pOperationDlg != nullptr;
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

    if (m_pWnd != nullptr)
        m_pWnd->ShowSequence(m_job, m_selected);
}

bool CCamTabCommand::FocusOpenDialog()
{
    if (m_pOriginDlg != nullptr)
        m_pOriginDlg->SetForegroundWindow();
    else if (m_pOperationDlg != nullptr)
        m_pOperationDlg->SetForegroundWindow();
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
    operation.name = NewOperationName(m_job, type == camcore::StepType::Facing ? "Facing" : "Drill");

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

    const bool originChanges = (ref.step != m_selected.step);
    m_selected = ref;
    if (originChanges)
        RebuildScene();
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

    if (!apply || !CanEditSequence() || !IsFolder(StepRef{ m_operationTarget.step }))
        return;

    std::vector<camcore::SequenceStep>& children = m_job.sequence[m_operationTarget.step].children;
    const camcore::SequenceStep& step = pDialog->GetStep();
    if (m_operationTarget.child == kNewStep)
    {
        children.push_back(step);
        m_selected = StepRef{ m_operationTarget.step, children.size() - 1 };
    }
    else if (m_operationTarget.child < children.size())
    {
        children[m_operationTarget.child] = step;
        m_selected = m_operationTarget;
    }
    else
    {
        return;
    }

    Log(L"[operation] applied %s", FromUtf8(step.name).c_str());
    MarkJobChanged();
    ShowSequence();
    RebuildScene();
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

void CCamTabCommand::ReadModelSize()
{
    try
    {
        IADSessionPtr session = GetSession();
        IADDesignSessionPtr design;
        session->QueryInterface(__uuidof(IADDesignSession), reinterpret_cast<void**>(&design));
        m_scene.millimetresPerUnit = MillimetresPerModelUnit(design);
        m_haveBounds = ReadAssemblyBounds(session, m_boundsLower, m_boundsUpper);
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
    return (m_pOriginDlg != nullptr && m_pOriginDlg->GetPickSlot() != PickSlot::None) ||
           (m_pOperationDlg != nullptr && (m_pOperationDlg->IsLocating() || m_pOperationDlg->IsSelectingFeatures()));
}

void CCamTabCommand::BeginPick(PickSlot slot)
{
    SelectionFilter wanted;
    wanted.solid = (slot == PickSlot::Plane) ? ADSelectionFilterOption_AD_FACES_EDGES : ADSelectionFilterOption_AD_EDGES;
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
        const PickFilter filter = (slot == PickSlot::Plane) ? PickFilter::FacesAndEdges : PickFilter::Edges;
        read = ReadSelectedPick(GetSession(), filter, pick, problem);
    }
    catch (const _com_error& error)
    {
        Log(L"[pick] could not find the session: hr=0x%08X", static_cast<unsigned>(error.Error()));
    }

    // Clearing the selection calls this again with nothing selected.
    if (!read && problem.empty())
        return;

    if (read && slot == PickSlot::XDirection && pick.kind != camcore::PickKind::LineEdge)
    {
        read = false;
        problem = L"The X direction needs a straight edge.";
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
            const double depth = camcore::DistanceAlongZ(frame.frame, located);
            Log(L"[locate] depth %.4f mm from %s", depth, FromUtf8(m_job.sequence[active].name).c_str());
            m_pOperationDlg->SetLocatedDepth(depth);
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
    ON_NOTIFY(TVN_SELCHANGED, IDC_CAM_TREE, &CCamTabWnd::OnTreeSelectionChanged)
    ON_NOTIFY(NM_DBLCLK, IDC_CAM_TREE, &CCamTabWnd::OnTreeDoubleClick)
    ON_NOTIFY(NM_RCLICK, IDC_CAM_TREE, &CCamTabWnd::OnTreeRightClick)
    ON_NOTIFY(TVN_BEGINLABELEDIT, IDC_CAM_TREE, &CCamTabWnd::OnTreeBeginRename)
    ON_NOTIFY(TVN_ENDLABELEDIT, IDC_CAM_TREE, &CCamTabWnd::OnTreeEndRename)
    ON_NOTIFY(TVN_BEGINDRAG, IDC_CAM_TREE, &CCamTabWnd::OnTreeBeginDrag)
    ON_WM_MOUSEMOVE()
    ON_WM_LBUTTONUP()
    ON_WM_RBUTTONDOWN()
    ON_WM_CAPTURECHANGED()
    ON_MESSAGE(WM_APP_LOAD_JOB, &CCamTabWnd::OnLoadJob)
    ON_MESSAGE(WM_APP_CLEAR_SELECTION, &CCamTabWnd::OnClearSelection)
END_MESSAGE_MAP()

CCamTabWnd::CCamTabWnd(CCamTabCommand* pTab)
    : m_pTab(pTab)
    , m_hTools(nullptr)
    , m_hOperations(nullptr)
    , m_fillingTree(false)
    , m_visible(false)
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
    m_tree.InsertItem(L"Machine", hSetup);
    m_hTools = m_tree.InsertItem(L"Tools", hSetup);
    m_tree.InsertItem(L"Stock", hSetup);
    m_hOperations = m_tree.InsertItem(L"Operations");
    m_tree.Expand(hSetup, TVE_EXPAND);

    m_status.Create(L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, CRect(0, 0, 10, 10), this, IDC_CAM_STATUS);
    m_status.SetFont(pFont);

    m_refresh.Create(L"Refresh view", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, CRect(0, 0, 10, 10), this,
                     IDC_CAM_REFRESH);
    m_refresh.SetFont(pFont);

    m_foreground.Create(L"Draw over model", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                        CRect(0, 0, 10, 10), this, IDC_CAM_FOREGROUND);
    m_foreground.SetFont(pFont);
    m_foreground.SetCheck(BST_CHECKED);

    m_save.Create(L"Save", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, CRect(0, 0, 10, 10), this, IDC_CAM_SAVE);
    m_save.SetFont(pFont);

    CRect client;
    GetClientRect(&client);
    LayoutControls(client.Width(), client.Height());

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

    const int margin = 4;
    const int buttonHeight = 23;
    const int checkHeight = 18;
    const int statusHeight = 60;
    const int width = (cx > margin * 2) ? cx - margin * 2 : 1;

    const int saveY = cy - margin - buttonHeight;
    const int refreshY = saveY - margin - buttonHeight;
    const int foregroundY = refreshY - margin - checkHeight;
    const int statusY = foregroundY - margin - statusHeight;
    const int treeHeight = statusY - margin * 2;

    m_tree.MoveWindow(margin, margin, width, (treeHeight > 40) ? treeHeight : 40);
    m_status.MoveWindow(margin, statusY, width, statusHeight);
    m_foreground.MoveWindow(margin, foregroundY, width, checkHeight);
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
        m_pTab->SetForeground(m_foreground.GetCheck() == BST_CHECKED);
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
    const bool onStep = IsStepItem(hItem);
    if (hItem != m_hOperations && !onStep)
        return;

    const StepRef ref = StepOfItem(hItem);
    const UINT editable = m_pTab->CanEditSequence() ? MF_ENABLED : MF_GRAYED;
    const UINT renamable = m_pTab->CanRename(ref) ? MF_ENABLED : MF_GRAYED;
    const UINT deletable = m_pTab->CanDelete(ref) ? MF_ENABLED : MF_GRAYED;

    CMenu menu;
    menu.CreatePopupMenu();
    CMenu holes;
    CMenu twoAndAHalfAxis;
    if (onStep && m_pTab->IsFolder(ref))
    {
        menu.AppendMenu(MF_STRING | renamable, ID_CAM_RENAME_STEP, L"Rename");
        menu.AppendMenu(MF_STRING | deletable, ID_CAM_DELETE_STEP, L"Delete");
        menu.AppendMenu(MF_SEPARATOR);

        holes.CreatePopupMenu();
        holes.AppendMenu(MF_STRING | editable, ID_CAM_ADD_DRILL, L"Drill...");
        menu.AppendMenu(MF_POPUP | editable, reinterpret_cast<UINT_PTR>(holes.GetSafeHmenu()), L"Add Holes");

        twoAndAHalfAxis.CreatePopupMenu();
        twoAndAHalfAxis.AppendMenu(MF_STRING | editable, ID_CAM_ADD_FACING, L"Facing...");
        menu.AppendMenu(MF_POPUP | editable, reinterpret_cast<UINT_PTR>(twoAndAHalfAxis.GetSafeHmenu()),
                        L"Add 2 1/2 Axis");
    }
    else if (onStep && m_pTab->IsOperation(ref))
    {
        menu.AppendMenu(MF_STRING | editable, ID_CAM_EDIT_STEP, L"Edit...");
        menu.AppendMenu(MF_STRING | renamable, ID_CAM_RENAME_STEP, L"Rename");
        menu.AppendMenu(MF_STRING | deletable, ID_CAM_DELETE_STEP, L"Delete");
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
    case ID_CAM_ADD_FACING:  m_pTab->AddOperation(ref.step, camcore::StepType::Facing); break;
    case ID_CAM_EDIT_STEP:   m_pTab->EditStep(ref); break;
    case ID_CAM_RENAME_STEP: m_tree.EditLabel(hItem); break;
    case ID_CAM_DELETE_STEP: m_pTab->DeleteStep(ref); break;
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
        m_pTab->OpenToolLibrary();
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

    for (size_t i = 0; i < job.sequence.size(); ++i)
    {
        const camcore::SequenceStep& step = job.sequence[i];
        std::wstring text = FromUtf8(step.name);
        if (step.type == camcore::StepType::SetOrigin && !camcore::BuildFrame(step.origin).ok)
            text += L" (incomplete)";

        const HTREEITEM hItem = m_tree.InsertItem(text.c_str(), m_hOperations);
        m_tree.SetItemData(hItem, StepItemData(StepRef{ i }));

        // A folder's operations sit under it.
        for (size_t c = 0; c < step.children.size(); ++c)
        {
            const HTREEITEM hOperation = m_tree.InsertItem(FromUtf8(step.children[c].name).c_str(), hItem);
            m_tree.SetItemData(hOperation, StepItemData(StepRef{ i, c }));
        }
        m_tree.Expand(hItem, TVE_EXPAND);
    }

    m_tree.Expand(m_hOperations, TVE_EXPAND);
    if (const HTREEITEM hSelect = FindStepItem(selected))
        m_tree.SelectItem(hSelect);

    m_fillingTree = false;
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
