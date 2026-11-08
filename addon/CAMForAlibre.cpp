// AlibreCAM.cpp : DLL initialisation and the entry points Alibre Design calls.
//
// Every exported function starts with AFX_MANAGE_STATE so MFC uses this
// DLL's module state (resources, window maps) rather than Alibre's.

#include "stdafx.h"
#include "AlibreCAM.h"
#include "AddOnInterface.h"
#include "CrashCatcher.h"
#include "Log.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

BEGIN_MESSAGE_MAP(CAlibreCAMApp, CWinApp)
END_MESSAGE_MAP()

CAlibreCAMApp::CAlibreCAMApp()
    : m_hAlibreWnd(nullptr)
    , m_pAddOn(nullptr)
{
}

// The one and only application object
CAlibreCAMApp theApp;

BOOL CAlibreCAMApp::InitInstance()
{
    CWinApp::InitInstance();
    return TRUE;
}

/////////////////////////////////////////////////////////////////////////////
// Exported entry points

void AddOnLoad(HWND windowHandle, void* pAutomationHook, void* /*reserved*/)
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    theApp.m_hAlibreWnd = windowHandle;

    LogInit();
    InstallCrashCatcher();

    if (pAutomationHook == nullptr)
    {
        Log(L"AddOnLoad: no automation hook");
        return;
    }

    try
    {
        IAutomationHook* pHook = static_cast<IAutomationHook*>(pAutomationHook);
        theApp.m_pRoot = pHook->GetRoot();

        if (theApp.m_pRoot != nullptr)
        {
            _bstr_t version = theApp.m_pRoot->Getversion();   // lowercase "version" in AlibreX_64.tlb
            theApp.m_strAlibreVersion = static_cast<const wchar_t*>(version);
            Log(L"Alibre version: %s", static_cast<LPCWSTR>(theApp.m_strAlibreVersion));
        }
    }
    catch (const _com_error& error)
    {
        // Not fatal here: the About box just won't show the Alibre version.
        Log(L"AddOnLoad failed: hr=0x%08X", static_cast<unsigned>(error.Error()));
    }
}

void AddOnInvoke(HWND /*windowHandle*/, void* /*pAutomationHook*/, LPCSTR /*sessionName*/,
                 BOOL /*isLicensed*/, void* /*reserved1*/, void* /*reserved2*/)
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    // Only called when get_RootMenuItem returns 0 (add-on listed as a single
    // Tools > Add-Ons entry). This add-on has a menu tree, so nothing to do.
}

void AddOnUnload(HWND /*windowHandle*/, BOOL /*forceUnload*/, BOOL* cancel,
                 void* /*reserved1*/, void* /*reserved2*/)
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    if (cancel != nullptr)
        *cancel = FALSE;            // never veto unloading

    // Drop our references while Alibre is still fully alive, rather than
    // during DLL detach.
    Log(L"AddOnUnload");

    if (theApp.m_pAddOn != nullptr)
    {
        theApp.m_pAddOn->CloseAllTabs();
        theApp.m_pAddOn->Release();
        theApp.m_pAddOn = nullptr;
    }
    theApp.m_pRoot = nullptr;
    RemoveCrashCatcher();
}

IUnknown* GetAddOnInterface()
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    if (theApp.m_pAddOn == nullptr)
    {
        theApp.m_pAddOn = new CAddOnInterface();
        theApp.m_pAddOn->AddRef();  // our own reference, released in AddOnUnload
    }

    // Reference handed to the caller.
    theApp.m_pAddOn->AddRef();
    return static_cast<IAlibreAddOn*>(theApp.m_pAddOn);
}
