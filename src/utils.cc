#include "utils.h"

#include <windows.h>

#include <pathcch.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <functional>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Global variable definitions
HMODULE hInstance = nullptr;

// Global constants - use functions to avoid static initialization order issues
const std::wstring& GetAppDir() {
  static std::wstring app_dir = []() {
    // `GetModuleFileNameW` truncates to the buffer and signals that by returning
    // the buffer size, so the buffer is grown until the return value is smaller
    // than it. Passed MAX_PATH, any executable in a deeper directory produced a
    // truncated path that every other path helper then inherited.
    std::vector<wchar_t> path(MAX_PATH);
    for (;;) {
      const DWORD written =
          ::GetModuleFileNameW(nullptr, path.data(),
                               static_cast<DWORD>(path.size()));
      if (written == 0) {
        return std::wstring();
      }
      if (written < path.size()) {
        path.resize(written + 1);  // room for the NUL PathRemoveFileSpec needs
        break;
      }
      constexpr size_t kMaxPathLength = 1u << 16;
      if (path.size() >= kMaxPathLength) {
        break;
      }
      path.resize(path.size() * 2);
    }
    ::PathRemoveFileSpec(path.data());
    // `PathRemoveFileSpec` leaves a trailing separator when the executable sits
    // in a drive root (`D:\chrome.exe` -> `D:\`), so this is only a guard for
    // the degenerate case; measured, the root case already includes it.
    std::wstring dir(path.data());
    if (dir.size() == 2 && dir[1] == L':') {
      dir.push_back(L'\\');
    }
    return dir;
  }();
  return app_dir;
}

const std::wstring& GetIniPath() {
  static std::wstring ini_path = GetAppDir() + L"\\chrome++.ini";
  return ini_path;
}

// String manipulation functions
// Specify the delimiter and wrapper to split the string.
std::vector<std::wstring> StringSplit(std::wstring_view str,
                                      const wchar_t delim,
                                      std::wstring_view enclosure) {
  std::vector<std::wstring> result;
  auto parts = std::views::split(str, delim);
  for (const auto& part : parts) {
    std::wstring_view part_sv(part);
    if (!enclosure.empty()) {
      if (!part_sv.empty() && part_sv.front() == enclosure.front()) {
        part_sv.remove_prefix(1);
      }
      if (!part_sv.empty() && part_sv.back() == enclosure.back()) {
        part_sv.remove_suffix(1);
      }
    }
    result.emplace_back(part_sv);
  }
  return result;
}

std::vector<std::string> StringSplit(std::string_view str,
                                     const char delim,
                                     std::string_view enclosure) {
  std::vector<std::string> result;
  auto parts = std::views::split(str, delim);
  for (const auto& part : parts) {
    std::string_view part_sv(part);
    if (!enclosure.empty()) {
      if (!part_sv.empty() && part_sv.front() == enclosure.front()) {
        part_sv.remove_prefix(1);
      }
      if (!part_sv.empty() && part_sv.back() == enclosure.back()) {
        part_sv.remove_suffix(1);
      }
    }
    result.emplace_back(part_sv);
  }
  return result;
}

// Compression html.
std::string& ltrim(std::string& s) {
  auto it = std::ranges::find_if_not(
      s, [](unsigned char c) { return std::isspace(c); });
  s.erase(s.begin(), it);
  return s;
}

std::string& rtrim(std::string& s) {
  auto reversed_view = s | std::views::reverse;
  auto it = std::ranges::find_if_not(
      reversed_view, [](unsigned char c) { return std::isspace(c); });
  s.erase(it.base(), s.end());
  return s;
}

std::string& trim(std::string& s) {
  return ltrim(rtrim(s));
}

void compression_html(std::string& html) {
  auto lines = StringSplit(html, '\n');
  html.clear();
  for (auto& line : lines) {
    html += "\n";
    html += trim(line);
  }
}

bool ReplaceStringInPlace(std::string& subject,
                          std::string_view search,
                          std::string_view replace) {
  bool find = false;
  size_t pos = 0;
  while ((pos = subject.find(search, pos)) != std::string::npos) {
    subject.replace(pos, search.length(), replace);
    pos += replace.length();
    find = true;
  }
  return find;
}

bool ReplaceStringInPlace(std::wstring& subject,
                          std::wstring_view search,
                          std::wstring_view replace) {
  bool find = false;
  size_t pos = 0;
  while ((pos = subject.find(search, pos)) != std::wstring::npos) {
    subject.replace(pos, search.length(), replace);
    pos += replace.length();
    find = true;
  }
  return find;
}

std::wstring QuoteSpaceIfNeeded(const std::wstring& str) {
  if (!str.contains(L' ')) {
    return str;
  }

  std::wstring escaped(L"\"");
  for (auto c : str) {
    if (c == L'"') {
      escaped += L'"';
    }
    escaped += c;
  }
  escaped += L'"';
  return escaped;
}

std::wstring JoinArgsString(const std::vector<std::wstring>& lines,
                            std::wstring_view delimiter) {
  if (lines.empty()) {
    return L"";
  }
  return lines | std::views::transform(QuoteSpaceIfNeeded) |
         std::views::join_with(delimiter) | std::ranges::to<std::wstring>();
}

// Search memory.
std::span<uint8_t> SearchMemory(std::span<uint8_t> src,
                                std::span<const uint8_t> sub) {
  if (src.empty() || sub.empty() || src.size() < sub.size()) {
    return {};
  }
  auto it = std::search(src.begin(), src.end(),
                        std::boyer_moore_searcher(sub.begin(), sub.end()));
  if (it != src.end()) {
    return src.subspan(std::distance(src.begin(), it));
  }
  return {};
}

std::wstring GetIniString(std::wstring_view section,
                          std::wstring_view key,
                          std::wstring_view default_value) {
  // The truncation test here is `>= buffer.size() - 1`, which is right for
  // `GetPrivateProfileStringW` -- and note that it is a different threshold from
  // the section reader in config.cc. Measured with a 100-wchar_t buffer: a
  // 98-character value returns 98 and was not truncated, while a value of 99 or
  // more returns 99, so `size - 1` is the signal. `GetPrivateProfileSectionW` in
  // contrast reports `size - 2`. This loop was left alone after a run over value
  // lengths 0..400 found no length at which `size - 2` would change the result;
  // there is no defect here to fix.
  std::vector<TCHAR> buffer(100);
  DWORD bytesread = 0;
  do {
    bytesread = ::GetPrivateProfileStringW(
        section.data(), key.data(), default_value.data(), buffer.data(),
        static_cast<DWORD>(buffer.size()), GetIniPath().c_str());
    if (bytesread >= buffer.size() - 1) {
      buffer.resize(buffer.size() * 2);
    } else {
      break;
    }
  } while (true);

  return std::wstring(buffer.data());
}

std::optional<std::wstring> CanonicalizePath(const std::wstring& path) {
  // `PathCanonicalizeW` cannot be used here however large the buffer is: it
  // rejects any input past MAX_PATH with ERROR_FILENAME_EXCED_RANGE, measured at
  // 261 characters with a 465-character output buffer and still failing. So the
  // canonicalization comes from `PathCchCanonicalizeEx` with
  // PATHCCH_ALLOW_LONG_PATHS, which returns byte-identical results for short
  // input (`C:\App\..\user_data` -> `C:\user_data` from both).
  //
  // Which code it returns for a long path depends on the buffer size, and this is
  // the trap in the retry loop: with a MAX_PATH buffer it reports
  // ERROR_FILENAME_EXCED_RANGE (260 -> 0x800700CE, 512 -> 0x8007007A
  // INSUFFICIENT_BUFFER, 1024 -> success, all measured). Treating
  // FILENAME_EXCED_RANGE as fatal makes the loop give up on the first try, which
  // is what an earlier version of this function did. Both codes therefore mean
  // "give me a bigger buffer".
  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    const HRESULT hr =
        ::PathCchCanonicalizeEx(buffer.data(), buffer.size(), path.c_str(),
                                PATHCCH_ALLOW_LONG_PATHS);
    if (SUCCEEDED(hr)) {
      return std::wstring(buffer.data());
    }
    const bool needs_more =
        hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) ||
        hr == HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
    if (!needs_more) {
      return std::nullopt;
    }
    constexpr size_t kMaxPathLength = 1u << 16;
    if (buffer.size() >= kMaxPathLength) {
      return std::nullopt;
    }
    // Doubling off MAX_PATH would need two rounds before a long path fits; the
    // first growth jumps straight past MAX_PATH instead.
    buffer.resize(buffer.size() < 1024 ? 1024 : buffer.size() * 2);
  }
}

std::optional<std::wstring> GetAbsolutePath(const std::wstring& path) {
  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    // The required length including the terminating NUL is the return value
    // whenever the buffer was too small (measured: a 16-character buffer for a
    // 22-character path returns 23). That is the signal to grow by.
    const DWORD needed = ::GetFullPathNameW(path.c_str(),
                                            static_cast<DWORD>(buffer.size()),
                                            buffer.data(), nullptr);
    if (needed == 0) {
      return std::nullopt;  // the call itself failed
    }
    if (needed <= buffer.size()) {
      return std::wstring(buffer.data());
    }
    constexpr size_t kMaxPathLength = 1u << 16;
    if (needed > kMaxPathLength) {
      return std::nullopt;
    }
    buffer.resize(needed);
  }
}

std::optional<std::wstring> ExpandEnvironmentPath(const std::wstring& path) {
  // The second argument of this constructor is a count, not a position, so the
  // original `std::wstring(&buffer[0], 0, ExpandedLength)` was already correct
  // (measured against the raw expansion); what it did not do was notice failure.
  // `ExpandEnvironmentStringsW` returns 0 on error and the length including the
  // terminating NUL otherwise, so a zero return has to be distinguished from an
  // empty expansion rather than silently producing an empty string.
  std::vector<wchar_t> buffer(MAX_PATH);
  for (;;) {
    const DWORD written = ::ExpandEnvironmentStringsW(
        path.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0) {
      return std::nullopt;
    }
    if (written <= buffer.size()) {
      // `written` counts the NUL, which the returned string must not contain.
      return std::wstring(buffer.data(), written - 1);
    }
    constexpr size_t kMaxPathLength = 1u << 16;
    if (written > kMaxPathLength) {
      return std::nullopt;
    }
    buffer.resize(written);
  }
}

HWND GetTopWnd(HWND hwnd) {
  while (::GetParent(hwnd) && ::IsWindowVisible(::GetParent(hwnd))) {
    hwnd = ::GetParent(hwnd);
  }
  return hwnd;
}

void ExecuteCommand(int id, HWND hwnd) {
  if (hwnd == 0) {
    hwnd = GetForegroundWindow();
  }
  // A null hwnd would turn the PostMessage below into a thread message.
  if (!hwnd) {
    return;
  }
  // Browser commands are window-scoped. Callers often pass the HWND under the
  // cursor (child widget / render host); route to the top-level frame.
  if (const HWND root = ::GetAncestor(hwnd, GA_ROOT)) {
    hwnd = root;
  }
  // Post, do not Send: frequently called from the UI-thread mouse hook
  // (double-click close). A synchronous SendMessageTimeout can re-enter
  // Chrome while the hook is still on the stack and break the next
  // window gesture after close.
  ::PostMessageW(hwnd, WM_SYSCOMMAND, id, 0);
}

void LaunchCommands(const std::wstring& get_commands) {
  auto commands = StringSplit(
      get_commands,
      L';');  // Quotes should not be used as they can cause errors with paths
              // that contain spaces. Since semicolons rarely appear in names
              // and commands, they are used as delimiters.
  if (commands.empty()) {
    return;
  }
  for (const auto& command : commands) {
    auto expanded = ExpandEnvironmentPath(command);
    if (!expanded) {
      // Running the command with an unexpanded path would launch the wrong
      // thing, so it is skipped and reported instead.
      WarnLog(L"ExecuteCommands: cannot expand the environment in '" + command +
              L"'; command skipped");
      continue;
    }
    std::wstring& expanded_path = *expanded;
    ReplaceStringInPlace(expanded_path, L"%app%", GetAppDir());

    // Using `start` launches the command in a new window asynchronously,
    // avoiding blocking Chrome's main thread. For more details:
    // https://github.com/Bush2021/chrome_plus/issues/130#issuecomment-2925782726
    // https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/start
    //  `cmd /c` ensures the command window exits after execution, preventing
    //  the "Not enough memory resources are available to process this command"
    //  error even when all commands run successfully.
    std::wstring cmd =
        LR"(start "chrome++ cmd" cmd /c ")" + expanded_path + LR"(")";
    _wsystem(cmd.c_str());
  }
}

[[nodiscard]] bool IsChromeWindow(HWND hwnd) {
  std::array<wchar_t, 256> class_name_buffer{};
  const int length =
      ::GetClassNameW(hwnd, class_name_buffer.data(),
                      static_cast<int>(class_name_buffer.size()));
  if (length == 0) {
    return false;
  }
  const std::wstring_view class_name_view{class_name_buffer.data(),
                                          static_cast<std::size_t>(length)};
  constexpr std::wstring_view target_prefix = L"Chrome_WidgetWin_";
  return class_name_view.starts_with(target_prefix);
}

namespace {

// Modifier keys mapping
constexpr std::pair<std::wstring_view, UINT> kModifierKeys[] = {
    {L"shift", MOD_SHIFT},     {L"ctrl", MOD_CONTROL},
    {L"control", MOD_CONTROL},  // alias
    {L"alt", MOD_ALT},         {L"win", MOD_WIN},
};

// Special virtual keys mapping
constexpr std::pair<std::wstring_view, UINT> kSpecialKeys[] = {
    // Arrow keys
    {L"left", VK_LEFT},
    {L"right", VK_RIGHT},
    {L"up", VK_UP},
    {L"down", VK_DOWN},
    {L"←", VK_LEFT},
    {L"→", VK_RIGHT},
    {L"↑", VK_UP},
    {L"↓", VK_DOWN},
    // Control keys
    {L"esc", VK_ESCAPE},
    {L"escape", VK_ESCAPE},  // alias
    {L"tab", VK_TAB},
    {L"backspace", VK_BACK},
    {L"enter", VK_RETURN},
    {L"return", VK_RETURN},  // alias
    {L"space", VK_SPACE},
    // System keys
    {L"prtsc", VK_SNAPSHOT},
    {L"printscreen", VK_SNAPSHOT},  // alias
    {L"scroll", VK_SCROLL},
    {L"pause", VK_PAUSE},
    // Navigation keys
    {L"insert", VK_INSERT},
    {L"delete", VK_DELETE},
    {L"del", VK_DELETE},  // alias
    {L"home", VK_HOME},
    {L"end", VK_END},
    {L"pageup", VK_PRIOR},
    {L"pgup", VK_PRIOR},  // alias
    {L"pagedown", VK_NEXT},
    {L"pgdn", VK_NEXT},  // alias
};

constexpr bool EqualsIgnoreCase(std::wstring_view a, std::wstring_view b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::towlower(a[i]) != std::towlower(b[i]))  // case-insensitive
      return false;
  }
  return true;
}

template <size_t N>
constexpr std::optional<UINT> FindInKeyMap(
    std::wstring_view key,
    const std::pair<std::wstring_view, UINT> (&map)[N]) {
  for (const auto& [name, code] : map) {
    if (EqualsIgnoreCase(key, name))
      return code;
  }
  return std::nullopt;
}

// Parse function key (F1-F24)
std::optional<UINT> ParseFunctionKey(std::wstring_view key) {
  if (key.size() < 2 || (key[0] != L'F' && key[0] != L'f'))
    return std::nullopt;

  auto num_part = key.substr(1);
  if (num_part.empty() || !std::ranges::all_of(num_part, ::iswdigit))
    return std::nullopt;

  int fx = 0;
  for (wchar_t c : num_part) {
    fx = fx * 10 + (c - L'0');
  }

  if (fx >= 1 && fx <= 24)
    return VK_F1 + fx - 1;
  return std::nullopt;
}

// Parse single character key (A-Z, 0-9, symbols)
std::optional<UINT> ParseCharacterKey(std::wstring_view key) {
  if (key.size() != 1)
    return std::nullopt;

  wchar_t ch = key[0];
  if (std::iswalnum(ch))
    return static_cast<UINT>(std::towupper(ch));

  // For other characters, use `VkKeyScan`
  SHORT scan = ::VkKeyScanW(ch);
  if (scan != -1)
    return LOBYTE(scan);

  return std::nullopt;
}

}  // namespace

UINT ParseHotkeys(std::wstring_view keys, bool no_repeat) {
  UINT modifiers = 0;
  UINT virtual_key = 0;

  for (const auto& part : std::views::split(keys, L'+')) {
    std::wstring_view key(part.begin(), part.end());
    if (key.empty())
      continue;
    if (auto mod = FindInKeyMap(key, kModifierKeys)) {
      modifiers |= *mod;
      continue;
    }
    if (auto vk = FindInKeyMap(key, kSpecialKeys)) {
      virtual_key = *vk;
      continue;
    }
    if (auto vk = ParseFunctionKey(key)) {
      virtual_key = *vk;
      continue;
    }
    if (auto vk = ParseCharacterKey(key))
      virtual_key = *vk;
  }

  if (no_repeat)
    modifiers |= MOD_NOREPEAT;

  return MAKELPARAM(modifiers, virtual_key);
}

// One `[chrome++] ...` line appended to Chrome++_Debug.log.
//
// Built from plain Win32 calls and concatenation on purpose: routing this
// through `std::format`, `std::wofstream` and `std::filesystem` pulls in enough
// to grow the DLL by roughly 480 KB, and unlike `DebugLog` this is always
// compiled in, including in the shipped release builds.
void WarnLog(std::wstring_view message) {
  static std::mutex log_mutex;
  std::lock_guard<std::mutex> lock(log_mutex);

  std::wstring path = GetAppDir();
  path.append(L"Chrome++_Debug.log");

  const HANDLE file = ::CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }

  std::wstring line = L"[chrome++] ";
  line.append(message);
  line.append(L"\r\n");

  const DWORD bytes = static_cast<DWORD>(line.size() * sizeof(wchar_t));
  DWORD written = 0;
  ::WriteFile(file, line.data(), bytes, &written, nullptr);
  ::CloseHandle(file);
}
