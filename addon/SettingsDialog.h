// SettingsDialog.h : the add-on's Settings dialog, opened from the CAM tab's gear.

#pragma once

#include <filesystem>

#include "resource.h"
#include "ProgramSettings.h"

// Where the settings are kept: %APPDATA%\AlibreCAM\settings.json. The folder is
// made if it isn't there.
std::filesystem::path ProgramSettingsPath();

class CSettingsDlg : public CDialog
{
public:
    CSettingsDlg(const camcore::ProgramSettings& settings, CWnd* pParent);

    const camcore::ProgramSettings& GetSettings() const { return m_settings; }

    enum { IDD = IDD_SETTINGS };

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;

    DECLARE_MESSAGE_MAP()

private:
    camcore::ProgramSettings m_settings;
};
