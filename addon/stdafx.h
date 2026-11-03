// stdafx.h : precompiled header for the AlibreCAM Alibre Design add-on.
//
// The Alibre type libraries are imported by file name only. The folder that
// holds them (C:\Program Files\Alibre Design <version>\Program\) is found by
// Alibre.props and passed to the compiler as an include directory, which is
// one of the places #import searches. See Alibre.props to override it.

#pragma once

#ifndef VC_EXTRALEAN
#define VC_EXTRALEAN            // Exclude rarely-used stuff from Windows headers
#endif

#include <SDKDDKVer.h>          // Defines _WIN32_WINNT for the newest SDK

#include <afxwin.h>             // MFC core and standard components
#include <afxext.h>             // MFC extensions
#include <afxcmn.h>             // CTreeCtrl, CListCtrl
#include <afxdlgs.h>            // CFileDialog
#include <comdef.h>             // _bstr_t, _com_error, smart pointers

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>

// The large automation API that drives Alibre (IADRoot, sessions, geometry...).
#import "AlibreX_64.tlb"
using namespace AlibreX;

// The small integration API (IAlibreAddOn, IAlibreAddOnCommand, menu states).
#import "AlibreAddOn_64.tlb" raw_interfaces_only
using namespace AlibreAddOn;

// Two benign warnings come from these imports and can be ignored:
//   C4278 'SendMessage': identifier in type library is already a macro
//   C4192 automatically excluding 'IStream' while importing type library
