// CommandBase.cpp : default IAlibreAddOnCommand behaviour.

#include "stdafx.h"
#include "CommandBase.h"
#include "Log.h"

namespace
{
    HRESULT NotHandled(VARIANT_BOOL* pHandled)
    {
        if (pHandled == nullptr)
            return E_POINTER;
        *pHandled = VARIANT_FALSE;
        return S_OK;
    }
}

CCommandBase::CCommandBase(const wchar_t* name)
    : m_name(name)
    , m_refCount(0)
{
}

CCommandBase::~CCommandBase()
{
}

/////////////////////////////////////////////////////////////////////////////
// IUnknown

STDMETHODIMP CCommandBase::QueryInterface(REFIID riid, void** ppObj)
{
    if (ppObj == nullptr)
        return E_POINTER;

    if (riid == IID_IUnknown || riid == IID_IDispatch || riid == __uuidof(IAlibreAddOnCommand))
    {
        *ppObj = static_cast<IAlibreAddOnCommand*>(this);
        AddRef();
        return S_OK;
    }

    *ppObj = nullptr;
    return E_NOINTERFACE;
}

STDMETHODIMP_(ULONG) CCommandBase::AddRef()
{
    return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
}

STDMETHODIMP_(ULONG) CCommandBase::Release()
{
    const long count = InterlockedDecrement(&m_refCount);
    if (count == 0)
        delete this;
    return static_cast<ULONG>(count);
}

/////////////////////////////////////////////////////////////////////////////
// IDispatch

STDMETHODIMP CCommandBase::GetTypeInfoCount(UINT* pctinfo)
{
    if (pctinfo == nullptr)
        return E_POINTER;
    *pctinfo = 0;
    return S_OK;
}

STDMETHODIMP CCommandBase::GetTypeInfo(UINT, LCID, ITypeInfo** ppTInfo)
{
    if (ppTInfo != nullptr)
        *ppTInfo = nullptr;
    return E_NOTIMPL;
}

STDMETHODIMP CCommandBase::GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*)
{
    return E_NOTIMPL;
}

STDMETHODIMP CCommandBase::Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*,
                                  EXCEPINFO*, UINT*)
{
    return E_NOTIMPL;
}

/////////////////////////////////////////////////////////////////////////////
// IAlibreAddOnCommand

STDMETHODIMP CCommandBase::putref_CommandSite(struct IADAddOnCommandSite* pSite)
{
    m_site = pSite;
    return S_OK;
}

STDMETHODIMP CCommandBase::get_CommandSite(struct IADAddOnCommandSite** ppSite)
{
    if (ppSite == nullptr)
        return E_POINTER;
    *ppSite = m_site;
    if (*ppSite != nullptr)
        (*ppSite)->AddRef();
    return S_OK;
}

STDMETHODIMP CCommandBase::IsTwoWayToggle(VARIANT_BOOL* pValue)
{
    if (pValue == nullptr)
        return E_POINTER;
    *pValue = VARIANT_FALSE;
    return S_OK;
}

STDMETHODIMP CCommandBase::AddTab(VARIANT_BOOL* pValue)
{
    if (pValue == nullptr)
        return E_POINTER;
    *pValue = VARIANT_FALSE;
    return S_OK;
}

STDMETHODIMP CCommandBase::OnShowUI(__int64)
{
    return S_OK;
}

STDMETHODIMP CCommandBase::OnRender(long, long, long, long, long)
{
    return S_OK;
}

STDMETHODIMP CCommandBase::OnClick(long, long, enum ADDONMouseButtons, VARIANT_BOOL* pHandled)
{
    return NotHandled(pHandled);
}

STDMETHODIMP CCommandBase::OnDoubleClick(long, long, VARIANT_BOOL* pHandled)
{
    return NotHandled(pHandled);
}

STDMETHODIMP CCommandBase::OnMouseDown(long, long, enum ADDONMouseButtons, VARIANT_BOOL* pHandled)
{
    return NotHandled(pHandled);
}

STDMETHODIMP CCommandBase::OnMouseMove(long, long, enum ADDONMouseButtons, VARIANT_BOOL* pHandled)
{
    return NotHandled(pHandled);
}

STDMETHODIMP CCommandBase::OnMouseUp(long, long, enum ADDONMouseButtons, VARIANT_BOOL* pHandled)
{
    return NotHandled(pHandled);
}

STDMETHODIMP CCommandBase::OnSelectionChange()
{
    return S_OK;
}

STDMETHODIMP CCommandBase::OnTerminate()
{
    Log(L"[%s] OnTerminate", m_name);
    return S_OK;
}

STDMETHODIMP CCommandBase::OnComplete()
{
    return S_OK;
}

STDMETHODIMP CCommandBase::OnKeyDown(long, VARIANT_BOOL* pHandled)
{
    return NotHandled(pHandled);
}

STDMETHODIMP CCommandBase::OnKeyUp(long, VARIANT_BOOL* pHandled)
{
    return NotHandled(pHandled);
}

STDMETHODIMP CCommandBase::OnEscape(VARIANT_BOOL* pHandled)
{
    return NotHandled(pHandled);
}

STDMETHODIMP CCommandBase::OnMouseWheel(double, VARIANT_BOOL* pHandled)
{
    return NotHandled(pHandled);
}

STDMETHODIMP CCommandBase::get_TabName(BSTR* pName)
{
    if (pName == nullptr)
        return E_POINTER;
    *pName = nullptr;
    return S_OK;
}

STDMETHODIMP CCommandBase::get_Extents(SAFEARRAY** ppExtents)
{
    if (ppExtents == nullptr)
        return E_POINTER;
    *ppExtents = nullptr;
    return S_OK;
}

STDMETHODIMP CCommandBase::On3DRender()
{
    return S_OK;
}
