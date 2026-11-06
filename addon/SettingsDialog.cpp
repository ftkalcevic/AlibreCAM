// SettingsDialog.cpp : the add-on's Settings dialog, opened from the CAM tab's gear.

#include "stdafx.h"
#include "AlibreCAM.h"
#include "SettingsDialog.h"

namespace fs = std::filesystem;
using camcore::ViewSize;

fs::path ProgramSettingsPath()
{
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer, MAX_PATH);

    fs::path folder = (length > 0 && length < MAX_PATH) ? fs::path(buffer) : fs::temp_directory_path();
    folder /= L"AlibreCAM";

    std::error_code ignored;
    fs::create_directories(folder, ignored);
    return folder / L"settings.json";
}

BEGIN_MESSAGE_MAP(CSettingsDlg, CDialog)
END_MESSAGE_MAP()

CSettingsDlg::CSettingsDlg(const camcore::ProgramSettings& settings, CWnd* pParent)
    : CDialog(IDD_SETTINGS, pParent)
    , m_settings(settings)
{
}

BOOL CSettingsDlg::OnInitDialog()
{
    CDialog::OnInitDialog();
    const int size = (m_settings.viewSize == ViewSize::Large)    ? IDC_VIEW_LARGE
                     : (m_settings.viewSize == ViewSize::Medium) ? IDC_VIEW_MEDIUM
                                                                 : IDC_VIEW_SMALL;
    CheckRadioButton(IDC_VIEW_SMALL, IDC_VIEW_LARGE, size);
    return TRUE;
}

void CSettingsDlg::OnOK()
{
    switch (GetCheckedRadioButton(IDC_VIEW_SMALL, IDC_VIEW_LARGE))
    {
    case IDC_VIEW_MEDIUM: m_settings.viewSize = ViewSize::Medium; break;
    case IDC_VIEW_LARGE:  m_settings.viewSize = ViewSize::Large; break;
    default:              m_settings.viewSize = ViewSize::Small; break;
    }
    CDialog::OnOK();
}
