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

    std::wstring FormatNumber(double value)
    {
        wchar_t text[64] = {};
        _snwprintf_s(text, _countof(text), _TRUNCATE, L"%.10g", value);
        return text;
    }

    void AppendProblem(std::wstring& problems, const std::wstring& problem)
    {
        if (!problems.empty())
            problems += L"\n";
        problems += problem;
    }
}

/////////////////////////////////////////////////////////////////////////////
// COperationPage

COperationPage::COperationPage(UINT templateId, COperationDlg* pOwner)
    : CDialog(templateId, nullptr)
    , m_pOwner(pOwner)
    , m_templateId(templateId)
{
}

bool COperationPage::CreateInTab(CWnd* pParent)
{
    return Create(m_templateId, pParent) != FALSE;
}

bool COperationPage::ReadNumber(int id, const wchar_t* label, double& value, std::wstring& problems,
                                bool blankAllowed, bool* pBlank)
{
    CString text;
    GetDlgItemText(id, text);
    text.Trim();

    if (pBlank != nullptr)
        *pBlank = text.IsEmpty();
    if (text.IsEmpty() && blankAllowed)
        return false;

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

void COperationPage::SetNumber(int id, double value)
{
    SetDlgItemText(id, FormatNumber(value).c_str());
}

/////////////////////////////////////////////////////////////////////////////
// CDrillingPage

BEGIN_MESSAGE_MAP(CDrillingPage, COperationPage)
    ON_BN_CLICKED(IDC_DRILL_LOCATE, &CDrillingPage::OnLocate)
    ON_BN_CLICKED(IDC_DWELL_OFF, &CDrillingPage::OnDwellChanged)
    ON_BN_CLICKED(IDC_DWELL_TIME, &CDrillingPage::OnDwellChanged)
    ON_BN_CLICKED(IDC_DWELL_REVOLUTIONS, &CDrillingPage::OnDwellChanged)
END_MESSAGE_MAP()

CDrillingPage::CDrillingPage(COperationDlg* pOwner)
    : COperationPage(IDD_OP_DRILLING, pOwner)
{
}

BOOL CDrillingPage::OnInitDialog()
{
    COperationPage::OnInitDialog();
    ShowLocating(false);
    return TRUE;
}

void CDrillingPage::Load(const SequenceStep& step)
{
    const DrillSettings& drill = step.drill;

    SetNumber(IDC_DRILL_DEPTH, drill.depth);
    CheckDlgButton(IDC_DRILL_ADD_TIP, drill.addTipToDepth ? BST_CHECKED : BST_UNCHECKED);

    CheckRadioButton(IDC_DRILL_AT_TOP, IDC_DRILL_AT_BOTTOM,
                     drill.location == GeometryLocation::AtBottom ? IDC_DRILL_AT_BOTTOM : IDC_DRILL_AT_TOP);

    const int dwell = (drill.dwell == DwellMode::Seconds)       ? IDC_DWELL_TIME
                      : (drill.dwell == DwellMode::Revolutions) ? IDC_DWELL_REVOLUTIONS
                                                                : IDC_DWELL_OFF;
    CheckRadioButton(IDC_DWELL_OFF, IDC_DWELL_REVOLUTIONS, dwell);
    SetNumber(IDC_DWELL_SECONDS, drill.dwellSeconds);
    SetNumber(IDC_DWELL_REVOLUTION_COUNT, drill.dwellRevolutions);

    SetNumber(IDC_DRILL_APPROACH, drill.approachDistance);

    if (drill.pecking)
        SetNumber(IDC_DRILL_PECK, drill.peckDepth);
    else
        SetDlgItemText(IDC_DRILL_PECK, L"");

    UpdateDwellFields();
}

bool CDrillingPage::Save(SequenceStep& step, std::wstring& problems)
{
    DrillSettings drill = step.drill;
    std::wstring found;

    ReadNumber(IDC_DRILL_DEPTH, L"The depth", drill.depth, found);
    drill.addTipToDepth = IsDlgButtonChecked(IDC_DRILL_ADD_TIP) == BST_CHECKED;
    drill.location = (GetCheckedRadioButton(IDC_DRILL_AT_TOP, IDC_DRILL_AT_BOTTOM) == IDC_DRILL_AT_BOTTOM)
                         ? GeometryLocation::AtBottom
                         : GeometryLocation::AtTop;

    switch (GetCheckedRadioButton(IDC_DWELL_OFF, IDC_DWELL_REVOLUTIONS))
    {
    case IDC_DWELL_TIME:
        drill.dwell = DwellMode::Seconds;
        ReadNumber(IDC_DWELL_SECONDS, L"The dwell time", drill.dwellSeconds, found);
        break;
    case IDC_DWELL_REVOLUTIONS:
        drill.dwell = DwellMode::Revolutions;
        ReadNumber(IDC_DWELL_REVOLUTION_COUNT, L"The dwell revolutions", drill.dwellRevolutions, found);
        break;
    default:
        drill.dwell = DwellMode::Off;
        break;
    }

    ReadNumber(IDC_DRILL_APPROACH, L"The approach distance", drill.approachDistance, found);

    bool blank = false;
    if (ReadNumber(IDC_DRILL_PECK, L"The peck depth", drill.peckDepth, found, true, &blank))
        drill.pecking = true;
    else if (blank)
        drill.pecking = false;

    // Range checks only make sense once every number has been read.
    if (found.empty())
    {
        for (const std::string& problem : ValidateDrillSettings(drill))
            AppendProblem(found, FromUtf8(problem));
    }

    if (!found.empty())
    {
        AppendProblem(problems, found);
        return false;
    }

    step.drill = drill;
    return true;
}

void CDrillingPage::UpdateDwellFields()
{
    const int dwell = GetCheckedRadioButton(IDC_DWELL_OFF, IDC_DWELL_REVOLUTIONS);
    GetDlgItem(IDC_DWELL_SECONDS)->EnableWindow(dwell == IDC_DWELL_TIME);
    GetDlgItem(IDC_DWELL_REVOLUTION_COUNT)->EnableWindow(dwell == IDC_DWELL_REVOLUTIONS);
}

void CDrillingPage::OnDwellChanged()
{
    UpdateDwellFields();
}

void CDrillingPage::OnLocate()
{
    if (m_pOwner->IsLocating())
        m_pOwner->StopLocating();
    else
        m_pOwner->StartLocating();
}

void CDrillingPage::ShowLocating(bool locating)
{
    SetDlgItemText(IDC_DRILL_LOCATE, locating ? L"Stop" : L"Locate");
    ShowLocateNote(locating ? L"Click a point, plane, face or edge to measure its distance from the origin along Z."
                            : L"");
}

void CDrillingPage::SetDepth(double depth)
{
    SetNumber(IDC_DRILL_DEPTH, depth);
}

void CDrillingPage::ShowLocateNote(const std::wstring& text)
{
    SetDlgItemText(IDC_DRILL_LOCATE_NOTE, text.c_str());
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
    , m_pDrilling(nullptr)
    , m_locating(false)
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

    // Every operation has these. They're empty until each is built.
    AddPage(L"Features", std::make_unique<COperationPage>(IDD_OP_PAGE, this));
    AddPage(L"Tool", std::make_unique<COperationPage>(IDD_OP_PAGE, this));
    AddPage(L"Feeds && Speeds", std::make_unique<COperationPage>(IDD_OP_PAGE, this));
    AddPage(L"Clearance", std::make_unique<COperationPage>(IDD_OP_PAGE, this));
    AddPage(L"Sorting", std::make_unique<COperationPage>(IDD_OP_PAGE, this));

    // Then its own.
    if (m_step.type == StepType::Drill)
    {
        auto drilling = std::make_unique<CDrillingPage>(this);
        CDrillingPage* pDrilling = drilling.get();
        AddPage(L"Drilling", std::move(drilling));
        if (pDrilling->GetSafeHwnd() != nullptr)
            m_pDrilling = pDrilling;
    }

    for (const auto& page : m_pages)
        page->Load(m_step);

    if (g_lastPosition.x >= 0)
        SetWindowPos(nullptr, g_lastPosition.x, g_lastPosition.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

    const int page = (g_lastPage < static_cast<int>(m_pages.size())) ? g_lastPage : 0;
    m_tabs.SetCurSel(page);
    ShowPage(page);
    return TRUE;
}

void COperationDlg::AddPage(const wchar_t* title, std::unique_ptr<COperationPage> page)
{
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

void COperationDlg::StartLocating()
{
    if (m_pDrilling == nullptr || m_pTab == nullptr)
        return;

    m_locating = true;
    m_pDrilling->ShowLocating(true);
    m_pTab->BeginLocate();
}

void COperationDlg::StopLocating()
{
    if (!m_locating)
        return;

    m_locating = false;
    if (m_pDrilling != nullptr)
        m_pDrilling->ShowLocating(false);
    if (m_pTab != nullptr)
        m_pTab->EndLocate();
}

void COperationDlg::SetLocatedDepth(double depth)
{
    StopLocating();
    if (m_pDrilling != nullptr)
    {
        m_pDrilling->SetDepth(depth);
        m_pDrilling->ShowLocateNote(depth > 0.0 ? L"The depth was measured from the picked item."
                                                : L"That item measures as 0. It's on the origin's XY plane, runs "
                                                  L"along the drill direction, or is too far away.");
    }
}

void COperationDlg::ShowLocateProblem(const std::wstring& problem)
{
    if (m_pDrilling != nullptr)
        m_pDrilling->ShowLocateNote(problem);
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

    std::wstring problems;
    if (name.IsEmpty())
        AppendProblem(problems, L"The operation needs a name.");

    // Each tab reads its own controls. The first tab with a problem is shown.
    SequenceStep step = m_step;
    int firstBad = -1;
    for (size_t i = 0; i < m_pages.size(); ++i)
    {
        if (!m_pages[i]->Save(step, problems) && firstBad < 0)
            firstBad = static_cast<int>(i);
    }

    if (!problems.empty())
    {
        if (firstBad >= 0)
        {
            m_tabs.SetCurSel(firstBad);
            ShowPage(firstBad);
        }
        MessageBoxW(problems.c_str(), OperationTitle(m_step.type), MB_OK | MB_ICONWARNING);
        return;
    }

    step.name = ToUtf8(static_cast<LPCWSTR>(name));
    m_step = step;
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

    m_locating = false;
    if (m_pTab != nullptr)
    {
        CCamTabCommand* pTab = m_pTab;
        m_pTab = nullptr;
        pTab->OnOperationDialogDone(this, apply);
    }
    DestroyWindow();
}
