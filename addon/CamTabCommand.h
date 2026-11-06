// CamTabCommand.h : the two-way toggle command that owns the CAM tab.
//
// There is one of these per assembly workspace. It holds that assembly's job,
// and loads and saves its .cam file. While the tab has focus it runs a
// CCamViewCommand, which draws the origins and receives picks.

#pragma once

#include <set>

#include "CommandBase.h"
#include "CamJob.h"
#include "CamViewCommand.h"
#include "GeometryReader.h"
#include "OperationDialog.h"
#include "OriginDialog.h"
#include "ProgramSettings.h"
#include "ToolLibrary.h"

class CAddOnInterface;
class CCamTabWnd;
class CPartBoxStockDlg;

// A place in the sequence: a top-level step, or an operation in a group.
struct StepRef
{
    static constexpr size_t kNone = camcore::kNoStepIndex;

    size_t step = kNone;        // index in the sequence
    size_t child = kNone;       // index in that group, for an operation

    bool IsOperation() const { return child != kNone; }
};

// Where a dragged step lands, next to the step it's dropped on. Into is for
// dropping an operation on a folder, which adds it at the folder's end.
enum class DropSide
{
    Before,
    After,
    Into
};

class CCamTabCommand : public CCommandBase
{
public:
    CCamTabCommand(CAddOnInterface* pAddOn, const std::wstring& sessionId);
    virtual ~CCamTabCommand();

    // Called by the tab window once it's on screen, so a load can show a prompt.
    void LoadJobFile();
    void SaveJobFile();

    // The tool library
    void OpenToolLibrary();
    void SelectToolByIndex(size_t index);

    // The stock. There's at most one.
    bool HasStock() const { return m_job.stock.kind != camcore::StockKind::None; }
    void MakeStock(camcore::StockKind kind);
    void EditStock();
    void DeleteStock();
    void SetStockShown(bool shown);

    // From the Part Box Stock dialog
    void BeginPartSelect();
    void EndPartSelect();
    void OnStockPreviewChanged();
    void OnPartBoxDialogDone(CPartBoxStockDlg* pDialog, bool apply);

    // The operation sequence
    void AddSetOrigin();
    void AddFolder();
    void AddOperation(size_t folder, camcore::StepType type);
    bool CanAddFolder() const;
    bool IsFolder(const StepRef& ref) const;
    bool IsOperation(const StepRef& ref) const;
    bool CanRename(const StepRef& ref) const;
    bool CanDelete(const StepRef& ref) const;
    bool RenameStep(const StepRef& ref, const std::wstring& name);      // false keeps the old name
    void EditStep(const StepRef& ref);
    void DeleteStep(const StepRef& ref);
    void SelectStep(const StepRef& ref);
    void DuplicateStep(const StepRef& ref);     // the copy goes just below
    bool IsSuppressed(const StepRef& ref) const;
    bool CanSuppress(const StepRef& ref) const;
    void ToggleSuppressed(const StepRef& ref);

    // Calculates toolpaths: every operation with no step, a folder's, or one operation's.
    bool CanGenerate() const;
    void Generate(const StepRef& scope);

    // Writes the generated toolpaths of every operation, or a folder's, to a file.
    void Post(const StepRef& scope);

    // Dragging to reorder. Steps never change level, and the first step stays a Set Origin.
    bool CanDrag(const StepRef& ref) const;
    bool CanMoveStep(const StepRef& from, const StepRef& target, DropSide side) const;
    void MoveStep(const StepRef& from, const StepRef& target, DropSide side);
    bool CanEditSequence() const;

    // The view command, which draws while the tab has focus
    void OnTabVisibilityChanged(bool visible);
    void RefreshView();
    void SetForeground(bool foreground);
    void SetToolpathsShown(bool shown);
    const CamScene& GetScene() const { return m_scene; }
    void HandleSelection();
    void ClearPickedSelection();
    void OnViewTerminated(CCamViewCommand* pView);

    // From the Set Origin dialog
    void BeginPick(PickSlot slot);
    void EndPick();
    void OnOriginPreviewChanged();
    void OnOriginDialogDone(COriginDlg* pDialog, bool apply);

    // From the operation dialog
    void BeginLocate();
    void EndLocate();
    void BeginFeatureSelect();
    void EndFeatureSelect();
    bool GetOperationFrame(camcore::Frame& frame) const;
    const camcore::ToolLibrary& GetToolLibrary() const { return m_library; }

    // Shows the rows chosen in the Features list in the view.
    void ShowFeaturesInView(const std::vector<camcore::PickedGeometry>& features);

    // The features whose outlines are drawn in orange: the open operation
    // dialog's, otherwise the tree's selected operation's (all of a folder's
    // operations' for a folder). Their outlines are read from the model and kept
    // until the features change.
    std::vector<camcore::PickedGeometry> HighlightedFeatures() const;
    void UpdateHighlights();
    void OnOperationDialogDone(COperationDlg* pDialog, bool apply);
    void OnOperationDialogGenerate(COperationDlg* pDialog);

    void OnWindowClosing();
    void OnWindowGone();
    void CloseFromAddOn();

    // IAlibreAddOnCommand
    STDMETHOD(IsTwoWayToggle)(VARIANT_BOOL* pValue) override;
    STDMETHOD(AddTab)(VARIANT_BOOL* pValue) override;
    STDMETHOD(get_TabName)(BSTR* pName) override;
    STDMETHOD(OnShowUI)(__int64 hWnd) override;
    STDMETHOD(OnTerminate)() override;

private:
    IADSessionPtr GetSession() const;
    void SetStatus(const std::wstring& text);
    void ShowStatus();
    void SetSaveBlocked(bool blocked);
    void WarnIfUnsaved();
    void MarkJobChanged();
    std::filesystem::path CurrentLibraryPath() const;
    void ReloadTools();
    void ReadJobFile();
    void ShowSequence();
    void DestroyWindowIfOpen();
    void LeaveAddOn();

    bool StartView(const wchar_t* reason);
    void StopView(const wchar_t* reason);
    void ReadModelSize();
    void RebuildScene();
    camcore::SequenceStep* FindStep(const StepRef& ref);
    const camcore::SequenceStep* FindStep(const StepRef& ref) const;
    bool IsDialogOpen() const;
    bool FocusOpenDialog();         // true if a dialog was open
    void OpenOriginDialog(const camcore::SequenceStep& step, size_t editIndex);
    void CloseOriginDialog(bool callAlibre);
    void OpenOperationDialog(const camcore::SequenceStep& step, const StepRef& target);
    void CloseOperationDialog();
    bool PrepareSelection(const SelectionFilter& wanted);     // false if the view couldn't start
    bool IsPicking() const;
    bool GenerateOperation(const StepRef& target, std::string& problem);
    bool ApplyOperationStep(camcore::SequenceStep step);
    // The operations in scope. Suppressed ones are left out, unless the scope is
    // the operation itself and it isn't for a post.
    std::vector<StepRef> OperationsIn(const StepRef& scope, bool forPost) const;
    void RestoreSelectionFilter();
    void HandleLocate();
    void OpenStockDialog(camcore::StockKind kind);
    void ApplyStock(const camcore::Stock& stock);
    void ClosePartBoxDialog(bool callAlibre);
    // The stock's edges in assembly mm, and its box. False with a problem if it can't be read.
    // pPoints, if given, gets points whose highest is the stock's top along any direction.
    bool ReadStockShape(const camcore::Stock& stock, std::vector<camcore::LineSegment>& edges,
                        camcore::StockBox& box, std::wstring& problem,
                        std::vector<camcore::Vec3>* pPoints = nullptr);

    CAddOnInterface* m_pAddOn;
    std::wstring m_sessionId;
    CCamTabWnd* m_pWnd;

    camcore::Job m_job;
    std::filesystem::path m_camPath;

    // Set when a file exists that saving would wrongly overwrite: one from a
    // newer build, a damaged one, or one for a different assembly.
    bool m_saveBlocked;

    bool m_unsavedChanges;

    camcore::ToolLibrary m_library;
    std::wstring m_jobStatus;
    std::wstring m_toolStatus;
    std::wstring m_sequenceStatus;

    // The selected step. The view draws the origin it runs under. Not saved.
    StepRef m_selected;

    CCamViewCommand* m_pView;           // holds a reference while the view is active
    CamScene m_scene;
    bool m_haveBounds;
    camcore::Vec3 m_boundsLower;
    camcore::Vec3 m_boundsUpper;

    COriginDlg* m_pOriginDlg;
    size_t m_editIndex;                 // the step the dialog edits, or kNewStep

    // Only one dialog is open at a time, so this and m_pOriginDlg aren't both set.
    COperationDlg* m_pOperationDlg;
    StepRef m_operationTarget;          // the folder, and the operation in it or kNewStep
    bool m_showingFeatures;             // Features list rows are selected in the view
    std::vector<camcore::PickedGeometry> m_highlighted;    // the features m_highlightOutlines were read for
    std::vector<std::vector<camcore::Vec3>> m_highlightOutlines;   // in assembly mm

    // Only one dialog is open at a time, so this is set only when the others aren't.
    CPartBoxStockDlg* m_pPartBoxDlg;

    // The stock's shape, read again when the stock or the model changes.
    bool m_stockShapeValid;
    std::vector<camcore::LineSegment> m_stockEdges;
    std::string m_stockFacetsPath;                  // the imported part m_stockPartEdges came from
    std::vector<camcore::LineSegment> m_stockPartEdges;     // in the part's own coordinates, mm
    std::vector<camcore::Vec3> m_stockPartPoints;

    std::filesystem::path m_lastPostPath;   // so the next post suggests the same folder
    std::wstring m_lastPostName;            // and the same post

    // Picking changes Alibre's selection filter. This puts it back.
    bool m_filterChanged;
    SelectionFilter m_previousFilter;
};

/////////////////////////////////////////////////////////////////////////////
// The window Alibre hosts in the Design Explorer tab.

class CCamTabWnd : public CWnd
{
public:
    explicit CCamTabWnd(CCamTabCommand* pTab);

    bool CreateInTab(HWND hParent);
    void DetachFromCommand() { m_pTab = nullptr; }

    void SetStatusText(const std::wstring& text);
    void EnableSave(bool enable);
    void ShowTools(const camcore::ToolLibrary& library, const std::string& selectedToolId);
    void MarkSelectedTool(const camcore::ToolLibrary& library, const std::string& selectedToolId);
    void ShowSequence(const camcore::Job& job, const StepRef& selected);
    void ShowStock(const camcore::Stock& stock);

    // Starts editing a step's name in place.
    void StartRename(const StepRef& ref);

    // Clears Alibre's selection once the pick that made it has been handled.
    void PostClearSelection();

protected:
    afx_msg int OnCreate(LPCREATESTRUCT pCreate);
    afx_msg void OnDestroy();
    afx_msg void OnSize(UINT type, int cx, int cy);
    afx_msg void OnTimer(UINT_PTR id);
    afx_msg void OnSave();
    afx_msg void OnRefresh();
    afx_msg void OnForeground();
    afx_msg void OnShowToolpaths();
    afx_msg void OnShowStock();
    afx_msg void OnToolbarTip(NMHDR* pNotify, LRESULT* pResult);
    afx_msg void OnSettings();
    afx_msg void OnHelp();
    afx_msg void OnTreeSelectionChanged(NMHDR* pNotify, LRESULT* pResult);
    afx_msg void OnTreeDoubleClick(NMHDR* pNotify, LRESULT* pResult);
    afx_msg void OnTreeRightClick(NMHDR* pNotify, LRESULT* pResult);
    afx_msg void OnTreeBeginRename(NMHDR* pNotify, LRESULT* pResult);
    afx_msg void OnTreeEndRename(NMHDR* pNotify, LRESULT* pResult);
    afx_msg void OnTreeBeginDrag(NMHDR* pNotify, LRESULT* pResult);
    afx_msg void OnTreeCustomDraw(NMHDR* pNotify, LRESULT* pResult);
    afx_msg void OnMouseMove(UINT flags, CPoint point);
    afx_msg void OnLButtonUp(UINT flags, CPoint point);
    afx_msg void OnRButtonDown(UINT flags, CPoint point);
    afx_msg void OnCaptureChanged(CWnd* pWnd);
    afx_msg LRESULT OnLoadJob(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnClearSelection(WPARAM wParam, LPARAM lParam);

    void PostNcDestroy() override;

    DECLARE_MESSAGE_MAP()

private:
    void LayoutControls(int cx, int cy);
    void ApplyViewSize();           // the text and icons at m_settings' size
    HTREEITEM ItemUnderCursor(bool select);
    bool IsStepItem(HTREEITEM hItem) const;
    StepRef StepOfItem(HTREEITEM hItem) const;
    HTREEITEM FindStepItem(const StepRef& ref) const;

    // Dragging. The window holds the mouse capture while a step is dragged.
    struct DropPlace
    {
        bool valid = false;
        StepRef target;
        DropSide side = DropSide::Before;
        HTREEITEM hMarkItem = nullptr;  // where the insert mark or highlight goes
    };
    DropPlace FindDropPlace(CPoint treePoint) const;
    void ShowDropPlace(const DropPlace& place);
    void EndDrag();

    CCamTabCommand* m_pTab;
    CTreeCtrl m_tree;
    CStatic m_status;
    CButton m_refresh;
    CToolBarCtrl m_toolbar;         // view toggles above the tree
    CToolBarCtrl m_settingsBar;     // the gear, at the right
    CImageList m_toolbarImages;
    CFont m_font;                   // the tab's text, at the view size
    camcore::ProgramSettings m_settings;
    bool m_settingsWritable;        // false if the file is damaged or newer, so it isn't overwritten
    CButton m_save;
    HTREEITEM m_hTools;
    HTREEITEM m_hStock;
    HTREEITEM m_hOperations;
    bool m_fillingTree;
    std::set<HTREEITEM> m_greyItems;    // suppressed steps, and operations in suppressed folders
    bool m_visible;

    bool m_dragging;
    StepRef m_dragFrom;
};
