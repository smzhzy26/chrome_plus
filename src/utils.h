#ifndef CHROME_PLUS_SRC_UTILS_H_
#define CHROME_PLUS_SRC_UTILS_H_

#include <windows.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Global variable declaration
extern HMODULE hInstance;

// Constants
consteval uint32_t GetMagicCode() {
  return 0x1603ABD9;
}

// Chrome command IDs
// https://source.chromium.org/chromium/chromium/src/+/main:chrome/app/chrome_command_ids.h?q=chrome_command_ids.h&ss=chromium%2Fchromium%2Fsrc
#define IDC_NEW_TAB 34014
#define IDC_CLOSE_TAB 34015
#define IDC_SELECT_NEXT_TAB 34016
#define IDC_SELECT_PREVIOUS_TAB 34017
#define IDC_SELECT_TAB_0 34018
#define IDC_SELECT_TAB_1 34019
#define IDC_SELECT_TAB_2 34020
#define IDC_SELECT_TAB_3 34021
#define IDC_SELECT_TAB_4 34022
#define IDC_SELECT_TAB_5 34023
#define IDC_SELECT_TAB_6 34024
#define IDC_SELECT_TAB_7 34025
#define IDC_SELECT_LAST_TAB 34026
#define IDC_FULLSCREEN 34030
#define IDC_SHOW_TRANSLATE 35009
#define IDC_WINDOW_CLOSE_OTHER_TABS 35023
#define IDC_CLOSE_FIND_OR_STOP 37003
#define IDC_UPGRADE_DIALOG 40024

// Global constants - use functions to avoid static initialization order issues
const std::wstring& GetAppDir();
const std::wstring& GetIniPath();

// String manipulation function declarations
// Specify the delimiter and wrapper to split the string.
std::vector<std::wstring> StringSplit(std::wstring_view str,
                                      const wchar_t delim,
                                      std::wstring_view enclosure = L"");
std::vector<std::string> StringSplit(std::string_view str,
                                     const char delim,
                                     std::string_view enclosure = "");

// HTML compression functions
void compression_html(std::string& html);

bool ReplaceStringInPlace(std::string& subject,
                          std::string_view search,
                          std::string_view replace);

bool ReplaceStringInPlace(std::wstring& subject,
                          std::wstring_view search,
                          std::wstring_view replace);

std::wstring QuoteSpaceIfNeeded(const std::wstring& str);

std::wstring JoinArgsString(const std::vector<std::wstring>& lines,
                            std::wstring_view delimiter);

// Memory and module search functions
std::span<uint8_t> SearchMemory(std::span<uint8_t> src,
                                std::span<const uint8_t> sub);

// Parse the INI file
std::wstring GetIniString(std::wstring_view section,
                          std::wstring_view key,
                          std::wstring_view default_value);

// Canonicalize the path. Returns nullopt when the path cannot be canonicalized;
// an empty string is never a valid result, which is what the previous
// `std::wstring` return made impossible to tell apart from failure.
//
// These three used to return `std::wstring` and silently produced an empty one
// past MAX_PATH, because each wrote into a fixed `wchar_t[MAX_PATH]`. The caller
// in config.cc then compared an empty string, found nothing, and fell through to
// `nullopt` -- so `data_dir` and `cache_dir` were discarded with no message
// whenever the installation path was long. Returning `std::optional` makes the
// compiler require every caller to consider that case.
std::optional<std::wstring> CanonicalizePath(const std::wstring& path);

// Get the absolute path, or nullopt if it cannot be produced.
std::optional<std::wstring> GetAbsolutePath(const std::wstring& path);

// Expand environment variables in the path, or nullopt if expansion fails.
std::optional<std::wstring> ExpandEnvironmentPath(const std::wstring& path);

// Debug log function
#if defined(_DEBUG)
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
template <typename... Args>
void DebugLog(std::wformat_string<Args...> fmt, Args&&... args) {
  static std::mutex log_mutex;
  std::lock_guard<std::mutex> lock(log_mutex);

  std::wstring log_content = std::format(
      L"[chrome++] {}", std::format(fmt, std::forward<Args>(args)...));

  std::filesystem::path log_path = GetAppDir();
  log_path /= L"Chrome++_Debug.log";

  if (std::wofstream log_file(log_path, std::ios::app); log_file.is_open()) {
    log_file.imbue(std::locale(""));
    log_file << log_content << L'\n';
  }
}
#else
inline void DebugLog(std::wstring_view, auto&&...) {}
#endif

// A log line that also reaches release builds. `DebugLog` is compiled away
// outside `_DEBUG`, which means a shipped build says nothing about a
// configuration it refused -- the user only sees a mapping that silently does
// not work. This takes an already-built string rather than a format string on
// purpose: `std::format` and `std::filesystem` in a path that is always
// compiled in added roughly 480 KB to the DLL, where concatenation costs a few
// hundred bytes.
void WarnLog(std::wstring_view message);

// Window and message processing functions
HWND GetTopWnd(HWND hwnd);
void ExecuteCommand(int id, HWND hwnd = 0);
void LaunchCommands(const std::wstring& get_commands);
[[nodiscard]] bool IsChromeWindow(HWND hwnd);

// Keyboard and mouse input functions
// Template function for sending combined key operations - kept in header
template <typename... T>
void SendKey(T&&... keys) {
  std::vector<typename std::common_type<T...>::type> keys_ = {
      std::forward<T>(keys)...};
  std::vector<INPUT> inputs{};
  inputs.reserve(keys_.size() * 2);
  for (auto& key : keys_) {
    INPUT input = {0};
    // Adjust mouse messages
    switch (key) {
      case VK_RBUTTON:
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = GetSystemMetrics(SM_SWAPBUTTON) == TRUE
                               ? MOUSEEVENTF_LEFTDOWN
                               : MOUSEEVENTF_RIGHTDOWN;
        input.mi.dwExtraInfo = GetMagicCode();
        break;
      case VK_LBUTTON:
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = GetSystemMetrics(SM_SWAPBUTTON) == TRUE
                               ? MOUSEEVENTF_RIGHTDOWN
                               : MOUSEEVENTF_LEFTDOWN;
        input.mi.dwExtraInfo = GetMagicCode();
        break;
      case VK_MBUTTON:
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN;
        input.mi.dwExtraInfo = GetMagicCode();
        break;
      default:
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = (WORD)key;
        input.ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
        input.ki.dwExtraInfo = GetMagicCode();
        break;
    }
    inputs.emplace_back(std::move(input));
  }
  for (auto& key : keys_) {
    INPUT input = {0};
    // Adjust mouse messages
    switch (key) {
      case VK_RBUTTON:
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = GetSystemMetrics(SM_SWAPBUTTON) == TRUE
                               ? MOUSEEVENTF_LEFTUP
                               : MOUSEEVENTF_RIGHTUP;
        input.mi.dwExtraInfo = GetMagicCode();
        break;
      case VK_LBUTTON:
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = GetSystemMetrics(SM_SWAPBUTTON) == TRUE
                               ? MOUSEEVENTF_RIGHTUP
                               : MOUSEEVENTF_LEFTUP;
        input.mi.dwExtraInfo = GetMagicCode();
        break;
      case VK_MBUTTON:
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
        input.mi.dwExtraInfo = GetMagicCode();
        break;
      default:
        input.type = INPUT_KEYBOARD;
        input.ki.dwFlags = KEYEVENTF_KEYUP;
        input.ki.wVk = (WORD)key;
        input.ki.dwFlags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
        input.ki.dwExtraInfo = GetMagicCode();
        break;
    }
    inputs.emplace_back(std::move(input));
  }
  SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

// Parse hotkey string like "Ctrl+Shift+A" into MAKELPARAM(modifiers, vk)
// Supports modifiers: shift, ctrl/control, alt, win
// Supports keys: F1-F24, A-Z, 0-9, arrow keys, special keys (esc, tab, etc.)
// Returns: LOWORD = modifiers, HIWORD = virtual key code
// no_repeat: if true, adds MOD_NOREPEAT flag (default for `RegisterHotKey`)
UINT ParseHotkeys(std::wstring_view keys, bool no_repeat = true);

#endif  // CHROME_PLUS_SRC_UTILS_H_
