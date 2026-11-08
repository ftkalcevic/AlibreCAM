// Log.h : the add-on's log file, %LOCALAPPDATA%\CAMForAlibre\CAMForAlibre.log.
//
// Runtime behaviour inside Alibre can't be seen from outside it, so each line is
// written and flushed at once, and the log survives a crash.

#pragma once

#include <string>

void LogInit();
void Log(const wchar_t* format, ...);

// The log's folder, ending in a backslash.
std::wstring LogFolderPath();
