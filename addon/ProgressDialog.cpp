// ProgressDialog.cpp : the window that shows what Generate is doing while it works.

#include "stdafx.h"
#include "CAMForAlibre.h"
#include "ProgressDialog.h"

#include <cmath>

/////////////////////////////////////////////////////////////////////////////
// CGenerateProgressDlg

CGenerateProgressDlg::CGenerateProgressDlg() : CDialog(IDD_GENERATE_PROGRESS)
{
}

bool CGenerateProgressDlg::CreateOver(HWND nearWindow)
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());
    // Owned by, and in the middle of, the top-level window the CAM tab is in.
    HWND owner = (nearWindow != nullptr && ::IsWindow(nearWindow)) ? ::GetAncestor(nearWindow, GA_ROOT) : nullptr;
    if (owner == nullptr)
        owner = theApp.m_hAlibreWnd;
    if (!Create(IDD_GENERATE_PROGRESS, CWnd::FromHandle(owner)))
        return false;
    CRect area, rect;
    if (owner == nullptr || ::IsIconic(owner) || !::GetWindowRect(owner, &area) || area.IsRectEmpty())
    {
        // Failing that, the screen the mouse is on.
        POINT cursor{};
        ::GetCursorPos(&cursor);
        MONITORINFO monitor{ sizeof(MONITORINFO) };
        if (::GetMonitorInfoW(::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor))
            area = monitor.rcWork;
    }
    GetWindowRect(&rect);
    SetWindowPos(nullptr, area.CenterPoint().x - rect.Width() / 2, area.CenterPoint().y - rect.Height() / 2, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(SW_SHOWNOACTIVATE);
    return true;
}

BOOL CGenerateProgressDlg::OnInitDialog()
{
    CDialog::OnInitDialog();
    SendDlgItemMessage(IDC_PROGRESS_BAR, PBM_SETRANGE32, 0, 1000);
    return TRUE;
}

void CGenerateProgressDlg::Show(const std::wstring& operation, const std::wstring& stage, double fraction)
{
    SetDlgItemTextW(IDC_PROGRESS_OPERATION, operation.c_str());
    SetDlgItemTextW(IDC_PROGRESS_STAGE, stage.c_str());
    CWnd* pBar = GetDlgItem(IDC_PROGRESS_BAR);
    if (fraction < 0.0)
    {
        // Not known: no bar, as a moving one needs messages handled.
        pBar->ShowWindow(SW_HIDE);
    }
    else
    {
        pBar->ShowWindow(SW_SHOWNA);
        pBar->SendMessage(PBM_SETPOS, static_cast<WPARAM>(std::lround((std::min)(1.0, fraction) * 1000.0)), 0);
    }
    // Painted now, as the message loop isn't running.
    RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
}

/////////////////////////////////////////////////////////////////////////////
// GenerateProgress

GenerateProgress::GenerateProgress(size_t operations, HWND nearWindow)
    : m_operations(operations), m_near(nearWindow), m_start(GetTickCount64())
{
}

GenerateProgress::~GenerateProgress()
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());
    if (m_pDialog != nullptr && m_pDialog->GetSafeHwnd() != nullptr)
        m_pDialog->DestroyWindow();
}

void GenerateProgress::StartOperation(size_t index, const std::wstring& name)
{
    m_operation = L"Generating " + name;
    if (m_operations > 1)
        m_operation += L" (" + std::to_wstring(index + 1) + L" of " + std::to_wstring(m_operations) + L")";
    m_stage = L"Starting";
    m_fraction = -1.0;
    Refresh(true);
}

void GenerateProgress::Update(double fraction, const std::wstring& stage)
{
    const bool newStage = stage != m_stage;
    m_stage = stage;
    m_fraction = fraction;
    Refresh(newStage);
}

void GenerateProgress::Refresh(bool force)
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());
    const ULONGLONG now = GetTickCount64();
    if (now - m_start < 500)
        return;
    if (!force && now - m_lastShown < 100)
        return;
    m_lastShown = now;
    if (m_pDialog == nullptr)
    {
        m_pDialog = std::make_unique<CGenerateProgressDlg>();
        if (!m_pDialog->CreateOver(m_near))
        {
            m_pDialog.reset();
            return;
        }
    }
    if (m_pDialog->GetSafeHwnd() == nullptr)
        return;
    m_pDialog->Show(m_operation, m_stage, m_fraction);
    // Looking at the queue tells Windows the thread is alive; nothing is taken from it.
    MSG message;
    ::PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
}
