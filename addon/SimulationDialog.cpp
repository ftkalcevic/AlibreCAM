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
    const UINT_PTR kRunTimer = 2;           // To Op End and To End, a tenth of a second of moves a tick
    const UINT kRunMilliseconds = 1;
    const ULONGLONG kRunSliceMilliseconds = 100;
    const int kSpeedSteps = 100;            // the speed slider's range, 0 to this
    const double kMostMovesPerTick = 500.0;

    CPoint g_lastPosition(-1, -1);
    int g_lastSpeed = 40;
    bool g_realTime = false;
    int g_realTimeSpeed = 50;       // the real time slider, 0 to kSpeedSteps: 50 is 1 times
    const ULONGLONG kLongestTick = 250;     // real time: a slow tick counts as no more than this
    bool g_autoDivide = true;
    std::wstring g_stlFolder;       // where the last STL was saved this session
    CCamTabCommand::SimulationShow g_show;      // what's shown, remembered until Alibre closes
    int g_resolutionStop = -1;      // the slider's stop, fine to coarse; -1 is the middle

    int StopIndex(const std::vector<double>& stops)
    {
        if (stops.empty())
            return 0;
        if (g_resolutionStop < 0)
            return static_cast<int>(stops.size() / 2);
        return (std::min)(g_resolutionStop, static_cast<int>(stops.size()) - 1);
    }

    // Machining time as m:ss, or h:mm:ss from an hour.
    std::wstring TimeText(double seconds)
    {
        const long long whole = static_cast<long long>(std::llround((std::max)(0.0, seconds)));
        wchar_t text[32];
        if (whole >= 3600)
            swprintf_s(text, L"%lld:%02lld:%02lld", whole / 3600, (whole / 60) % 60, whole % 60);
        else
            swprintf_s(text, L"%lld:%02lld", whole / 60, whole % 60);
        return text;
    }

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
    ON_BN_CLICKED(IDC_SIM_AUTO_DIVIDE, &CSimulationDlg::OnAutoDivide)
    ON_BN_CLICKED(IDC_SIM_SAVE_STL, &CSimulationDlg::OnSaveStl)
    ON_BN_CLICKED(IDC_SIM_SHOW_STOCK, &CSimulationDlg::OnShowChanged)
    ON_BN_CLICKED(IDC_SIM_SHOW_PART, &CSimulationDlg::OnShowChanged)
    ON_BN_CLICKED(IDC_SIM_SHOW_NOT_ENOUGH, &CSimulationDlg::OnShowChanged)
    ON_BN_CLICKED(IDC_SIM_SHOW_TOO_MUCH, &CSimulationDlg::OnShowChanged)
    ON_BN_CLICKED(IDC_SIM_REALTIME, &CSimulationDlg::OnRealTimeChanged)
    ON_WM_HSCROLL()
    ON_WM_TIMER()
    ON_WM_CTLCOLOR()
    ON_WM_CLOSE()
END_MESSAGE_MAP()

void CSimulationDlg::OnHelpButton()
{
    ShowHelp(L"simulation");
}

camcore::SimResolution CSimulationDlg::ChosenResolution(const std::vector<double>& stops)
{
    camcore::SimResolution resolution;
    resolution.autoDivide = g_autoDivide;
    if (!g_autoDivide && !stops.empty())
        resolution.cell = stops[static_cast<size_t>(StopIndex(stops))];
    return resolution;
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
    m_realTimeSpeed.SubclassDlgItem(IDC_SIM_REALTIME_SPEED, this);
    m_realTimeSpeed.SetRange(0, kSpeedSteps);
    m_realTimeSpeed.SetPos(g_realTimeSpeed);
    CheckDlgButton(IDC_SIM_REALTIME, g_realTime ? BST_CHECKED : BST_UNCHECKED);

    // Resolution: Auto divide, or a stop from fine to coarse.
    m_resolution.SubclassDlgItem(IDC_SIM_RESOLUTION, this);
    if (m_pTab != nullptr)
        m_stops = m_pTab->SimulationStops();
    m_resolution.SetRange(0, (std::max)(1, static_cast<int>(m_stops.size()) - 1));
    m_resolution.SetTicFreq(1);
    m_appliedStop = StopIndex(m_stops);
    m_resolution.SetPos(m_appliedStop);
    CheckDlgButton(IDC_SIM_AUTO_DIVIDE, g_autoDivide ? BST_CHECKED : BST_UNCHECKED);
    ShowResolution();

    // What's shown.
    CheckDlgButton(IDC_SIM_SHOW_STOCK, g_show.stock ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_SIM_SHOW_PART, g_show.part ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_SIM_SHOW_NOT_ENOUGH, g_show.notEnough ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_SIM_SHOW_TOO_MUCH, g_show.tooMuch ? BST_CHECKED : BST_UNCHECKED);
    if (m_pTab != nullptr)
        m_pTab->SetSimulationShow(g_show);

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

double CSimulationDlg::RealTimeFactor() const
{
    // 0.1 times at the left, 1 in the middle, 10 at the right, on a log scale.
    const double fraction = static_cast<double>(m_realTimeSpeed.GetPos()) / kSpeedSteps;
    return 0.1 * std::pow(100.0, fraction);
}

void CSimulationDlg::OnRealTimeChanged()
{
    g_realTime = IsDlgButtonChecked(IDC_SIM_REALTIME) == BST_CHECKED;
    m_lastTick = GetTickCount64();
    ShowState();
}

void CSimulationDlg::Stop()
{
    if (m_playing)
        KillTimer(kPlayTimer);
    m_playing = false;
    if (m_running)
    {
        // Stopped part way: the view catches up with where it got to.
        KillTimer(kRunTimer);
        m_running = false;
        if (m_pTab != nullptr)
            m_pTab->OnSimulationChanged();
    }
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
    status += L" \u2014 time " + TimeText(simulation.ElapsedSeconds()) + L" of " + TimeText(simulation.TotalSeconds());
    if (simulation.Finished())
        status += L" (done)";
    SetDlgItemText(IDC_SIM_STATUS, status.c_str());
    m_position.SetPos(static_cast<int>((std::min)(simulation.Position(), size_t(0x7FFFFFFF))));

    SetDlgItemText(IDC_SIM_INFO, (m_saved.empty() ? m_note : m_saved + L" " + m_note).c_str());
    SetDlgItemText(IDC_SIM_COMPARE, m_pTab->SimulationCompareText().c_str());

    const size_t speed = MovesPerTick();
    SetDlgItemText(IDC_SIM_SPEED_TEXT, (WithCommas(speed) + (speed == 1 ? L" move a step" : L" moves a step")).c_str());

    // Real time plays at the feeds, times the slider; moves a step aren't used.
    const double factor = RealTimeFactor();
    wchar_t times[32];
    swprintf_s(times, L"%.2g\u00D7 real time", factor);
    SetDlgItemText(IDC_SIM_REALTIME_TEXT, times);
    GetDlgItem(IDC_SIM_REALTIME_SPEED)->EnableWindow(g_realTime ? TRUE : FALSE);
    GetDlgItem(IDC_SIM_REALTIME_TEXT)->EnableWindow(g_realTime ? TRUE : FALSE);
    GetDlgItem(IDC_SIM_SPEED)->EnableWindow(g_realTime ? FALSE : TRUE);
    GetDlgItem(IDC_SIM_SPEED_TEXT)->EnableWindow(g_realTime ? FALSE : TRUE);

    std::wstring warning;
    if (simulation.HasWarning())
    {
        const camcore::SimMove& move = simulation.Moves()[simulation.WarningMove()];
        const wchar_t* what = L"Rapid into stock";
        if (simulation.Warning() == camcore::SimWarning::HolderHitsStock)
            what = L"The tool holder hits the stock";
        else if (simulation.Warning() == camcore::SimWarning::ShankHitsStock)
            what = L"The tool's shank hits the stock";
        warning = std::wstring(what) + L" in " + FromUtf8(simulation.Operations()[move.operation].name) +
                  L", toolpath move " + std::to_wstring(move.toolpathMove + 1) + L".";
    }
    SetDlgItemText(IDC_SIM_WARNING, warning.c_str());

    const BOOL more = simulation.Finished() ? FALSE : TRUE;
    GetDlgItem(IDC_SIM_STEP)->EnableWindow(more);
    GetDlgItem(IDC_SIM_OPERATION_END)->EnableWindow(more);
    GetDlgItem(IDC_SIM_END)->EnableWindow(more);
    GetDlgItem(IDC_SIM_PLAY)->EnableWindow(more || m_playing || m_running);
}

void CSimulationDlg::OnShowChanged()
{
    g_show.stock = IsDlgButtonChecked(IDC_SIM_SHOW_STOCK) == BST_CHECKED;
    g_show.part = IsDlgButtonChecked(IDC_SIM_SHOW_PART) == BST_CHECKED;
    g_show.notEnough = IsDlgButtonChecked(IDC_SIM_SHOW_NOT_ENOUGH) == BST_CHECKED;
    g_show.tooMuch = IsDlgButtonChecked(IDC_SIM_SHOW_TOO_MUCH) == BST_CHECKED;
    if (m_pTab == nullptr)
        return;
    CWaitCursor wait;
    m_pTab->SetSimulationShow(g_show);
    ShowState();
}

void CSimulationDlg::OnSaveStl()
{
    Stop();
    if (m_pTab == nullptr)
        return;

    // The cut stock as it is now, as an STL file: beside the .cam file at first,
    // then in the last folder used.
    const std::filesystem::path suggested = m_pTab->SimulationStlPath();
    const std::wstring folder = g_stlFolder.empty() ? suggested.parent_path().wstring() : g_stlFolder;
    CFileDialog dialog(FALSE, L"stl", suggested.filename().c_str(),
                       OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST,
                       L"STL files (*.stl)|*.stl|All files (*.*)|*.*||", this);
    if (!folder.empty())
        dialog.m_ofn.lpstrInitialDir = folder.c_str();
    if (dialog.DoModal() != IDOK)
        return;

    const std::filesystem::path path(dialog.GetPathName().GetString());
    g_stlFolder = path.parent_path().wstring();
    m_pTab->SaveSimulationStl(path, m_saved);
    ShowState();
}

void CSimulationDlg::OnAutoDivide()
{
    g_autoDivide = IsDlgButtonChecked(IDC_SIM_AUTO_DIVIDE) == BST_CHECKED;
    ShowResolution();
    Restart();
}

void CSimulationDlg::ShowResolution()
{
    const BOOL slider = (!g_autoDivide && m_stops.size() > 1) ? TRUE : FALSE;
    for (int id : { IDC_SIM_RESOLUTION, IDC_SIM_RESOLUTION_LABEL, IDC_SIM_FINE, IDC_SIM_MEDIUM, IDC_SIM_COARSE })
        GetDlgItem(id)->EnableWindow(slider);
}

void CSimulationDlg::Restart()
{
    Stop();
    if (m_pTab == nullptr)
        return;
    CWaitCursor wait;
    m_pTab->RestartSimulation(ChosenResolution(m_stops));
    ShowState();
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
    m_pTab->GetSimulation().ClearWarning();
    RunTo(m_pTab->GetSimulation().OperationEnd());
}

void CSimulationDlg::OnEnd()
{
    Stop();
    if (m_pTab == nullptr)
        return;
    m_pTab->GetSimulation().ClearWarning();
    RunTo(m_pTab->GetSimulation().Moves().size());
}

void CSimulationDlg::RunTo(size_t end)
{
    // On a timer, so the dialog's buttons still work: Play becomes Pause. The view
    // is redrawn when it gets there or is stopped; the slider shows where it is.
    if (m_pTab == nullptr || m_pTab->GetSimulation().Position() >= end)
        return;
    m_runEnd = end;
    m_running = true;
    SetDlgItemText(IDC_SIM_PLAY, L"Pause");
    ShowState();
    SetTimer(kRunTimer, kRunMilliseconds, nullptr);
}

void CSimulationDlg::RunTick()
{
    if (m_pTab == nullptr)
    {
        Stop();
        return;
    }
    camcore::Simulation& simulation = m_pTab->GetSimulation();
    const ULONGLONG start = GetTickCount64();
    while (simulation.Position() < m_runEnd && !simulation.HasWarning() &&
           GetTickCount64() - start < kRunSliceMilliseconds)
    {
        if (simulation.Step(1) == 0)
            break;
    }
    if (simulation.Position() >= m_runEnd || simulation.HasWarning() || simulation.Finished())
    {
        Stop();             // redraws the view
        ShowState();
        return;
    }
    ShowState();
}

void CSimulationDlg::OnPlay()
{
    if (m_playing || m_running)
    {
        Stop();
        ShowState();
        return;
    }
    if (m_pTab == nullptr || m_pTab->GetSimulation().Finished())
        return;
    m_pTab->GetSimulation().ClearWarning();
    m_playing = true;
    m_lastTick = GetTickCount64();
    SetDlgItemText(IDC_SIM_PLAY, L"Pause");
    SetTimer(kPlayTimer, kPlayMilliseconds, nullptr);
}

void CSimulationDlg::OnTimer(UINT_PTR id)
{
    if (id == kRunTimer)
    {
        RunTick();
        return;
    }
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
    if (g_realTime)
    {
        // As much machining time as has passed since the last tick, times the slider.
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG passed = (std::min)(kLongestTick, now - m_lastTick);
        m_lastTick = now;
        simulation.Advance(static_cast<double>(passed) / 1000.0 * RealTimeFactor());
    }
    else
    {
        simulation.Step(MovesPerTick());
    }
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
    if (pScrollBar->GetSafeHwnd() == m_realTimeSpeed.GetSafeHwnd())
    {
        g_realTimeSpeed = m_realTimeSpeed.GetPos();
        ShowState();
        return;
    }

    // A new resolution starts again when the slider is let go at a different stop.
    if (pScrollBar->GetSafeHwnd() == m_resolution.GetSafeHwnd())
    {
        if (code != TB_THUMBTRACK && m_resolution.GetPos() != m_appliedStop)
        {
            g_resolutionStop = m_resolution.GetPos();
            m_appliedStop = g_resolutionStop;
            Restart();
        }
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
