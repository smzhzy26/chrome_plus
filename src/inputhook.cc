#include "inputhook.h"

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "utils.h"

namespace {

template <typename Handler>
struct HandlerEntry {
  Handler handler;
  int priority;
};

std::vector<HandlerEntry<KeyboardHandler>> keyboard_handlers;
std::vector<HandlerEntry<MouseHandler>> mouse_handlers;

HHOOK keyboard_hook = nullptr;
HHOOK mouse_hook = nullptr;

LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode == HC_ACTION) {
    for (const auto& entry : keyboard_handlers) {
      if (entry.handler(wParam, lParam)) {
        return 1;
      }
    }
  }
  return CallNextHookEx(keyboard_hook, nCode, wParam, lParam);
}

LRESULT CALLBACK MouseProc(int nCode, WPARAM wParam, LPARAM lParam) {
  if (nCode != HC_ACTION) {
    return CallNextHookEx(mouse_hook, nCode, wParam, lParam);
  }

  if (wParam == WM_NCMOUSEMOVE) {
    return CallNextHookEx(mouse_hook, nCode, wParam, lParam);
  }

  PMOUSEHOOKSTRUCT pmouse = reinterpret_cast<PMOUSEHOOKSTRUCT>(lParam);

  if (pmouse->dwExtraInfo == GetMagicCode()) {
    return CallNextHookEx(mouse_hook, nCode, wParam, lParam);
  }

  for (const auto& entry : mouse_handlers) {
    if (entry.handler(wParam, lParam)) {
      return 1;
    }
  }

  return CallNextHookEx(mouse_hook, nCode, wParam, lParam);
}

}  // namespace

void RegisterKeyboardHandler(KeyboardHandler handler,
                             HandlerPriority priority) {
  keyboard_handlers.emplace_back(std::move(handler), static_cast<int>(priority));
  std::ranges::sort(keyboard_handlers, [](const auto& a, const auto& b) {
    return a.priority < b.priority;
  });
}

void RegisterMouseHandler(MouseHandler handler, HandlerPriority priority) {
  mouse_handlers.emplace_back(std::move(handler), static_cast<int>(priority));
  std::ranges::sort(mouse_handlers, [](const auto& a, const auto& b) {
    return a.priority < b.priority;
  });
}

bool IsKeyPressed(int vk) {
  return vk && (::GetKeyState(vk) & 0x8000) != 0;
}

void InstallInputHooks() {
  // Both results were discarded and nothing was logged, so a failure here left
  // every keyboard and mouse feature dead with nothing to explain it: the user
  // sees key mappings, the boss key and the translate key all doing nothing, and
  // no way to tell that from having configured them wrongly. `hInstance` can be
  // null too, which makes the call fail in a way that is easy to miss.
  if (hInstance == nullptr) {
    WarnLog(L"InputHooks: the module handle is null; input hooks not installed");
    return;
  }

  keyboard_hook = SetWindowsHookEx(WH_KEYBOARD, KeyboardProc, hInstance,
                                   GetCurrentThreadId());
  if (keyboard_hook == nullptr) {
    WarnLog(L"InputHooks: SetWindowsHookEx(WH_KEYBOARD) failed with error " +
            std::to_wstring(GetLastError()) +
            L"; key mappings and the boss key will not work");
  }

  // WH_MOUSE, and `GetCurrentThreadId()` is deliberate rather than incidental:
  // WH_MOUSE is per-thread and only reports input for windows owned by the
  // installing thread. Measured with two windows on two threads and both hook
  // types installed on one of them, a synthesised wheel over the hook thread's
  // own window reached WH_MOUSE once and WH_MOUSE_LL zero times, while a wheel
  // over the other thread's window reached WH_MOUSE zero times and WH_MOUSE_LL
  // once. So every mouse feature here depends on Chrome's window being owned by
  // the thread that installs this hook.
  mouse_hook =
      SetWindowsHookEx(WH_MOUSE, MouseProc, hInstance, GetCurrentThreadId());
  if (mouse_hook == nullptr) {
    WarnLog(L"InputHooks: SetWindowsHookEx(WH_MOUSE) failed with error " +
            std::to_wstring(GetLastError()) +
            L"; mouse gestures will not work");
  }
}
