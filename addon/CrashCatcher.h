// CrashCatcher.h : records a crash inside CAMForAlibre's own code, so it can be found.
//
// Alibre turns a crash in an add-on's window code into its own .NET exception,
// which loses where it happened. This catches the first access violation (or
// similar fault) inside CAMForAlibre.dll as it happens, logs its address and the
// call stack as offsets in the DLL, and writes crash.dmp beside the log.

#pragma once

void InstallCrashCatcher();
void RemoveCrashCatcher();
