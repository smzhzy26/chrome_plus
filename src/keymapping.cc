#include "keymapping.h"

#include <windows.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "config.h"
#include "inputhook.h"
#include "utils.h"

namespace {
struct KeyMapping {
  UINT source_vk;
  UINT source_modifiers;
  UINT target_vk;
  UINT target_modifiers;
  int target_command;
};

struct TranslateKey {
  UINT vk = 0;
  UINT modifiers = 0;
};

std::vector<KeyMapping> key_mappings;
TranslateKey translate_key;

// A mapping re-injects input through `SendInput`, and the keyboard hook that
// fires the mapping cannot tell injected input from real input: a WH_KEYBOARD
// hook receives the key code in wParam and a bitfield of flags in lParam, not a
// KBDLLHOOKSTRUCT, so the `dwExtraInfo` marker that `SendMappedKey` sets is not
// visible to it. (The mouse hook can filter on `dwExtraInfo`, because
// MSLLHOOKSTRUCT does carry it; the keyboard equivalent exists only in
// WH_KEYBOARD_LL.) A mapping whose target is also a mapping source therefore
// feeds itself and the UI thread never returns. Measured on the pre-fix code:
// F2=F2, A=B + B=A and A=B + B=C + C=A each produced 2001 handler calls and 4000
// injections without terminating.
//
// Nothing downstream can catch this, so a mapping that would close a cycle is
// refused as it is loaded.
bool WouldCreateCycle(const std::vector<KeyMapping>& accepted,
                      const KeyMapping& candidate) {
  const auto key_of = [](UINT vk, UINT modifiers) {
    return (static_cast<uint32_t>(modifiers) << 16) | (vk & 0xFFFF);
  };
  const uint32_t source = key_of(candidate.source_vk, candidate.source_modifiers);
  const uint32_t target = key_of(candidate.target_vk, candidate.target_modifiers);
  if (source == target) {
    return true;
  }

  // Depth-first from the candidate's target: reaching the candidate's source
  // means the new edge closes a loop. Modifier state is part of the key because
  // `CheckModifiers` requires an exact match, so `Ctrl+A=B` and `A=C` are
  // distinct triggers.
  std::vector<uint32_t> stack{target};
  std::vector<uint32_t> visited;
  while (!stack.empty()) {
    const uint32_t current = stack.back();
    stack.pop_back();
    if (current == source) {
      return true;
    }
    if (std::ranges::find(visited, current) != visited.end()) {
      continue;
    }
    visited.push_back(current);
    for (const auto& mapping : accepted) {
      if (mapping.target_command != 0) {
        continue;  // commands inject no keys, so they cannot relay anything
      }
      if (key_of(mapping.source_vk, mapping.source_modifiers) == current) {
        stack.push_back(key_of(mapping.target_vk, mapping.target_modifiers));
      }
    }
  }
  return false;
}

bool CheckModifiers(UINT modifiers) {
  const bool shift_ok = !(modifiers & MOD_SHIFT) || IsKeyPressed(VK_SHIFT);
  const bool ctrl_ok = !(modifiers & MOD_CONTROL) || IsKeyPressed(VK_CONTROL);
  const bool alt_ok = !(modifiers & MOD_ALT) || IsKeyPressed(VK_MENU);
  const bool win_ok =
      !(modifiers & MOD_WIN) || IsKeyPressed(VK_LWIN) || IsKeyPressed(VK_RWIN);

  const bool no_extra_shift =
      (modifiers & MOD_SHIFT) || !IsKeyPressed(VK_SHIFT);
  const bool no_extra_ctrl =
      (modifiers & MOD_CONTROL) || !IsKeyPressed(VK_CONTROL);
  const bool no_extra_alt = (modifiers & MOD_ALT) || !IsKeyPressed(VK_MENU);
  const bool no_extra_win = (modifiers & MOD_WIN) ||
                            (!IsKeyPressed(VK_LWIN) && !IsKeyPressed(VK_RWIN));

  return shift_ok && ctrl_ok && alt_ok && win_ok && no_extra_shift &&
         no_extra_ctrl && no_extra_alt && no_extra_win;
}

void AddModifierInput(std::vector<INPUT>& inputs, WORD vk, bool key_up) {
  INPUT input = {};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = vk;
  input.ki.dwFlags = key_up ? KEYEVENTF_KEYUP : 0;
  input.ki.dwExtraInfo = GetMagicCode();
  inputs.emplace_back(input);
}

// Backstop for the loops the graph cannot see.
//
// `WouldCreateCycle` covers mappings that relay into each other, but not a loop
// built into this file: `TranslateKeyHandler` injects VK_RIGHT unconditionally,
// so `translate_key=right` re-triggers itself. Measured on the pre-fix code:
// 2001 handler calls, 4000 injections, and the command executed 2000 times.
// Nothing in the configuration graph can express that, so injections are also
// capped per unit of time. A loop never leaves the current call stack, so the
// cap is reached within the first millisecond; the limit is set far above real
// use so it cannot be felt. Windows' own repeat rate tops out near 30/s and a
// two-hop chain doubles it, so 512/s leaves roughly eight times the headroom.
constexpr int kMaxInjectionsPerWindow = 512;
constexpr ULONGLONG kInjectionWindowMs = 1000;

// The allowance is a plain struct rather than two function-local statics so a
// test can start each case with a fresh budget. Without that, a harness running
// several cases in one process would have the first looping case exhaust the
// allowance and every later case would look broken.
struct InjectionBudget {
  ULONGLONG window_start = 0;
  int used = 0;
};

InjectionBudget& Budget() {
  static InjectionBudget budget;
  return budget;
}

bool ConsumeInjectionBudget() {
  InjectionBudget& budget = Budget();
  const ULONGLONG now = ::GetTickCount64();
  if (now - budget.window_start >= kInjectionWindowMs) {
    budget.window_start = now;
    budget.used = 0;
  }
  if (budget.used >= kMaxInjectionsPerWindow) {
    return false;
  }
  ++budget.used;
  return true;
}

// Test hook: not called by the product.
void ResetInjectionBudgetForTesting() { Budget() = {}; }

void SendModifiers(UINT modifiers, bool key_up) {
  std::vector<INPUT> inputs;

  if (modifiers & MOD_CONTROL) {
    AddModifierInput(inputs, VK_CONTROL, key_up);
  }
  if (modifiers & MOD_SHIFT) {
    AddModifierInput(inputs, VK_SHIFT, key_up);
  }
  if (modifiers & MOD_ALT) {
    AddModifierInput(inputs, VK_MENU, key_up);
  }
  if (modifiers & MOD_WIN) {
    AddModifierInput(inputs, VK_LWIN, key_up);
  }

  if (!inputs.empty()) {
    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
  }
}

UINT GetAsyncHeldModifiers(UINT modifiers) {
  UINT held_modifiers = 0;
  if ((modifiers & MOD_CONTROL) && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
    held_modifiers |= MOD_CONTROL;
  }
  if ((modifiers & MOD_SHIFT) && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) {
    held_modifiers |= MOD_SHIFT;
  }
  if ((modifiers & MOD_ALT) && (GetAsyncKeyState(VK_MENU) & 0x8000)) {
    held_modifiers |= MOD_ALT;
  }
  if ((modifiers & MOD_WIN) && ((GetAsyncKeyState(VK_LWIN) & 0x8000) ||
                                (GetAsyncKeyState(VK_RWIN) & 0x8000))) {
    held_modifiers |= MOD_WIN;
  }
  return held_modifiers;
}

void SendMappedKey(const KeyMapping& mapping) {
  // - to_release: source has but target doesn't (user is holding, need to
  // release)
  // - to_press: target has but source doesn't (need to press)
  const UINT to_release = mapping.source_modifiers & ~mapping.target_modifiers;
  const UINT to_press = mapping.target_modifiers & ~mapping.source_modifiers;
  // Snapshot before SendInput. Injected key-up events update the asynchronous
  // state later, so querying it after injection races the raw input thread.
  // https://devblogs.microsoft.com/oldnewthing/20140213-00/?p=1773
  const UINT to_restore = GetAsyncHeldModifiers(to_release);

  SendModifiers(to_release, true);
  SendModifiers(to_press, false);

  std::vector<INPUT> inputs;
  {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(mapping.target_vk);
    input.ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    input.ki.dwExtraInfo = GetMagicCode();
    inputs.emplace_back(input);
  }
  {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(mapping.target_vk);
    input.ki.dwFlags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
    input.ki.dwExtraInfo = GetMagicCode();
    inputs.emplace_back(input);
  }
  SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));

  // Release modifiers we pressed
  SendModifiers(to_press, true);

  // Restore modifiers we released (if user still holding them)
  SendModifiers(to_restore, false);
}

void ExecuteMappedCommand(const KeyMapping& mapping) {
  // For commands, we need to release source modifiers temporarily
  const UINT to_release = mapping.source_modifiers;
  const UINT to_restore = GetAsyncHeldModifiers(to_release);

  SendModifiers(to_release, true);
  ExecuteCommand(mapping.target_command);
  SendModifiers(to_restore, false);
}

bool KeyMappingHandler(WPARAM wParam, LPARAM lParam) {
  if (lParam & 0x80000000) {
    return false;
  }

  for (const auto& mapping : key_mappings) {
    if (wParam == mapping.source_vk &&
        CheckModifiers(mapping.source_modifiers)) {
      // Out of budget means something is feeding itself. Returning false lets
      // the key through to the browser rather than swallowing it silently, which
      // is also how the user finds out that the mapping is looping.
      if (!ConsumeInjectionBudget()) {
        WarnLog(L"KeyMapping: injection rate limit reached, disabling further "
                L"mappings this second");
        return false;
      }
      if (mapping.target_command != 0) {
        ExecuteMappedCommand(mapping);
      } else {
        SendMappedKey(mapping);
      }
      return true;
    }
  }
  return false;
}

bool TranslateKeyHandler(WPARAM wParam, LPARAM lParam) {
  if (lParam & 0x80000000) {
    return false;
  }

  if (translate_key.vk == 0) {
    return false;
  }

  if (wParam != translate_key.vk || !CheckModifiers(translate_key.modifiers)) {
    return false;
  }

  // This handler injects VK_RIGHT below, so a translate key of RIGHT or of any
  // key the injection produces re-triggers it. Measured: 2001 calls and the
  // command run 2000 times before the pre-fix code was stopped by the harness.
  if (!ConsumeInjectionBudget()) {
    WarnLog(L"TranslateKey: injection rate limit reached, ignoring the key");
    return false;
  }

  ExecuteCommand(IDC_SHOW_TRANSLATE);
  keybd_event(VK_RIGHT, 0, 0, 0);
  keybd_event(VK_RIGHT, 0, KEYEVENTF_KEYUP, 0);
  return true;
}

int ParseCommand(std::wstring_view str) {
  int result = 0;
  for (const wchar_t c : str) {
    if (c < L'0' || c > L'9') {
      return 0;
    }
    result = result * 10 + (c - L'0');
  }
  return result;
}

void InitKeyMapping() {
  const auto& mappings = config.GetKeyMappings();

  for (const auto& [source, target] : mappings) {
    KeyMapping mapping = {};

    // Pass false for no_repeat since we don't need MOD_NOREPEAT in key mappings
    UINT source_parsed = ParseHotkeys(source, /*no_repeat=*/false);
    mapping.source_modifiers = LOWORD(source_parsed);
    mapping.source_vk = HIWORD(source_parsed);

    if (mapping.source_vk == 0) {
      DebugLog(L"KeyMapping: Invalid source key '{}'", source);
      continue;
    }

    if (target.starts_with(L"command:")) {
      std::wstring_view command_str = target;
      command_str.remove_prefix(8);
      mapping.target_command = ParseCommand(command_str);
      if (mapping.target_command == 0) {
        DebugLog(L"KeyMapping: Invalid command '{}'", target);
        continue;
      }
    } else {
      UINT target_parsed = ParseHotkeys(target, /*no_repeat=*/false);
      mapping.target_modifiers = LOWORD(target_parsed);
      mapping.target_vk = HIWORD(target_parsed);

      if (mapping.target_vk == 0) {
        DebugLog(L"KeyMapping: Invalid target key '{}'", target);
        continue;
      }
    }

    // `KeyMappingHandler` takes the first entry whose source matches, so a
    // repeated source leaves every later one dead. Dead entries are excluded
    // from the cycle graph, not just from dispatch: otherwise `F2=A, F2=B,
    // B=F2` looks like a cycle (B -> F2 -> B) even though the `B=F2` edge can
    // never fire, and a working configuration gets refused. That misjudgement is
    // what an earlier attempt at this check got wrong.
    const auto same_source = [&](const KeyMapping& other) {
      return other.source_vk == mapping.source_vk &&
             other.source_modifiers == mapping.source_modifiers;
    };
    if (std::ranges::any_of(key_mappings, same_source)) {
      DebugLog(L"KeyMapping: '{}' is shadowed by an earlier mapping and can "
               L"never fire; not loaded",
               source);
      continue;
    }

    if (WouldCreateCycle(key_mappings, mapping)) {
      // WarnLog takes a finished string rather than a format string, to keep
      // std::format out of a path that ships in release builds.
      std::wstring message =
          L"KeyMapping: '" + std::wstring(source) +
          L"' would inject into a key that maps back to it, which loops "
          L"forever; not loaded";
      WarnLog(message);
      continue;
    }

    key_mappings.emplace_back(mapping);
    DebugLog(L"KeyMapping: Loaded {} -> {}", source, target);
  }

  if (!key_mappings.empty()) {
    RegisterKeyboardHandler(KeyMappingHandler, HandlerPriority::kHigh);
    DebugLog(L"KeyMapping: Registered {} mappings", key_mappings.size());
  }
}

void InitTranslateKey() {
  const auto& translate_key_str = config.GetTranslateKey();
  if (translate_key_str.empty()) {
    return;
  }

  UINT parsed = ParseHotkeys(translate_key_str, /*no_repeat=*/false);
  translate_key.modifiers = LOWORD(parsed);
  translate_key.vk = HIWORD(parsed);

  if (translate_key.vk == 0) {
    DebugLog(L"TranslateKey: Invalid key '{}'", translate_key_str);
    return;
  }

  // `TranslateKeyHandler` presses VK_RIGHT before it returns, so a translate key
  // of RIGHT is a loop with no configuration involved: every injected press
  // re-enters the handler. The shipped ini suggests `translate_key=right`, so
  // this is not a theoretical case. A cheap exact check, independent of the
  // mapping graph, which cannot see this at all.
  if (translate_key.vk == VK_RIGHT && translate_key.modifiers == 0) {
    std::wstring message = L"TranslateKey: '" + translate_key_str +
                           L"' is the key this action injects, which would "
                           L"repeat without end; not registered";
    WarnLog(message);
    return;
  }

  RegisterKeyboardHandler(TranslateKeyHandler, HandlerPriority::kHigh);
  DebugLog(L"TranslateKey: Registered '{}'", translate_key_str);
}

}  // namespace

void KeyMapping() {
  InitKeyMapping();
  InitTranslateKey();
}