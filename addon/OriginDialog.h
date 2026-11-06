// OriginDialog.h : the modeless dialog that edits one Set Origin step.
//
// It stays open while the user picks in the view. The tab passes it each pick,
// draws its preview, and applies it on OK.

#pragma once

#include "resource.h"
#include "CamJob.h"

class CCamTabCommand;

enum class PickSlot
{
    None,
    Plane,
    Position,
    XDirection
};

class COriginDlg : public CDialog
{
public:
    COriginDlg(CCamTabCommand* pTab, const camcore::SequenceStep& step);

    bool CreateModeless(CWnd* pParent);
    void DetachFromTab() { m_pTab = nullptr; }

    // The step as edited so far. Its offsets are the last valid ones typed.
    const camcore::SequenceStep& GetStep() const { return m_step; }

    PickSlot GetPickSlot() const { return m_pickSlot; }
    void SetPick(PickSlot slot, const camcore::PickedGeometry& pick);
    void ShowPickProblem(const std::wstring& problem);
    void StopPicking();

    enum { IDD = IDD_SET_ORIGIN };

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    void OnCancel() override;
    void PostNcDestroy() override;

    afx_msg void OnPickPlane();
    afx_msg void OnPickPosition();
    afx_msg void OnPickX();
    afx_msg void OnClearPosition();
    afx_msg void OnClearX();
    afx_msg void OnOptionChanged();
    afx_msg void OnClose();
    afx_msg void OnHelpButton();

    DECLARE_MESSAGE_MAP()

private:
    void StartPicking(PickSlot slot);
    void ShowPicks();
    void UpdatePickButtons();
    bool ReadOffsets(std::wstring& problem);
    void UpdatePreview();
    void Finish(bool apply);

    CCamTabCommand* m_pTab;
    camcore::SequenceStep m_step;
    PickSlot m_pickSlot;
    bool m_offsetsValid;
    bool m_filling;
    CComboBox m_rotation;
};
