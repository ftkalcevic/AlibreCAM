// OperationDialog.h : the modeless dialog that edits one operation.
//
// Every operation has the same first five tabs, then tabs of its own. It's
// modeless so the Features tab can pick in the view. The tabs are empty for
// now and are built one at a time.

#pragma once

#include "resource.h"
#include "CamJob.h"

#include <memory>
#include <vector>

class CCamTabCommand;

// One tab of the dialog. Each becomes its own class as it's built.
class COperationPage : public CDialog
{
public:
    COperationPage();

    bool CreateInTab(CWnd* pParent);

    enum { IDD = IDD_OP_PAGE };
};

class COperationDlg : public CDialog
{
public:
    COperationDlg(CCamTabCommand* pTab, const camcore::SequenceStep& step);

    bool CreateModeless(CWnd* pParent);
    void DetachFromTab() { m_pTab = nullptr; }

    const camcore::SequenceStep& GetStep() const { return m_step; }

    enum { IDD = IDD_OPERATION };

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    void OnCancel() override;
    void PostNcDestroy() override;

    afx_msg void OnTabChanged(NMHDR* pNotify, LRESULT* pResult);
    afx_msg void OnClose();

    DECLARE_MESSAGE_MAP()

private:
    void AddPage(const wchar_t* title);
    void ShowPage(int index);
    void Finish(bool apply);

    CCamTabCommand* m_pTab;
    camcore::SequenceStep m_step;
    CTabCtrl m_tabs;
    std::vector<std::unique_ptr<COperationPage>> m_pages;
};
