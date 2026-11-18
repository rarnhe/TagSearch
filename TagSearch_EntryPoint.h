#pragma once

#include <windows.h>

// Each executable supplies this externally linked entry point for the CRT.
int WINAPI wWinMain(
    _In_ HINSTANCE instance,
    _In_opt_ HINSTANCE previousInstance,
    _In_ LPWSTR commandLine,
    _In_ int showCommand);
