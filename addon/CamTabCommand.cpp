// CamTabCommand.cpp : the two-way toggle command that owns the CAM tab.

#include "stdafx.h"
#include "AlibreCAM.h"
#include "AddOnInterface.h"
#include "CamTabCommand.h"
#include "ModelReader.h"
#include "ToolDialogs.h"
#include "Log.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace fs = std::filesystem;

namespace
{
    const UINT_PTR kResizeTimer = 1;
    const UINT kResizeMilliseconds = 250;
    const UINT WM_APP_LOAD_JOB = WM_APP + 1;

    enum
    {
        IDC_CAM_TREE = 14001,
        IDC_CAM_STATUS,
        IDC_CAM_SAVE
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
    if (m_pWnd != nullptr)
        m_pWnd->SetStatusText(m_toolStatus.empty() ? m_jobStatus : m_jobStatus + L"\n" + m_toolStatus);
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
    Log(L"[tab] window destroyed by Alibre");
    m_pWnd = nullptr;
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
// CCamTabWnd

BEGIN_MESSAGE_MAP(CCamTabWnd, CWnd)
    ON_WM_CREATE()
    ON_WM_DESTROY()
    ON_WM_SIZE()
    ON_WM_TIMER()
    ON_BN_CLICKED(IDC_CAM_SAVE, &CCamTabWnd::OnSave)
    ON_NOTIFY(TVN_SELCHANGED, IDC_CAM_TREE, &CCamTabWnd::OnTreeSelectionChanged)
    ON_NOTIFY(NM_DBLCLK, IDC_CAM_TREE, &CCamTabWnd::OnTreeDoubleClick)
    ON_MESSAGE(WM_APP_LOAD_JOB, &CCamTabWnd::OnLoadJob)
END_MESSAGE_MAP()

CCamTabWnd::CCamTabWnd(CCamTabCommand* pTab)
    : m_pTab(pTab)
    , m_hTools(nullptr)
    , m_fillingTree(false)
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

    m_tree.Create(WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES |
                      TVS_LINESATROOT | TVS_SHOWSELALWAYS,
                  CRect(0, 0, 10, 10), this, IDC_CAM_TREE);
    m_tree.SetFont(pFont);

    HTREEITEM hSetup = m_tree.InsertItem(L"Setup");
    m_tree.InsertItem(L"Machine", hSetup);
    m_hTools = m_tree.InsertItem(L"Tools", hSetup);
    m_tree.InsertItem(L"Stock", hSetup);
    m_tree.InsertItem(L"Operations");
    m_tree.Expand(hSetup, TVE_EXPAND);

    m_status.Create(L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, CRect(0, 0, 10, 10), this, IDC_CAM_STATUS);
    m_status.SetFont(pFont);

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
    const int statusHeight = 60;
    const int width = (cx > margin * 2) ? cx - margin * 2 : 1;

    const int buttonY = cy - margin - buttonHeight;
    const int statusY = buttonY - margin - statusHeight;
    const int treeHeight = statusY - margin * 2;

    m_tree.MoveWindow(margin, margin, width, (treeHeight > 40) ? treeHeight : 40);
    m_status.MoveWindow(margin, statusY, width, statusHeight);
    m_save.MoveWindow(margin, buttonY, width, buttonHeight);
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
}

void CCamTabWnd::OnSave()
{
    if (m_pTab != nullptr)
        m_pTab->SaveJobFile();
}

void CCamTabWnd::OnTreeSelectionChanged(NMHDR* /*pNotify*/, LRESULT* pResult)
{
    *pResult = 0;

    if (m_fillingTree || m_pTab == nullptr)
        return;

    const HTREEITEM hItem = m_tree.GetSelectedItem();
    if (hItem != nullptr && m_tree.GetParentItem(hItem) == m_hTools)
        m_pTab->SelectToolByIndex(static_cast<size_t>(m_tree.GetItemData(hItem)));
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
    if (hItem != nullptr && (hItem == m_hTools || m_tree.GetParentItem(hItem) == m_hTools))
    {
        // Handled, so the tree doesn't also expand or collapse the item.
        *pResult = 1;
        m_pTab->OpenToolLibrary();
    }
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
