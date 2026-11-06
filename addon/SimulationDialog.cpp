// SimulationDialog.cpp : the modeless Simulation dialog, which plays the toolpaths
// against the stock in Alibre's view.

#include "stdafx.h"
#include "AlibreCAM.h"
#include "SimulationDialog.h"
#include "CamTabCommand.h"
#include "Help.h"
#include "ModelReader.h"
#include "Log.h"

#include <cmath>

namespace
{
    const UINT_PTR kPlayTimer = 1;
    const UINT kPlayMilliseconds = 50;
    const int kSpeedSteps = 100;            // the speed slider's range, 0 to this
    const double kMostMovesPerTick = 500.0;

    CPoint g_lastPosition(-1, -1);
    int g_lastSpeed = 40;

    std::wstring WithCommas(size_t value)
    {
        std::wstring text = std::to_wstring(value);
        for (int at = static_cast<int>(text.size()) - 3; at > 0; at -= 3)
            text.insert(static_cast<size_t>(at), L",");
        return text;
    }
}

BEGIN_MESSAGE_MAP(CSimulationDlg, CDialog)
    ON_BN_CLICKED(IDC_HELP_BUTTON, &CSimulationDlg::OnHelpButton)
    ON_BN_CLICKED(IDC_SIM_REWIND, &CSimulationDlg::OnRewind)
    ON_BN_CLICKED(IDC_SIM_STEP, &CSimulationDlg::OnStep)
    ON_BN_CLICKED(IDC_SIM_PLAY, &CSimulationDlg::OnPlay)
    ON_BN_CLICKED(IDC_SIM_OPERATION_END, &CSimulationDlg::OnOperationEnd)
    ON_BN_CLICKED(IDC_SIM_END, &CSimulationDlg::OnEnd)
    ON_WM_HSCROLL()
    ON_WM_TIMER()
    ON_WM_CTLCOLOR()
    ON_WM_CLOSE()
END_MESSAGE_MAP()

void CSimulationDlg::OnHelpButton()
{
    ShowHelp(L"simulation");
}

CSimulationDlg::CSimulationDlg(CCamTabCommand* pTab)
    : CDialog(IDD_SIMULATION, nullptr)
    , m_pTab(pTab)
    , m_playing(false)
{
}

bool CSimulationDlg::CreateModeless(CWnd* pParent)
{
    return Create(IDD_SIMULATION, pParent) != FALSE;
}

BOOL CSimulationDlg::OnInitDialog()
{
    CDialog::OnInitDialog();

    m_position.SubclassDlgItem(IDC_SIM_POSITION, this);
    m_speed.SubclassDlgItem(IDC_SIM_SPEED, this);
    m_speed.SetRange(0, kSpeedSteps);
    m_speed.SetPos(g_lastSpeed);

    if (m_pTab != nullptr)
    {
        const camcore::Simulation& simulation = m_pTab->GetSimulation();
        m_position.SetRange(0, static_cast<int>((std::min)(simulation.Moves().size(), size_t(0x7FFFFFFF))));
    }

    if (g_lastPosition.x >= 0)
        SetWindowPos(nullptr, g_lastPosition.x, g_lastPosition.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowState();
    return TRUE;
}

void CSimulationDlg::PostNcDestroy()
{
    delete this;
}

size_t CSimulationDlg::MovesPerTick() const
{
    // From one move a tick at the left to the most at the right, on a log scale.
    const double fraction = static_cast<double>(m_speed.GetPos()) / kSpeedSteps;
    return static_cast<size_t>(std::lround(std::pow(kMostMovesPerTick, fraction)));
}

void CSimulationDlg::Stop()
{
    if (m_playing)
        KillTimer(kPlayTimer);
    m_playing = false;
    SetDlgItemText(IDC_SIM_PLAY, L"Play");
}

void CSimulationDlg::Changed()
{
    if (m_pTab != nullptr)
        m_pTab->OnSimulationChanged();
    ShowState();
}

void CSimulationDlg::ShowState()
{
    if (m_pTab == nullptr)
        return;
    const camcore::Simulation& simulation = m_pTab->GetSimulation();

    // Where it is: the operation, and the move of all of them.
    std::wstring status;
    const camcore::SimOperation* pOperation = simulation.CurrentOperation();
    if (pOperation != nullptr)
        status = FromUtf8(pOperation->name) + L" \u2014 ";
    status += L"move " + WithCommas(simulation.Position()) + L" of " + WithCommas(simulation.Moves().size());
    if (simulation.Finished())
        status += L" (done)";
    SetDlgItemText(IDC_SIM_STATUS, status.c_str());
    m_position.SetPos(static_cast<int>((std::min)(simulation.Position(), size_t(0x7FFFFFFF))));

    wchar_t cells[160];
    swprintf_s(cells, L"Cells %g mm (%d x %d x %d). ", simulation.Stock().Cell(), simulation.Stock().SizeX(),
               simulation.Stock().SizeY(), simulation.Stock().SizeZ());
    SetDlgItemText(IDC_SIM_INFO, (std::wstring(cells) + m_note).c_str());

    const size_t speed = MovesPerTick();
    SetDlgItemText(IDC_SIM_SPEED_TEXT, (WithCommas(speed) + (speed == 1 ? L" move a step" : L" moves a step")).c_str());

    std::wstring warning;
    if (simulation.HasWarning())
    {
        const camcore::SimMove& move = simulation.Moves()[simulation.WarningMove()];
        warning = L"Rapid into stock in " + FromUtf8(simulation.Operations()[move.operation].name) +
                  L", toolpath move " + std::to_wstring(move.toolpathMove + 1) + L".";
    }
    SetDlgItemText(IDC_SIM_WARNING, warning.c_str());

    const BOOL more = simulation.Finished() ? FALSE : TRUE;
    GetDlgItem(IDC_SIM_STEP)->EnableWindow(more);
    GetDlgItem(IDC_SIM_OPERATION_END)->EnableWindow(more);
    GetDlgItem(IDC_SIM_END)->EnableWindow(more);
    GetDlgItem(IDC_SIM_PLAY)->EnableWindow(more || m_playing);
}

void CSimulationDlg::OnRewind()
{
    Stop();
    if (m_pTab == nullptr)
        return;
    m_pTab->GetSimulation().Rewind();
    Changed();
}

void CSimulationDlg::OnStep()
{
    Stop();
    if (m_pTab == nullptr)
        return;
    m_pTab->GetSimulation().ClearWarning();
    m_pTab->GetSimulation().Step(1);
    Changed();
}

void CSimulationDlg::OnOperationEnd()
{
    Stop();
    if (m_pTab == nullptr)
        return;
    CWaitCursor wait;
    m_pTab->GetSimulation().ClearWarning();
    m_pTab->GetSimulation().RunToOperationEnd();
    Changed();
}

void CSimulationDlg::OnEnd()
{
    Stop();
    if (m_pTab == nullptr)
        return;
    CWaitCursor wait;
    m_pTab->GetSimulation().ClearWarning();
    m_pTab->GetSimulation().RunToEnd();
    Changed();
}

void CSimulationDlg::OnPlay()
{
    if (m_playing)
    {
        Stop();
        ShowState();
        return;
    }
    if (m_pTab == nullptr || m_pTab->GetSimulation().Finished())
        return;
    m_pTab->GetSimulation().ClearWarning();
    m_playing = true;
    SetDlgItemText(IDC_SIM_PLAY, L"Pause");
    SetTimer(kPlayTimer, kPlayMilliseconds, nullptr);
}

void CSimulationDlg::OnTimer(UINT_PTR id)
{
    if (id != kPlayTimer)
    {
        CDialog::OnTimer(id);
        return;
    }
    if (m_pTab == nullptr)
    {
        Stop();
        return;
    }
    camcore::Simulation& simulation = m_pTab->GetSimulation();
    simulation.Step(MovesPerTick());
    if (simulation.Finished() || simulation.HasWarning())
        Stop();
    Changed();
}

void CSimulationDlg::OnHScroll(UINT code, UINT position, CScrollBar* pScrollBar)
{
    CDialog::OnHScroll(code, position, pScrollBar);
    if (pScrollBar == nullptr || m_pTab == nullptr)
        return;

    if (pScrollBar->GetSafeHwnd() == m_speed.GetSafeHwnd())
    {
        g_lastSpeed = m_speed.GetPos();
        ShowState();
        return;
    }

    // The scrub bar seeks when it's let go, not while it's dragged: seeking back
    // replays from the start.
    if (pScrollBar->GetSafeHwnd() == m_position.GetSafeHwnd() && code != TB_THUMBTRACK)
    {
        Stop();
        CWaitCursor wait;
        m_pTab->GetSimulation().SeekTo(static_cast<size_t>((std::max)(0, m_position.GetPos())));
        Changed();
    }
}

HBRUSH CSimulationDlg::OnCtlColor(CDC* pDC, CWnd* pWnd, UINT ctlColor)
{
    HBRUSH brush = CDialog::OnCtlColor(pDC, pWnd, ctlColor);
    if (pWnd != nullptr && pWnd->GetDlgCtrlID() == IDC_SIM_WARNING)
        pDC->SetTextColor(RGB(200, 0, 0));
    return brush;
}

void CSimulationDlg::OnOK()
{
    Finish();
}

void CSimulationDlg::OnCancel()
{
    Finish();
}

void CSimulationDlg::OnClose()
{
    Finish();
}

void CSimulationDlg::Finish()
{
    Stop();
    CRect rect;
    GetWindowRect(&rect);
    g_lastPosition = rect.TopLeft();
    if (m_pTab != nullptr)
    {
        CCamTabCommand* pTab = m_pTab;
        m_pTab = nullptr;
        pTab->OnSimulationDialogDone(this);
    }
    DestroyWindow();
}
