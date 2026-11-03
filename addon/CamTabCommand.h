// CamTabCommand.h : the two-way toggle command that owns the CAM tab.
//
// There is one of these per assembly workspace. It holds that assembly's job,
// and loads and saves its .cam file. While the tab has focus it runs a
// CCamViewCommand, which draws the origins and receives picks.

#pragma once

#include "CommandBase.h"
#include "CamJob.h"
#include "CamViewCommand.h"
#include "OriginDialog.h"
#include "ToolLibrary.h"

class CAddOnInterface;
class CCamTabWnd;

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

    // The operation sequence
    void AddSetOrigin();
    void EditStep(size_t index);
    void DeleteStep(size_t index);
    void SelectStep(size_t index);
    bool CanEditSequence() const;

    // The view command, which draws while the tab has focus
    void OnTabVisibilityChanged(bool visible);
    void RefreshView();
    void SetForeground(bool foreground);
    const CamScene& GetScene() const { return m_scene; }
    void HandleSelection();
    void ClearPickedSelection();
    void OnViewTerminated(CCamViewCommand* pView);

    // From the Set Origin dialog
    void BeginPick(PickSlot slot);
    void EndPick();
    void OnOriginPreviewChanged();
    void OnOriginDialogDone(COriginDlg* pDialog, bool apply);

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
    void OpenOriginDialog(const camcore::SequenceStep& step, size_t editIndex);
    void CloseOriginDialog(bool callAlibre);
    void RestoreSelectionFilter();

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

    // The step drawn with its plane. Not saved.
    size_t m_selectedStep;

    CCamViewCommand* m_pView;           // holds a reference while the view is active
    CamScene m_scene;
    bool m_haveBounds;
    camcore::Vec3 m_boundsLower;
    camcore::Vec3 m_boundsUpper;

    COriginDlg* m_pOriginDlg;
    size_t m_editIndex;                 // the step the dialog edits, or kNewStep

    // Picking changes Alibre's selection filter. This puts it back.
    bool m_filterChanged;
    ADSelectionFilterOption m_previousFilter;
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
    void ShowSequence(const camcore::Job& job, size_t selectedStep);

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
    afx_msg void OnTreeSelectionChanged(NMHDR* pNotify, LRESULT* pResult);
    afx_msg void OnTreeDoubleClick(NMHDR* pNotify, LRESULT* pResult);
    afx_msg void OnTreeRightClick(NMHDR* pNotify, LRESULT* pResult);
    afx_msg LRESULT OnLoadJob(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnClearSelection(WPARAM wParam, LPARAM lParam);

    void PostNcDestroy() override;

    DECLARE_MESSAGE_MAP()

private:
    void LayoutControls(int cx, int cy);
    HTREEITEM ItemUnderCursor(bool select);
    bool IsStepItem(HTREEITEM hItem) const;

    CCamTabCommand* m_pTab;
    CTreeCtrl m_tree;
    CStatic m_status;
    CButton m_refresh;
    CButton m_foreground;
    CButton m_save;
    HTREEITEM m_hTools;
    HTREEITEM m_hOperations;
    bool m_fillingTree;
    bool m_visible;
};
