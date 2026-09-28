// SPDX-License-Identifier: MIT
// Small fixed-output Win32 window for the Prospero Win / PS5 WSI PE controls.
#pragma once
#include <windows.h>

enum { PE_WIDTH = 1920, PE_HEIGHT = 1080 };

static HWND pe_window(const wchar_t* title) {
  const wchar_t* name = L"PS5VK_DXVK_PE_CONTROL";
  HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSW cls = {};
  cls.lpfnWndProc = DefWindowProcW;
  cls.hInstance = instance;
  cls.lpszClassName = name;
  if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    return nullptr;
  HWND window = CreateWindowExW(0, name, title, WS_POPUP | WS_VISIBLE,
      0, 0, PE_WIDTH, PE_HEIGHT, nullptr, nullptr, instance, nullptr);
  if (window) {
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
  }
  return window;
}

static void pe_visible_frame() {
  MSG message;
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  Sleep(250);
}
