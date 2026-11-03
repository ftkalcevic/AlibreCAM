// OperationDialog.h : the modeless dialog that edits one operation.
//
// Operations share their first tabs, then have tabs of their own. It's
// modeless so tabs can pick in the view. Tabs are built one at a time; the
// ones not built yet are an empty COperationPage.

#pragma once

#include "resource.h"
#include "CamJob.h"
#include "Features.h"
#include "ToolLibrary.h"

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

// The Features tab every operation has: the geometry it machines, in order.
class CFeaturesPage : public COperationPage
{
public:
    explicit CFeaturesPage(COperationDlg* pOwner);

    void Load(const camcore::SequenceStep& step) override;
    bool Save(camcore::SequenceStep& step, std::wstring& problems) override;

    const std::vector<camcore::PickedGeometry>& GetFeatures() const { return m_features; }
    void SetSelectedFeatures(const std::vector<camcore::PickedGeometry>& selected, int ignored);
    void ForgetViewSelection();     // before selecting starts, so the next selection is all new
    void ShowSelecting(bool selecting);
    void ShowNote(const std::wstring& text);

    // Shows the chosen rows in the view.
    void ShowChosenRows();

    enum { IDD = IDD_OP_FEATURES };

protected:
    BOOL OnInitDialog() override;
    afx_msg void OnSelectFeatures();
    afx_msg void OnDelete();
    afx_msg void OnSort();
    afx_msg void OnOrderChanged();
    afx_msg void OnListKeyDown(NMHDR* pNotify, LRESULT* pResult);
    afx_msg void OnListRightClick(NMHDR* pNotify, LRESULT* pResult);
    afx_msg void OnListBeginDrag(NMHDR* pNotify, LRESULT* pResult);
    afx_msg void OnListSelectionChanged(NMHDR* pNotify, LRESULT* pResult);
    afx_msg void OnMouseMove(UINT flags, CPoint point);
    afx_msg void OnLButtonUp(UINT flags, CPoint point);
    afx_msg void OnCaptureChanged(CWnd* pWnd);
    afx_msg LRESULT OnRowsChosen(WPARAM wParam, LPARAM lParam);

    DECLARE_MESSAGE_MAP()

private:
    void FillList();
    void SelectRow(int row);
    void UpdateButtons();
    int DropRow(CPoint pagePoint) const;     // the row to insert before; the row count means the end
    void EndDrag();
    void ChangedByUser();

    std::vector<camcore::PickedGeometry> m_features;

    // What the view had selected when the list last heard, while selecting.
    std::vector<camcore::PickedGeometry> m_viewSelection;

    CListCtrl m_list;
    CComboBox m_order;
    CComboBox m_corner;
    bool m_dragging;
    int m_dragRow;
    int m_dropHighlight;
    bool m_filling;             // the list is being refilled, so its changes aren't the user's
    bool m_rowsChosenPosted;    // one message covers a burst of row changes
};

// The Tool tab every operation has: the library's tools, and the chosen one's details.
class CToolPage : public COperationPage
{
public:
    explicit CToolPage(COperationDlg* pOwner);

    void Load(const camcore::SequenceStep& step) override;
    bool Save(camcore::SequenceStep& step, std::wstring& problems) override;

    enum { IDD = IDD_OP_TOOL };

protected:
    BOOL OnInitDialog() override;
    afx_msg void OnToolChanged(NMHDR* pNotify, LRESULT* pResult);

    DECLARE_MESSAGE_MAP()

private:
    const camcore::Tool* ChosenTool() const;
    void ShowDetails(const camcore::Tool* pTool);

    camcore::ToolLibrary m_library;
    std::string m_toolId;       // kept if it isn't in the library, so it isn't lost
    CListCtrl m_tools;
    CListCtrl m_details;
};

// The Clearance tab every operation has.
class CClearancePage : public COperationPage
{
public:
    explicit CClearancePage(COperationDlg* pOwner);

    void Load(const camcore::SequenceStep& step) override;
    bool Save(camcore::SequenceStep& step, std::wstring& problems) override;

    enum { IDD = IDD_OP_CLEARANCE };

protected:
    afx_msg void OnModeChanged();

    DECLARE_MESSAGE_MAP()

private:
    void UpdateFields();
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

    // Select Features on the Features tab turns Alibre's selection into the list.
    camcore::FeatureFilter GetFeatureFilter() const;
    bool IsSelectingFeatures() const { return m_selectingFeatures; }
    void StartSelectingFeatures();
    void StopSelectingFeatures();
    std::vector<camcore::PickedGeometry> GetFeatures() const;
    void SetSelectedFeatures(const std::vector<camcore::PickedGeometry>& selected, int ignored);
    void ShowFeatureNote(const std::wstring& text);
    void ShowFeaturesInView(const std::vector<camcore::PickedGeometry>& features);

    // The frame of the origin the operation runs under. False if it has none yet.
    bool GetFrame(camcore::Frame& frame) const;

    // The job's tool library. Empty if the dialog has lost its tab.
    camcore::ToolLibrary GetToolLibrary() const;

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
    CFeaturesPage* m_pFeatures;     // owned by m_pages
    CDrillingPage* m_pDrilling;     // owned by m_pages. Null for other operations.
    bool m_locating;
    bool m_selectingFeatures;
    bool m_haveFrame;
    camcore::Frame m_frame;
};
