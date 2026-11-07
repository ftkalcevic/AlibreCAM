// ProgressDialog.h : the window that shows what Generate is doing while it works.

#pragma once

#include <memory>
#include <string>

#include "resource.h"

// A small modeless window: the operation, its stage, and how far along it is.
class CGenerateProgressDlg : public CDialog
{
public:
    CGenerateProgressDlg();

    // Shown in the middle of the top-level window holding `nearWindow` (or the screen).
    bool CreateOver(HWND nearWindow);
    void Show(const std::wstring& operation, const std::wstring& stage, double fraction);

    enum { IDD = IDD_GENERATE_PROGRESS };

protected:
    BOOL OnInitDialog() override;
    void OnCancel() override {}     // Escape doesn't close it
    void OnOK() override {}
};

// Shows the progress window for one Generate, only once it's taken more than
// half a second, and keeps it painted while Alibre's thread is busy: it paints
// the window straight away and peeks at the message queue so Windows doesn't
// call Alibre "Not responding", but it handles no clicks or keys, so nothing
// else runs in the middle of Generate. The window closes when this goes.
class GenerateProgress
{
public:
    // nearWindow: a window in the one it's shown in the middle of (the CAM tab's).
    GenerateProgress(size_t operations, HWND nearWindow);
    ~GenerateProgress();

    // The operation now being generated, from 0.
    void StartOperation(size_t index, const std::wstring& name);

    // Its stage, and how far along the stage is (0 to 1), or a negative number for not known.
    void Update(double fraction, const std::wstring& stage);

private:
    void Refresh(bool force);

    size_t m_operations;
    HWND m_near;
    ULONGLONG m_start;
    ULONGLONG m_lastShown = 0;
    std::wstring m_operation;
    std::wstring m_stage;
    double m_fraction = -1.0;
    std::unique_ptr<CGenerateProgressDlg> m_pDialog;
};
