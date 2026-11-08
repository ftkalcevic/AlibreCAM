// CAMForAlibre.h : main header for the CAMForAlibre add-on DLL.

#pragma once

#ifndef __AFXWIN_H__
#error "include 'stdafx.h' before including this file for PCH"
#endif

#include "resource.h"

class CAddOnInterface;

// Version shown in the About box. Keep in step with VERSIONINFO in CAMForAlibre.rc.
#define CAMFORALIBRE_VERSION_STRING  L"1.0.0"

/////////////////////////////////////////////////////////////////////////////
// CCAMForAlibreApp - the MFC application object for this regular DLL.

class CCAMForAlibreApp : public CWinApp
{
public:
    CCAMForAlibreApp();

    BOOL InitInstance() override;

    // Alibre's root automation object, handed to us in AddOnLoad.
    IADRootPtr          m_pRoot;

    // Alibre's main window (parent for our dialogs).
    HWND                m_hAlibreWnd;

    // Alibre version string read once at load time (IADRoot.Version).
    CString             m_strAlibreVersion;

    // The single IAlibreAddOn instance. We hold one reference of our own.
    CAddOnInterface*    m_pAddOn;

    DECLARE_MESSAGE_MAP()
};

extern CCAMForAlibreApp theApp;

/////////////////////////////////////////////////////////////////////////////
// Entry points called by Alibre Design. Exported undecorated via CAMForAlibre.def.

extern "C"
{
    void AddOnLoad(HWND windowHandle,
                   void* pAutomationHook,
                   void* reserved);

    void AddOnInvoke(HWND windowHandle,
                     void* pAutomationHook,
                     LPCSTR sessionName,
                     BOOL isLicensed,
                     void* reserved1,
                     void* reserved2);

    void AddOnUnload(HWND windowHandle,
                     BOOL forceUnload,
                     BOOL* cancel,
                     void* reserved1,
                     void* reserved2);

    IUnknown* GetAddOnInterface();
}
