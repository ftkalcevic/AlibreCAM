// OperationDialog.h : the modeless dialog that edits one operation.
//
// Every operation has the same first five tabs, then tabs of its own. It's
// modeless so tabs can pick in the view. Tabs are built one at a time; the
// ones not built yet are an empty COperationPage.

#pragma once

#include "resource.h"
#include "CamJob.h"

#include <memory>
#include <vector>

class CCamTabCommand;
class COperationDlg;

// One tab of the dialog. A tab with settings derives from this.
class COperationPage : public CDialog
{
public:
    COperationPage(UINT templateId, COperationDlg* pOwner);

    bool CreateInTab(CWnd* pParent);

    // Fill the controls from the step, and read them back into it. Save adds a
    // line to problems for each thing that's wrong, and returns false if any are.
    virtual void Load(const camcore::SequenceStep& /*step*/) {}
    virtual bool Save(camcore::SequenceStep& /*step*/, std::wstring& /*problems*/) { return true; }

protected:
    // Enter and Escape belong to the dialog, not the page.
    void OnOK() override {}
    void OnCancel() override {}

    // Reads a number. Blank is allowed only if blankAllowed, and gives false.
    bool ReadNumber(int id, const wchar_t* label, double& value, std::wstring& problems,
                    bool blankAllowed = false, bool* pBlank = nullptr);
    void SetNumber(int id, double value);

    COperationDlg* m_pOwner;

private:
    UINT m_templateId;
};

// The Drill operation's own tab.
class CDrillingPage : public COperationPage
{
public:
    explicit CDrillingPage(COperationDlg* pOwner);

    void Load(const camcore::SequenceStep& step) override;
    bool Save(camcore::SequenceStep& step, std::wstring& problems) override;

    void ShowLocating(bool locating);
    void SetDepth(double depth);
    void ShowLocateNote(const std::wstring& text);

    enum { IDD = IDD_OP_DRILLING };

protected:
    BOOL OnInitDialog() override;
    afx_msg void OnLocate();
    afx_msg void OnDwellChanged();

    DECLARE_MESSAGE_MAP()

private:
    void UpdateDwellFields();
};

class COperationDlg : public CDialog
{
public:
    COperationDlg(CCamTabCommand* pTab, const camcore::SequenceStep& step);

    bool CreateModeless(CWnd* pParent);
    void DetachFromTab() { m_pTab = nullptr; }

    const camcore::SequenceStep& GetStep() const { return m_step; }

    // Locate on the Drilling tab picks something in the view to measure the depth to.
    bool IsLocating() const { return m_locating; }
    void StartLocating();
    void StopLocating();
    void SetLocatedDepth(double depth);
    void ShowLocateProblem(const std::wstring& problem);

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
    void AddPage(const wchar_t* title, std::unique_ptr<COperationPage> page);
    void ShowPage(int index);
    void Finish(bool apply);

    CCamTabCommand* m_pTab;
    camcore::SequenceStep m_step;
    CTabCtrl m_tabs;
    std::vector<std::unique_ptr<COperationPage>> m_pages;
    CDrillingPage* m_pDrilling;     // owned by m_pages. Null for other operations.
    bool m_locating;
};
