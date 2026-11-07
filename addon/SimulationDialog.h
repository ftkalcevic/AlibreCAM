// SimulationDialog.h : the modeless Simulation dialog, which plays the toolpaths
// against the stock in Alibre's view.

#pragma once

#include "resource.h"

#include "Simulation.h"

class CCamTabCommand;

class CSimulationDlg : public CDialog
{
public:
    explicit CSimulationDlg(CCamTabCommand* pTab);

    bool CreateModeless(CWnd* pParent);
    void DetachFromTab() { m_pTab = nullptr; }

    // A line under the controls, such as operations that were left out.
    void SetNote(const std::wstring& note) { m_note = note; }

    // The resolution last chosen (remembered until Alibre closes), for a stock
    // whose Resolution slider has these stops.
    static camcore::SimResolution ChosenResolution(const std::vector<double>& stops);

    enum { IDD = IDD_SIMULATION };

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    void OnCancel() override;
    void PostNcDestroy() override;

    afx_msg void OnRewind();
    afx_msg void OnStep();
    afx_msg void OnPlay();
    afx_msg void OnOperationEnd();
    afx_msg void OnEnd();
    afx_msg void OnHScroll(UINT code, UINT position, CScrollBar* pScrollBar);
    afx_msg void OnTimer(UINT_PTR id);
    afx_msg HBRUSH OnCtlColor(CDC* pDC, CWnd* pWnd, UINT ctlColor);
    afx_msg void OnClose();
    afx_msg void OnHelpButton();
    afx_msg void OnAutoDivide();
    afx_msg void OnSaveStl();
    afx_msg void OnShowChanged();

    DECLARE_MESSAGE_MAP()

private:
    void Stop();
    void Changed();             // the stock and the tool moved: redraw and show where it is
    void ShowState();
    size_t MovesPerTick() const;
    void Finish();
    void ShowResolution();      // greys the slider while Auto divide is on
    void Restart();             // the stock again at the chosen resolution, cut to the same move

    CCamTabCommand* m_pTab;
    CSliderCtrl m_position;
    CSliderCtrl m_speed;
    CSliderCtrl m_resolution;
    std::vector<double> m_stops;
    int m_appliedStop = -1;
    bool m_playing;
    std::wstring m_note;
    std::wstring m_saved;       // what the last Save STL did
};
