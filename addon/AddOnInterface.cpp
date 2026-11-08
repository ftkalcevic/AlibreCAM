// AddOnInterface.cpp : IAlibreAddOn implementation for CAMForAlibre.

#include "stdafx.h"
#include "CAMForAlibre.h"
#include "AddOnInterface.h"
#include "AboutDlg.h"
#include "CamTabCommand.h"
#include "Log.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

CAddOnInterface::CAddOnInterface()
    : m_nRefCount(0)
{
}

CAddOnInterface::~CAddOnInterface()
{
}

/////////////////////////////////////////////////////////////////////////////
// IUnknown

STDMETHODIMP CAddOnInterface::QueryInterface(REFIID riid, void** ppObj)
{
    if (ppObj == nullptr)
        return E_POINTER;

    if (riid == IID_IUnknown || riid == IID_IDispatch || riid == __uuidof(IAlibreAddOn))
    {
        *ppObj = static_cast<IAlibreAddOn*>(this);
        AddRef();
        return S_OK;
    }

    *ppObj = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) CAddOnInterface::AddRef()
{
    return static_cast<ULONG>(InterlockedIncrement(&m_nRefCount));
}

STDMETHODIMP_(ULONG) CAddOnInterface::Release()
{
    long count = InterlockedDecrement(&m_nRefCount);
    if (count == 0)
        delete this;
    return static_cast<ULONG>(count);
}

/////////////////////////////////////////////////////////////////////////////
// IDispatch (not used by Alibre - vtable only)

STDMETHODIMP CAddOnInterface::GetTypeInfoCount(UINT* pctinfo)
{
    if (pctinfo == nullptr)
        return E_POINTER;
    *pctinfo = 0;
    return S_OK;
}

STDMETHODIMP CAddOnInterface::GetTypeInfo(UINT /*iTInfo*/, LCID /*lcid*/, ITypeInfo** ppTInfo)
{
    if (ppTInfo != nullptr)
        *ppTInfo = nullptr;
    return E_NOTIMPL;
}

STDMETHODIMP CAddOnInterface::GetIDsOfNames(REFIID /*riid*/, LPOLESTR* /*rgszNames*/,
                                            UINT /*cNames*/, LCID /*lcid*/, DISPID* /*rgDispId*/)
{
    return E_NOTIMPL;
}

STDMETHODIMP CAddOnInterface::Invoke(DISPID /*dispIdMember*/, REFIID /*riid*/, LCID /*lcid*/,
                                     WORD /*wFlags*/, DISPPARAMS* /*pDispParams*/,
                                     VARIANT* /*pVarResult*/, EXCEPINFO* /*pExcepInfo*/,
                                     UINT* /*puArgErr*/)
{
    return E_NOTIMPL;
}

/////////////////////////////////////////////////////////////////////////////
// IAlibreAddOn - menu structure
//
// Alibre also queries ids it does not own, so every [out] parameter is
// initialised before anything else.

STDMETHODIMP CAddOnInterface::get_RootMenuItem(long* pRootMenuID)
{
    if (pRootMenuID == nullptr)
        return E_POINTER;
    *pRootMenuID = MENU_ID_ROOT;
    return S_OK;
}

STDMETHODIMP CAddOnInterface::HasSubMenus(long menuID, VARIANT_BOOL* pHasSubMenus)
{
    if (pHasSubMenus == nullptr)
        return E_POINTER;
    *pHasSubMenus = (menuID == MENU_ID_ROOT) ? VARIANT_TRUE : VARIANT_FALSE;
    return S_OK;
}

STDMETHODIMP CAddOnInterface::SubMenuItems(long menuID, SAFEARRAY** pSubMenuIDs)
{
    if (pSubMenuIDs == nullptr)
        return E_POINTER;
    *pSubMenuIDs = nullptr;

    if (menuID != MENU_ID_ROOT)
        return S_OK;

    const long children[] = { MENU_ID_CAM_TAB, MENU_ID_ABOUT };
    const ULONG count = static_cast<ULONG>(sizeof(children) / sizeof(children[0]));

    // The caller takes ownership of the array.
    SAFEARRAY* psa = SafeArrayCreateVector(VT_I4, 0, count);
    if (psa == nullptr)
        return E_OUTOFMEMORY;

    long* pData = nullptr;
    HRESULT hr = SafeArrayAccessData(psa, reinterpret_cast<void**>(&pData));
    if (FAILED(hr))
    {
        SafeArrayDestroy(psa);
        return hr;
    }
    for (ULONG i = 0; i < count; ++i)
        pData[i] = children[i];
    SafeArrayUnaccessData(psa);

    *pSubMenuIDs = psa;
    return S_OK;
}

STDMETHODIMP CAddOnInterface::MenuItemText(long menuID, BSTR* pMenuDisplayText)
{
    if (pMenuDisplayText == nullptr)
        return E_POINTER;
    *pMenuDisplayText = nullptr;

    switch (menuID)
    {
    case MENU_ID_ROOT:   *pMenuDisplayText = _bstr_t(L"CAMForAlibre").Detach(); break;
    case MENU_ID_CAM_TAB: *pMenuDisplayText = _bstr_t(L"CAM Tab").Detach(); break;
    case MENU_ID_ABOUT:  *pMenuDisplayText = _bstr_t(L"About").Detach();    break;
    default: break;
    }
    return S_OK;
}

STDMETHODIMP CAddOnInterface::MenuItemState(long menuID, BSTR sessionIdentifier,
                                            enum ADDONMenuStates* pType)
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    if (pType == nullptr)
        return E_POINTER;

    // The root must be enabled too, or the whole drop-down/tab greys out.
    // The About box works in every workspace (including Home, which passes an
    // empty session id). CAM works on assemblies only, on a supported Alibre.
    if (menuID == MENU_ID_ROOT || menuID == MENU_ID_ABOUT)
        *pType = ADDONMenuStates_ADDON_MENU_ENABLED;
    else if (menuID == MENU_ID_CAM_TAB && IsAlibreVersionSupported() && IsAssemblySession(sessionIdentifier))
        *pType = ADDONMenuStates_ADDON_MENU_ENABLED;
    else
        *pType = ADDONMenuStates_ADDON_MENU_GRAYED;

    return S_OK;
}

STDMETHODIMP CAddOnInterface::MenuItemToolTip(long menuID, BSTR* pToolTip)
{
    if (pToolTip == nullptr)
        return E_POINTER;
    *pToolTip = nullptr;

    switch (menuID)
    {
    case MENU_ID_ROOT:
        *pToolTip = _bstr_t(L"CAMForAlibre add-on").Detach();
        break;
    case MENU_ID_CAM_TAB:
        *pToolTip = IsAlibreVersionSupported()
                        ? _bstr_t(L"Open or close the CAM tab in the Design Explorer. Assemblies only.").Detach()
                        : _bstr_t(L"CAMForAlibre needs Alibre Design 27.0.1.27039 or later.").Detach();
        break;
    case MENU_ID_ABOUT:
        *pToolTip = _bstr_t(L"Show information about the CAMForAlibre add-on").Detach();
        break;
    default:
        break;
    }
    return S_OK;
}

STDMETHODIMP CAddOnInterface::PopupMenu(long /*menuID*/, VARIANT_BOOL* pIsPopup)
{
    if (pIsPopup == nullptr)
        return E_POINTER;
    *pIsPopup = VARIANT_FALSE;
    return S_OK;
}

/////////////////////////////////////////////////////////////////////////////
// IAlibreAddOn - commands

STDMETHODIMP CAddOnInterface::InvokeCommand(long menuID, BSTR sessionIdentifier,
                                            struct IAlibreAddOnCommand** pCommand)
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    if (pCommand == nullptr)
        return E_POINTER;

    // Stateless commands do the work here and hand Alibre no command object.
    *pCommand = nullptr;

    if (menuID == MENU_ID_ABOUT)
    {
        ShowAboutBox();
        return S_OK;
    }

    if (menuID == MENU_ID_CAM_TAB)
    {
        const std::wstring sessionId = (sessionIdentifier != nullptr) ? sessionIdentifier : L"";
        const auto existing = m_tabs.find(sessionId);

        CCamTabCommand* pTab = nullptr;
        if (existing != m_tabs.end())
        {
            // Returning the active toggle command again closes the tab.
            pTab = existing->second;
        }
        else if (IsAlibreVersionSupported() && IsAssemblySession(sessionIdentifier))
        {
            pTab = new CCamTabCommand(this, sessionId);
            pTab->AddRef();     // the map's reference
            m_tabs[sessionId] = pTab;
        }

        if (pTab != nullptr)
            return pTab->QueryInterface(__uuidof(IAlibreAddOnCommand), reinterpret_cast<void**>(pCommand));
    }

    return S_OK;
}

void CAddOnInterface::RemoveTabForSession(const std::wstring& sessionId)
{
    const auto entry = m_tabs.find(sessionId);
    if (entry != m_tabs.end())
    {
        CCamTabCommand* pTab = entry->second;
        m_tabs.erase(entry);
        pTab->Release();
    }
}

void CAddOnInterface::CloseAllTabs()
{
    std::map<std::wstring, CCamTabCommand*> tabs;
    tabs.swap(m_tabs);

    for (auto& entry : tabs)
    {
        entry.second->CloseFromAddOn();
        entry.second->Release();
    }
}

bool CAddOnInterface::IsAssemblySession(BSTR sessionIdentifier)
{
    if (sessionIdentifier == nullptr || SysStringLen(sessionIdentifier) == 0 || theApp.m_pRoot == nullptr)
        return false;

    try
    {
        // An unknown identifier throws out of GetItem.
        IADSessionPtr session = theApp.m_pRoot->GetSessions()->GetItem(_variant_t(sessionIdentifier));
        return session != nullptr && session->GetSessionType() == ADObjectSubType_AD_ASSEMBLY;
    }
    catch (const _com_error&)
    {
        return false;
    }
}

bool CAddOnInterface::IsAlibreVersionSupported()
{
    // The build number is the largest number in the version string, such as
    // "PRODUCTVERSION 27,0,1,27039". If there's no number at all, allow the
    // add-on rather than blocking a working version.
    const CString& version = theApp.m_strAlibreVersion;

    long build = -1;
    long current = -1;
    for (int i = 0; i <= version.GetLength(); ++i)
    {
        if (i < version.GetLength() && iswdigit(version[i]))
        {
            current = ((current < 0) ? 0 : current * 10) + (version[i] - L'0');
        }
        else
        {
            if (current > build)
                build = current;
            current = -1;
        }
    }

    return build < 0 || build >= MINIMUM_ALIBRE_BUILD;
}

void CAddOnInterface::ShowAboutBox()
{
    CWnd* pParent = (theApp.m_hAlibreWnd != nullptr)
                        ? CWnd::FromHandle(theApp.m_hAlibreWnd)
                        : nullptr;

    CAboutDlg dlg(pParent);
    dlg.DoModal();
}

/////////////////////////////////////////////////////////////////////////////
// IAlibreAddOn - persistence (this add-on stores nothing in documents)

STDMETHODIMP CAddOnInterface::HasPersistentDataToSave(BSTR /*sessionIdentifier*/,
                                                      VARIANT_BOOL* pHasDataToSave)
{
    if (pHasDataToSave == nullptr)
        return E_POINTER;
    *pHasDataToSave = VARIANT_FALSE;
    return S_OK;
}

STDMETHODIMP CAddOnInterface::LoadData(struct IStream* /*pCustomData*/, BSTR /*sessionIdentifier*/)
{
    return S_OK;
}

STDMETHODIMP CAddOnInterface::SaveData(struct IStream* /*pCustomData*/, BSTR /*sessionIdentifier*/)
{
    return S_OK;
}

STDMETHODIMP CAddOnInterface::setIsAddOnLicensed(VARIANT_BOOL /*isLicensed*/)
{
    return S_OK;
}

/////////////////////////////////////////////////////////////////////////////
// IAlibreAddOn - spec v2: icons and the dedicated ribbon tab

STDMETHODIMP CAddOnInterface::MenuIcon(long menuID, BSTR* pMenuIconPath)
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    if (pMenuIconPath == nullptr)
        return E_POINTER;
    *pMenuIconPath = nullptr;

    if (menuID != MENU_ID_ROOT && menuID != MENU_ID_ABOUT && menuID != MENU_ID_CAM_TAB)
        return S_OK;

    // The icon is deployed beside the DLL. Resolve it from our own module,
    // never from the current working directory.
    CString iconPath = GetModuleFolder() + L"CAMForAlibre.ico";
    if (GetFileAttributesW(iconPath) != INVALID_FILE_ATTRIBUTES)
        *pMenuIconPath = _bstr_t(static_cast<LPCWSTR>(iconPath)).Detach();

    return S_OK;
}

STDMETHODIMP CAddOnInterface::UseDedicatedRibbonTab(VARIANT_BOOL* pFlag)
{
    if (pFlag == nullptr)
        return E_POINTER;
    *pFlag = VARIANT_TRUE;      // give CAMForAlibre its own ribbon tab
    return S_OK;
}

/////////////////////////////////////////////////////////////////////////////
// Helpers

// Folder containing this DLL, with a trailing backslash.
CString CAddOnInterface::GetModuleFolder()
{
    HMODULE hModule = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&CAddOnInterface::GetModuleFolder),
                            &hModule))
    {
        return CString();
    }

    wchar_t path[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(hModule, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return CString();

    CString folder(path);
    int slash = folder.ReverseFind(L'\\');
    return (slash >= 0) ? folder.Left(slash + 1) : CString();
}
