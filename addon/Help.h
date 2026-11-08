// Help.h : opens the add-on's help page in the browser at a section.

#pragma once

// Opens help\CAMForAlibre.html, installed beside the add-on, at the section with
// this anchor, such as L"cam-tab" or L"set-origin". The default browser is
// started directly, because opening a file by its path drops the #anchor.
void ShowHelp(const wchar_t* section);
