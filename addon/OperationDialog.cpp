// OperationDialog.cpp : the modeless dialog that edits one operation.

#include "stdafx.h"
#include "AlibreCAM.h"
#include "OperationDialog.h"
#include "CamTabCommand.h"
#include "ModelReader.h"
#include "Log.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

using namespace camcore;

namespace
{
    // Where the dialog was last closed, so it reopens there. Empty until then.
    CPoint g_lastPosition(-1, -1);

    // The tab shown when the dialog last closed.
    int g_lastPage = 0;

    const wchar_t* OperationTitle(StepType type)
    {
        return (type == StepType::Drill) ? L"Drill" : L"Operation";
    }
}

/////////////////////////////////////////////////////////////////////////////
// COperationPage

COperationPage::COperationPage()
    : CDialog(IDD_OP_PAGE, nullptr)
{
}

bool COperationPage::CreateInTab(CWnd* pParent)
{
    return Create(IDD_OP_PAGE, pParent) != FALSE;
}

/////////////////////////////////////////////////////////////////////////////
// COperationDlg

BEGIN_MESSAGE_MAP(COperationDlg, CDialog)
    ON_NOTIFY(TCN_SELCHANGE, IDC_OP_TABS, &COperationDlg::OnTabChanged)
    ON_WM_CLOSE()
END_MESSAGE_MAP()

COperationDlg::COperationDlg(CCamTabCommand* pTab, const SequenceStep& step)
    : CDialog(IDD_OPERATION, nullptr)
    , m_pTab(pTab)
    , m_step(step)
{
}

bool COperationDlg::CreateModeless(CWnd* pParent)
{
    return Create(IDD_OPERATION, pParent) != FALSE;
}

BOOL COperationDlg::OnInitDialog()
{
    CDialog::OnInitDialog();

    SetWindowText(OperationTitle(m_step.type));
    SetDlgItemText(IDC_OP_NAME, FromUtf8(m_step.name).c_str());

    m_tabs.SubclassDlgItem(IDC_OP_TABS, this);

    // Every operation has these.
    AddPage(L"Features");
    AddPage(L"Tool");
    AddPage(L"Feeds && Speeds");
    AddPage(L"Clearance");
    AddPage(L"Sorting");

    // Then its own.
    if (m_step.type == StepType::Drill)
        AddPage(L"Drilling");

    if (g_lastPosition.x >= 0)
        SetWindowPos(nullptr, g_lastPosition.x, g_lastPosition.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    const int page = (g_lastPage < static_cast<int>(m_pages.size())) ? g_lastPage : 0;
    m_tabs.SetCurSel(page);
    ShowPage(page);
    return TRUE;
}

void COperationDlg::AddPage(const wchar_t* title)
{
    auto page = std::make_unique<COperationPage>();
    if (!page->CreateInTab(this))
    {
        Log(L"[operation] could not create the %s tab", title);
        return;
    }

    // A tab's label shows a single & as an underline, so titles double it.
    m_tabs.InsertItem(static_cast<int>(m_pages.size()), title);

    // Place the page over the tab control's display area, in front of it.
    CRect area;
    m_tabs.GetWindowRect(&area);
    ScreenToClient(&area);
    m_tabs.AdjustRect(FALSE, &area);
    page->SetWindowPos(&CWnd::wndTop, area.left, area.top, area.Width(), area.Height(), SWP_HIDEWINDOW);

    m_pages.push_back(std::move(page));
}

void COperationDlg::ShowPage(int index)
{
    for (size_t i = 0; i < m_pages.size(); ++i)
        m_pages[i]->ShowWindow(static_cast<int>(i) == index ? SW_SHOW : SW_HIDE);
}

void COperationDlg::OnTabChanged(NMHDR* /*pNotify*/, LRESULT* pResult)
{
    *pResult = 0;
    ShowPage(m_tabs.GetCurSel());
}

void COperationDlg::PostNcDestroy()
{
    delete this;
}

void COperationDlg::OnOK()
{
    CString name;
    GetDlgItemText(IDC_OP_NAME, name);
    name.Trim();
    if (name.IsEmpty())
    {
        MessageBoxW(L"The operation needs a name.", OperationTitle(m_step.type), MB_OK | MB_ICONWARNING);
        return;
    }

    m_step.name = ToUtf8(static_cast<LPCWSTR>(name));
    Finish(true);
}

void COperationDlg::OnCancel()
{
    Finish(false);
}

void COperationDlg::OnClose()
{
    Finish(false);
}

void COperationDlg::Finish(bool apply)
{
    CRect rect;
    GetWindowRect(&rect);
    g_lastPosition = rect.TopLeft();
    g_lastPage = m_tabs.GetCurSel();

    if (m_pTab != nullptr)
    {
        CCamTabCommand* pTab = m_pTab;
        m_pTab = nullptr;
        pTab->OnOperationDialogDone(this, apply);
    }
    DestroyWindow();
}
