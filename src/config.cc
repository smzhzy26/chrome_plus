#include "config.h"

#include <windows.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "utils.h"

Config& Config::Instance() {
  static Config instance;
  return instance;
}

Config::Config() {
  LoadConfig();
}

void Config::LoadConfig() {
  // general
  command_line_ = GetIniString(L"general", L"command_line", L"");
  launch_on_startup_ = GetIniString(L"general", L"launch_on_startup", L"");
  launch_on_exit_ = GetIniString(L"general", L"launch_on_exit", L"");
  user_data_dir_ = LoadDirPath(L"data");
  disk_cache_dir_ = LoadDirPath(L"cache");
  boss_key_ = GetIniString(L"general", L"boss_key", L"");
  translate_key_ = GetIniString(L"general", L"translate_key", L"");
  show_password_ = ::GetPrivateProfileIntW(L"general", L"show_password", 1,
                                           GetIniPath().c_str()) != 0;
  win32k_ = ::GetPrivateProfileIntW(L"general", L"win32k", 0,
                                    GetIniPath().c_str()) != 0;
  ignore_policies_ = ::GetPrivateProfileIntW(L"general", L"ignore_policies", 0,
                                             GetIniPath().c_str()) != 0;
  suppress_false_upgrade_notification_ =
      ::GetPrivateProfileIntW(L"general",
                              L"suppress_false_upgrade_notification", 0,
                              GetIniPath().c_str()) != 0;

  // tabs
  keep_last_tab_ = ::GetPrivateProfileIntW(L"tabs", L"keep_last_tab", 1,
                                           GetIniPath().c_str()) != 0;
  double_click_close_ = ::GetPrivateProfileIntW(L"tabs", L"double_click_close",
                                                1, GetIniPath().c_str()) != 0;
  right_click_close_ = ::GetPrivateProfileIntW(L"tabs", L"right_click_close", 0,
                                               GetIniPath().c_str()) != 0;
  // Off by default, matching the shipped ini: this gesture needs `IsOnTabBar` to
  // find the tab strip through UI Automation, and Chrome does not expose those
  // elements unless accessibility is enabled. `wheel_tab_when_press_rbutton`
  // below does not consult the accessibility tree, so it stays on -- verified on
  // a default profile, where the right-button gesture switches tabs and does not
  // open the context menu.
  wheel_tab_ = ::GetPrivateProfileIntW(L"tabs", L"wheel_tab", 0,
                                       GetIniPath().c_str()) != 0;
  wheel_tab_when_press_rbutton_ =
      ::GetPrivateProfileIntW(L"tabs", L"wheel_tab_when_press_rbutton", 1,
                              GetIniPath().c_str()) != 0;
  hover_tab_ = ::GetPrivateProfileIntW(L"tabs", L"hover_tab", 0,
                                       GetIniPath().c_str()) != 0;
  hover_tab_delay_ = LoadHoverTabDelay();
  open_url_new_tab_ = LoadOpenUrlNewTabMode();
  bookmark_new_tab_ = LoadBookmarkNewTabMode();
  new_tab_disable_ = ::GetPrivateProfileIntW(L"tabs", L"new_tab_disable", 1,
                                             GetIniPath().c_str()) != 0;
  disable_tab_name_ = GetIniString(L"tabs", L"new_tab_disable_name", L"");
  disable_tab_names_ = StringSplit(disable_tab_name_, L',', L"\"");

  // keymapping
  LoadKeyMappings();
}

void Config::LoadKeyMappings() {
  // `GetPrivateProfileSectionW` copies as much of the section as fits and, when
  // the section does not fit, fills the buffer and returns `size - 2`; it has no
  // mode that reports the size needed (passing a null buffer and zero returns 0).
  // Measured: a 4096-wchar_t buffer returns 4094, and the same 4094 for 300, 500,
  // 1000 or 5000 entries written -- so the section stopped at 270 entries and
  // every mapping past it was silently dropped. An earlier version of this loop
  // tested `chars_read < buffer.size() - 1` and therefore read that 4094 as "the
  // whole section fits", which is the off-by-one the numbers above pin down.
  std::vector<wchar_t> buffer(4096);
  DWORD chars_read = 0;
  for (;;) {
    chars_read = ::GetPrivateProfileSectionW(
        L"keymapping", buffer.data(), static_cast<DWORD>(buffer.size()),
        GetIniPath().c_str());
    if (chars_read < buffer.size() - 2) {
      break;  // the whole section fits
    }
    constexpr size_t kMaxSectionChars = 1u << 20;  // 1 MiB of wchar_t
    if (buffer.size() >= kMaxSectionChars) {
      // Out of room rather than out of entries. Saying so matters: the
      // alternative is a user whose later mappings do nothing for no stated
      // reason, which is the defect this growth loop exists to remove.
      WarnLog(L"Config: the [keymapping] section is larger than the 1 MiB "
              L"limit and has been truncated");
      break;
    }
    buffer.resize(buffer.size() * 2);
  }

  if (chars_read == 0) {
    return;
  }

  const wchar_t* current = buffer.data();
  while (*current != L'\0') {
    const std::wstring_view line(current);
    current += line.length() + 1;

    const auto eq_pos = line.find(L'=');
    if (eq_pos == std::wstring_view::npos || eq_pos == 0) {
      continue;
    }

    std::wstring_view key = line.substr(0, eq_pos);
    std::wstring_view value = line.substr(eq_pos + 1);

    while (!key.empty() && (key.back() == L' ' || key.back() == L'\t')) {
      key.remove_suffix(1);
    }
    while (!value.empty() &&
           (value.front() == L' ' || value.front() == L'\t')) {
      value.remove_prefix(1);
    }

    if (!key.empty() && !value.empty()) {
      key_mappings_.emplace_back(std::wstring(key), std::wstring(value));
    }
  }
}

std::optional<std::wstring> Config::LoadDirPath(const std::wstring& dir_type) {
  // Both helpers can now fail outright, and each failure has to be told apart
  // from a legitimate value. `CanonicalizePath` used to answer an empty string
  // for any path past MAX_PATH, which this function then handed to
  // `GetIniString` as the default: the directory silently became nullopt and the
  // user's own `data_dir` / `cache_dir` was discarded without a message.
  const auto path = CanonicalizePath(GetAppDir() + L"\\..\\" + dir_type);
  if (!path) {
    WarnLog(L"Config: cannot resolve the default directory for '" + dir_type +
            L"'; the setting is ignored");
    return std::nullopt;
  }

  std::wstring dir_key = dir_type + L"_dir";
  std::wstring dir_buffer = GetIniString(L"general", dir_key, *path);

  if (dir_buffer == L"none") {
    return std::nullopt;
  }

  if (dir_buffer.empty()) {
    dir_buffer = *path;
  }

  auto expanded_path = ExpandEnvironmentPath(dir_buffer);
  if (!expanded_path) {
    WarnLog(L"Config: cannot expand the environment in '" + dir_buffer +
            L"' for '" + dir_key + L"'; the setting is ignored");
    return std::nullopt;
  }
  ReplaceStringInPlace(*expanded_path, L"%app%", GetAppDir());
  const auto absolute = GetAbsolutePath(*expanded_path);
  if (!absolute) {
    WarnLog(L"Config: cannot make '" + *expanded_path +
            L"' absolute for '" + dir_key + L"'; the setting is ignored");
    return std::nullopt;
  }
  return *absolute;
}

int Config::LoadHoverTabDelay() {
  constexpr int kDefaultDelayMs = 400;
  constexpr int kMaxDelayMs = 5000;
  const int delay = ::GetPrivateProfileIntW(
      L"tabs", L"hover_tab_delay", kDefaultDelayMs, GetIniPath().c_str());
  if (delay < 0 || delay > kMaxDelayMs) {
    return kDefaultDelayMs;
  }
  return delay;
}

int Config::LoadOpenUrlNewTabMode() {
  return ::GetPrivateProfileIntW(L"tabs", L"open_url_new_tab", 0,
                                 GetIniPath().c_str());
}
int Config::LoadBookmarkNewTabMode() {
  return ::GetPrivateProfileIntW(L"tabs", L"open_bookmark_new_tab", 0,
                                 GetIniPath().c_str());
}

const Config& config = Config::Instance();
