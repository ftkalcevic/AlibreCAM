// AddOnInterface.cpp : IAlibreAddOn implementation for AlibreCAM.

#include "stdafx.h"
#include "AlibreCAM.h"
#include "AddOnInterface.h"
#include "AboutDlg.h"

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

    const long children[] = { MENU_ID_ABOUT };
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
    case MENU_ID_ROOT:   *pMenuDisplayText = _bstr_t(L"AlibreCAM").Detach(); break;
    case MENU_ID_ABOUT:  *pMenuDisplayText = _bstr_t(L"About").Detach();    break;
    default: break;
    }
    return S_OK;
}

STDMETHODIMP CAddOnInterface::MenuItemState(long menuID, BSTR /*sessionIdentifier*/,
                                            enum ADDONMenuStates* pType)
{
    if (pType == nullptr)
        return E_POINTER;

    // The root must be enabled too, or the whole drop-down/tab greys out.
    // The About box works in every workspace (including Home, which passes an
    // empty session id), so the session is not consulted.
    if (menuID == MENU_ID_ROOT || menuID == MENU_ID_ABOUT)
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
        *pToolTip = _bstr_t(L"AlibreCAM add-on").Detach();
        break;
    case MENU_ID_ABOUT:
        *pToolTip = _bstr_t(L"Show information about the AlibreCAM add-on").Detach();
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

STDMETHODIMP CAddOnInterface::InvokeCommand(long menuID, BSTR /*sessionIdentifier*/,
                                            struct IAlibreAddOnCommand** pCommand)
{
    AFX_MANAGE_STATE(AfxGetStaticModuleState());

    if (pCommand == nullptr)
        return E_POINTER;

    // Stateless command: do the work here and hand Alibre no command object.
    *pCommand = nullptr;

    if (menuID == MENU_ID_ABOUT)
        ShowAboutBox();

    return S_OK;
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

    if (menuID != MENU_ID_ROOT && menuID != MENU_ID_ABOUT)
        return S_OK;

    // The icon is deployed beside the DLL. Resolve it from our own module,
    // never from the current working directory.
    CString iconPath = GetModuleFolder() + L"AlibreCAM.ico";
    if (GetFileAttributesW(iconPath) != INVALID_FILE_ATTRIBUTES)
        *pMenuIconPath = _bstr_t(static_cast<LPCWSTR>(iconPath)).Detach();

    return S_OK;
}

STDMETHODIMP CAddOnInterface::UseDedicatedRibbonTab(VARIANT_BOOL* pFlag)
{
    if (pFlag == nullptr)
        return E_POINTER;
    *pFlag = VARIANT_TRUE;      // give AlibreCAM its own ribbon tab
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
