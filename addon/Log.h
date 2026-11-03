// Log.h : the add-on's log file, %LOCALAPPDATA%\AlibreCAM\AlibreCAM.log.
//
// Runtime behaviour inside Alibre can't be seen from outside it, so each line is
// written and flushed at once, and the log survives a crash.

#pragma once

void LogInit();
void Log(const wchar_t* format, ...);
