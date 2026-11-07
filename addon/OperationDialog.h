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

#include <map>
#include <memory>
#include <set>
#include <vector>

class CCamTabCommand;
class COperationDlg;

// What a pick in the view is measuring for.
enum class LocateTarget
{
    DrillDepth,     // the Drilling tab's depth
    CutTop,         // the Cut Levels tab's Pick Top Z
    CutDepth,       // the Cut Levels tab's total depth
    HoleTop,        // the Hole Pocketing tab's Pick Top Z
    HoleDepth       // the Hole Pocketing tab's hole depth
};

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

    // The tab's dialog template, which says which help section it has.
    UINT TemplateId() const { return m_templateId; }

    // Bulk editing works on the controls, so each tab's own Load, Save and
    // rules are used unchanged. A control's value, by its ID: an edit's text, a
    // check box's or radio's check, a combo's or list's selection. Sliders are
    // left out, as they follow the boxes beside them.
    struct ControlValue
    {
        int kind = 0;
        std::wstring text;
        int number = 0;

        bool operator==(const ControlValue& other) const
        {
            return kind == other.kind && text == other.text && number == other.number;
        }
        bool operator!=(const ControlValue& other) const { return !(*this == other); }
    };
    using ControlState = std::map<int, ControlValue>;

    ControlState Snapshot() const;

    // Shows each control whose value differs between the states as mixed: an
    // empty box, no radio or combo choice, a check box neither on nor off.
    void ShowMixed(const std::vector<ControlState>& states);

    // The controls that differ between two states. A changed radio brings every
    // radio in its group.
    std::set<int> ChangedControls(const ControlState& before, const ControlState& after);

    // Sets those controls to their values in the state, telling the tab as if
    // the user had: choices first, then boxes.
    void Overlay(const ControlState& state, const std::set<int>& ids);

protected:
    // Enter and Escape belong to the dialog, not the page.
    void OnOK() override {}
    void OnCancel() override {}

    // Reads a number. Blank is allowed only if blankAllowed, and gives false.
    bool ReadNumber(int id, const wchar_t* label, double& value, std::wstring& problems,
                    bool blankAllowed = false, bool* pBlank = nullptr);
    void SetNumber(int id, double value);

    // Reads a number quietly, as the user types. False if the box isn't one.
    bool ReadValue(int id, double& value);

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
    afx_msg void OnStockFootprint();
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

public:
    // The tool chosen on the tab, or the operation's own if none is.
    std::string ChosenToolId() const;

private:
    const camcore::Tool* ChosenTool() const;
    void ShowDetails(const camcore::Tool* pTool);

    camcore::ToolLibrary m_library;
    std::string m_toolId;       // kept if it isn't in the library, so it isn't lost
    CListCtrl m_tools;
    CListCtrl m_details;
};

// The Feeds & Speeds tab every operation has.
class CFeedsPage : public COperationPage
{
public:
    explicit CFeedsPage(COperationDlg* pOwner);

    void Load(const camcore::SequenceStep& step) override;
    bool Save(camcore::SequenceStep& step, std::wstring& problems) override;

    enum { IDD = IDD_OP_FEEDS };

protected:
    BOOL OnInitDialog() override;
    afx_msg void OnTransferChanged();
    afx_msg void OnFromTool();

    DECLARE_MESSAGE_MAP()

private:
    void UpdateFields();
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

// The Cut Levels tab, for operations that cut in levels (not Drill).
class CCutLevelsPage : public COperationPage
{
public:
    explicit CCutLevelsPage(COperationDlg* pOwner);

    void Load(const camcore::SequenceStep& step) override;
    bool Save(camcore::SequenceStep& step, std::wstring& problems) override;

    void ShowLocating(LocateTarget target, bool locating);
    void SetTopZ(double z);
    void SetDepthToHeight(double z);    // the total depth becomes the distance from the cut geometry

    enum { IDD = IDD_OP_CUT_LEVELS };

protected:
    BOOL OnInitDialog() override;
    afx_msg void OnLocationChanged();
    afx_msg void OnPickTop();
    afx_msg void OnPickDepth();
    afx_msg void OnTotalChanged();
    afx_msg void OnRoughChanged();
    afx_msg void OnFinishChanged();
    afx_msg void OnStepChanged();
    afx_msg void OnHScroll(UINT code, UINT position, CScrollBar* pScrollBar);

    DECLARE_MESSAGE_MAP()

private:
    static int StepPosition(double step, double depth);
    void ShowDepths();
    void FitSteps();
    double ReferenceZ();

    camcore::CutLevelSettings m_levels;     // the depths as edited so far
    CSliderCtrl m_ratio;
    CSliderCtrl m_roughSlider;
    CSliderCtrl m_finishSlider;
    bool m_updating;                        // the page is setting its own boxes
};

// The Entry/Exit tab, for operations that cut in levels (not Drill).
class CEntryExitPage : public COperationPage
{
public:
    explicit CEntryExitPage(COperationDlg* pOwner);

    void Load(const camcore::SequenceStep& step) override;
    bool Save(camcore::SequenceStep& step, std::wstring& problems) override;

    enum { IDD = IDD_OP_ENTRY_EXIT };

protected:
    afx_msg void OnChoiceChanged();

    DECLARE_MESSAGE_MAP()

private:
    // The control IDs of one engage or retract motion's group.
    struct LeadControls
    {
        int group, linear, radial, lengthLabel, length, angleLabel, angle, radiusLabel, radius;
    };

    void LoadLead(const LeadControls& ids, const camcore::LeadMotion& lead);
    void SaveLead(const LeadControls& ids, const wchar_t* name, camcore::LeadMotion& lead, std::wstring& found);
    void EnableLead(const LeadControls& ids, bool enabled);
    void UpdateFields();

    static const LeadControls kEngage;
    static const LeadControls kRetract;
};

// The Facing operation's own tab.
class CFacingPage : public COperationPage
{
public:
    explicit CFacingPage(COperationDlg* pOwner);

    void Load(const camcore::SequenceStep& step) override;
    bool Save(camcore::SequenceStep& step, std::wstring& problems) override;

    enum { IDD = IDD_OP_FACING };

protected:
    afx_msg void OnChoiceChanged();

    DECLARE_MESSAGE_MAP()

private:
    // A "% Tool Dia" or "Distance" pair of radio buttons, each with its box.
    struct SizeControls
    {
        int percentRadio, percent, distanceRadio, distance;
    };

    void LoadSize(const SizeControls& ids, const camcore::ToolRelativeSize& size);
    void SaveSize(const SizeControls& ids, const wchar_t* name, camcore::ToolRelativeSize& size, std::wstring& found);
    void UpdateFields();

    static const SizeControls kStep;
    static const SizeControls kOvertravel;
};

// The Profiling operation's own tab.
class CProfilingPage : public COperationPage
{
public:
    explicit CProfilingPage(COperationDlg* pOwner);
    CProfilingPage(UINT templateId, COperationDlg* pOwner);      // for the Chamfering tab

    void Load(const camcore::SequenceStep& step) override;
    bool Save(camcore::SequenceStep& step, std::wstring& problems) override;

    enum { IDD = IDD_OP_PROFILING };

protected:
    BOOL OnInitDialog() override;
    afx_msg void OnChoiceChanged();
    afx_msg void OnWidthChanged();
    afx_msg void OnStepChanged();
    afx_msg void OnHScroll(UINT code, UINT position, CScrollBar* pScrollBar);

    DECLARE_MESSAGE_MAP()

private:
    void ShowStepover();
    void UpdateFields();

    double m_cutWidth;          // the stepover as edited so far
    double m_stepPerCut;
    CSliderCtrl m_stepSlider;
    bool m_updating;            // the page is setting its own boxes
};

// The Chamfer operation's own tab: Profiling's, with the chamfer's width, tool tip
// clearance and depth per cut.
class CChamferingPage : public CProfilingPage
{
public:
    explicit CChamferingPage(COperationDlg* pOwner);

    void Load(const camcore::SequenceStep& step) override;
    bool Save(camcore::SequenceStep& step, std::wstring& problems) override;

    enum { IDD = IDD_OP_CHAMFERING };
};

// The Engrave operation's own tab: how the tool gets between cut levels.
class CEngravingPage : public COperationPage
{
public:
    explicit CEngravingPage(COperationDlg* pOwner);

    void Load(const camcore::SequenceStep& step) override;
    bool Save(camcore::SequenceStep& step, std::wstring& problems) override;

    enum { IDD = IDD_OP_ENGRAVING };
};

// The Pocketing operation's own tab.
class CPocketingPage : public COperationPage
{
public:
    explicit CPocketingPage(COperationDlg* pOwner);

    void Load(const camcore::SequenceStep& step) override;
    bool Save(camcore::SequenceStep& step, std::wstring& problems) override;

    enum { IDD = IDD_OP_POCKETING };

protected:
    afx_msg void OnCutTypeChanged();
    afx_msg void OnDirectionChanged();
    afx_msg void OnStartChanged();
    afx_msg void OnStepoverChanged();

    DECLARE_MESSAGE_MAP()

private:
    camcore::PocketCutType ShownCutType() const;
    void UpdateFields();

    // What's been chosen, kept apart from the buttons, as Spiral can't show
    // Mixed and the start buttons mean different things for Linear Cuts.
    camcore::CutDirection m_direction;
    camcore::PocketStart m_start;
    camcore::PocketLinearStart m_linearStart;
};

// The Waterline Roughing operation's own tab.
class CWaterlineRoughingPage : public COperationPage
{
public:
    explicit CWaterlineRoughingPage(COperationDlg* pOwner);

    void Load(const camcore::SequenceStep& step) override;
    bool Save(camcore::SequenceStep& step, std::wstring& problems) override;

    enum { IDD = IDD_OP_WATERLINE_ROUGHING };

protected:
    BOOL OnInitDialog() override;
    afx_msg void OnChoiceChanged();

    DECLARE_MESSAGE_MAP()

private:
    void UpdateFields();
};

// The Waterline Finishing operation's own tab.
class CWaterlineFinishingPage : public COperationPage
{
public:
    explicit CWaterlineFinishingPage(COperationDlg* pOwner);

    void Load(const camcore::SequenceStep& step) override;
    bool Save(camcore::SequenceStep& step, std::wstring& problems) override;

    enum { IDD = IDD_OP_WATERLINE_FINISHING };

protected:
    afx_msg void OnChoiceChanged();

    DECLARE_MESSAGE_MAP()

private:
    void UpdateFields();
};

// The Parallel Finishing operation's own tab.
class CParallelFinishingPage : public COperationPage
{
public:
    explicit CParallelFinishingPage(COperationDlg* pOwner);

    void Load(const camcore::SequenceStep& step) override;
    bool Save(camcore::SequenceStep& step, std::wstring& problems) override;

    enum { IDD = IDD_OP_PARALLEL_FINISHING };

protected:
    afx_msg void OnChoiceChanged();

    DECLARE_MESSAGE_MAP()

private:
    void UpdateFields();
};

// The waterline operations' own Cut Levels tab.
class CWaterlineLevelsPage : public COperationPage
{
public:
    explicit CWaterlineLevelsPage(COperationDlg* pOwner);

    void Load(const camcore::SequenceStep& step) override;
    bool Save(camcore::SequenceStep& step, std::wstring& problems) override;

    enum { IDD = IDD_OP_WATERLINE_LEVELS };

protected:
    afx_msg void OnChoiceChanged();

    DECLARE_MESSAGE_MAP()

private:
    void UpdateFields();
    void ShowSinglePass();

    bool m_singlePass = false;      // Parallel Finishing: Top and Bottom only
};

// The Plunge Roughing operation's own tab.
class CPlungeRoughingPage : public COperationPage
{
public:
    explicit CPlungeRoughingPage(COperationDlg* pOwner);

    void Load(const camcore::SequenceStep& step) override;
    bool Save(camcore::SequenceStep& step, std::wstring& problems) override;

    enum { IDD = IDD_OP_PLUNGE_ROUGHING };

protected:
    afx_msg void OnStepoverChanged();

    DECLARE_MESSAGE_MAP()

private:
    void UpdateFields();
};

// The Hole Pocketing operation's own tab.
class CHolePocketingPage : public COperationPage
{
public:
    explicit CHolePocketingPage(COperationDlg* pOwner);

    void Load(const camcore::SequenceStep& step) override;
    bool Save(camcore::SequenceStep& step, std::wstring& problems) override;

    void ShowLocating(LocateTarget target, bool locating);
    void SetTopZ(double z);
    void SetDepthToHeight(double z);    // the depth becomes the distance from the cut geometry

    enum { IDD = IDD_OP_HOLE_POCKETING };

protected:
    afx_msg void OnChoiceChanged();
    afx_msg void OnPickTop();
    afx_msg void OnPickDepth();

    DECLARE_MESSAGE_MAP()

private:
    void UpdateFields();
    double ReferenceZ();
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

    // Edits several operations together: only the tabs they all have, with
    // values that differ shown mixed, and only the fields changed applied to each.
    COperationDlg(CCamTabCommand* pTab, const std::vector<camcore::SequenceStep>& steps);
    bool IsBulk() const { return !m_bulk.empty(); }
    const std::vector<camcore::SequenceStep>& GetSteps() const { return m_bulk; }

    bool CreateModeless(CWnd* pParent);
    void DetachFromTab() { m_pTab = nullptr; }

    const camcore::SequenceStep& GetStep() const { return m_step; }

    // Locate on the Drilling tab picks something in the view to measure the depth to.
    // Locate picks something in the view to measure from, for one of several boxes.
    bool IsLocating() const { return m_locating; }
    void StartLocating(LocateTarget target);
    void StopLocating();
    void SetLocatedHeight(bool found, double height);   // a signed Z in the operation's frame
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

    // The chosen tool's speeds and feeds for the job's material. False with a
    // problem, in words for the user, if there are none.
    bool FeedsFromTool(camcore::FeedSettings& feeds, std::wstring& problem) const;

    enum { IDD = IDD_OPERATION };

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    void OnCancel() override;
    void PostNcDestroy() override;

    afx_msg void OnTabChanged(NMHDR* pNotify, LRESULT* pResult);
    afx_msg void OnClose();
    afx_msg void OnGenerate();
    afx_msg void OnHelpButton();

    DECLARE_MESSAGE_MAP()

private:
    void AddPage(const wchar_t* title, std::unique_ptr<COperationPage> page);
    void ShowPage(int index);
    bool ReadSettings();        // every tab into m_step. False after showing the problems.
    bool ReadBulkSettings();    // the changed fields into each of m_bulk
    std::unique_ptr<COperationPage> MakePage(UINT templateId);
    void Finish(bool apply);

    CCamTabCommand* m_pTab;
    camcore::SequenceStep m_step;
    CTabCtrl m_tabs;
    std::vector<std::unique_ptr<COperationPage>> m_pages;
    CFeaturesPage* m_pFeatures;     // owned by m_pages
    CDrillingPage* m_pDrilling;     // owned by m_pages. Null for other operations.
    CCutLevelsPage* m_pCutLevels;   // owned by m_pages. Null for operations without one.
    CHolePocketingPage* m_pHolePocketing;   // owned by m_pages. Null for other operations.
    CToolPage* m_pToolPage;     // owned by m_pages

    // Bulk editing: the operations, each tab's state as first shown, and a
    // hidden copy of each tab that applies the changes to one operation at a time.
    std::vector<camcore::SequenceStep> m_bulk;
    std::vector<COperationPage::ControlState> m_baselines;
    std::vector<std::unique_ptr<COperationPage>> m_twins;
    bool m_locating;
    LocateTarget m_locateTarget;
    bool m_selectingFeatures;
    bool m_haveFrame;
    camcore::Frame m_frame;
};
