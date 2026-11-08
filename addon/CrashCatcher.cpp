// CrashCatcher.cpp : records a crash inside AlibreCAM's own code, so it can be found.

#include "stdafx.h"
#include "CrashCatcher.h"
#include "Log.h"

#include <DbgHelp.h>

#pragma comment(lib, "dbghelp.lib")

namespace
{
    PVOID g_handler = nullptr;
    ULONG_PTR g_moduleStart = 0;
    ULONG_PTR g_moduleEnd = 0;
    volatile LONG g_caught = 0;

    bool InModule(ULONG_PTR address)
    {
        return address >= g_moduleStart && address < g_moduleEnd;
    }

    // An address as module+offset, for looking up against the PDB.
    std::wstring Describe(ULONG_PTR address)
    {
        wchar_t text[300] = {};
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(address), &module) &&
            module != nullptr)
        {
            wchar_t path[MAX_PATH] = {};
            GetModuleFileNameW(module, path, MAX_PATH);
            const wchar_t* name = wcsrchr(path, L'\\');
            swprintf_s(text, L"%s+0x%llx", name != nullptr ? name + 1 : path,
                       static_cast<unsigned long long>(address - reinterpret_cast<ULONG_PTR>(module)));
        }
        else
        {
            swprintf_s(text, L"0x%llx", static_cast<unsigned long long>(address));
        }
        return text;
    }

    LONG CALLBACK OnException(EXCEPTION_POINTERS* pInfo)
    {
        const DWORD code = pInfo->ExceptionRecord->ExceptionCode;
        const bool fatal = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                           code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_STACK_OVERFLOW ||
                           code == EXCEPTION_ARRAY_BOUNDS_EXCEEDED || code == EXCEPTION_PRIV_INSTRUCTION;
        const ULONG_PTR at = reinterpret_cast<ULONG_PTR>(pInfo->ExceptionRecord->ExceptionAddress);
        if (!fatal || !InModule(at) || InterlockedExchange(&g_caught, 1) != 0)
            return EXCEPTION_CONTINUE_SEARCH;

        Log(L"[crash] exception 0x%08X in AlibreCAM at %s", code, Describe(at).c_str());
        if (code == EXCEPTION_ACCESS_VIOLATION && pInfo->ExceptionRecord->NumberParameters >= 2)
            Log(L"[crash] %s address 0x%llx",
                pInfo->ExceptionRecord->ExceptionInformation[0] == 1 ? L"writing" : L"reading",
                static_cast<unsigned long long>(pInfo->ExceptionRecord->ExceptionInformation[1]));

        // The call stack, unwound from the fault.
        CONTEXT context = *pInfo->ContextRecord;
        for (int frame = 0; frame < 40 && context.Rip != 0; ++frame)
        {
            Log(L"[crash]   %2d %s", frame, Describe(context.Rip).c_str());
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION pFunction = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
            if (pFunction == nullptr)
            {
                // A leaf function: its return address is on top of the stack.
                context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
                context.Rsp += 8;
                continue;
            }
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, pFunction, &context, &handlerData,
                             &establisher, nullptr);
        }

        // And a small dump, with the fault.
        const std::wstring path = LogFolderPath() + L"crash.dmp";
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            MINIDUMP_EXCEPTION_INFORMATION exception{};
            exception.ThreadId = GetCurrentThreadId();
            exception.ExceptionPointers = pInfo;
            exception.ClientPointers = FALSE;
            const BOOL written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                                                   static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                                                              MiniDumpWithThreadInfo),
                                                   &exception, nullptr, nullptr);
            CloseHandle(file);
            Log(L"[crash] %s %s", written ? L"dump written to" : L"dump couldn't be written to", path.c_str());
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

void InstallCrashCatcher()
{
    if (g_handler != nullptr)
        return;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&InstallCrashCatcher), &module))
        return;
    const auto* pDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    const auto* pNt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(module) + pDos->e_lfanew);
    g_moduleStart = reinterpret_cast<ULONG_PTR>(module);
    g_moduleEnd = g_moduleStart + pNt->OptionalHeader.SizeOfImage;
    g_handler = AddVectoredExceptionHandler(1, OnException);
}

void RemoveCrashCatcher()
{
    if (g_handler != nullptr)
    {
        RemoveVectoredExceptionHandler(g_handler);
        g_handler = nullptr;
    }
}
