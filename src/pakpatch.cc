#include "pakpatch.h"

#include <windows.h>

#include <sddl.h>

#include <cstdint>
#include <cwchar>
#include <optional>
#include <string>
#include <string_view>

#include "detours.h"

#include "pakfile.h"
#include "utils.h"
#include "version.h"

namespace {
#if defined(_M_ARM64)
#define BUILD_ARCH " (ARM64)"
#elif defined(_M_X64)
#define BUILD_ARCH " (64-bit)"
#else
#define BUILD_ARCH " (32-bit)"
#endif

static HANDLE resources_pak_map = nullptr;

static auto RawCreateFileMapping = CreateFileMappingW;
static auto RawMapViewOfFile = MapViewOfFile;

// `PakPatch()` runs in the browser and in every renderer (see the loader in
// chrome++.cc: with in-process WebUI resource loading the renderer serves
// `chrome://settings` from its own pak mapping, so it must patch that mapping
// itself). Re-running the content scan in each renderer inflated every gzip
// entry up to the target and re-deflated the patched one at level 9, on the
// renderer main thread during `PreSandboxStartup` -- a freeze on every new
// tab and cross-site navigation. Instead the browser scans once and hands the
// result to its children through two inherited environment variables: the
// target's pak resource id, so a renderer that has to decompress inflates
// exactly one entry, and the name of a read-only section holding the
// browser's already-patched entry, which a renderer applies with a single
// memcpy and no decompression at all.
constexpr wchar_t kPakTargetIdEnv[] = L"CHROME_PLUS_PAK_RES_ID";
constexpr wchar_t kPakBlobEnv[] = L"CHROME_PLUS_PAK_BLOB";

struct PakBlobHeader {
  uint32_t resource_id;
  uint32_t length;
};

// Keeps the published section alive for the browser's lifetime so child
// processes can open it by name.
static HANDLE published_blob_section = nullptr;

// The loader calls `PakPatch()` only in the browser and in `--type=renderer`
// children (chrome++.cc `Loader`), so no `-type=` switch means the browser.
bool IsBrowserProcess() {
  return !wcsstr(GetCommandLineW(), L"-type=");
}

uint16_t GetPakTargetId() {
  wchar_t value[8];
  DWORD len = GetEnvironmentVariableW(kPakTargetIdEnv, value, ARRAYSIZE(value));
  if (len == 0 || len >= ARRAYSIZE(value)) {
    return 0;
  }
  wchar_t* end = nullptr;
  unsigned long id = wcstoul(value, &end, 10);
  if (end == value || id == 0 || id > 0xFFFF) {
    return 0;
  }
  return static_cast<uint16_t>(id);
}

// Browser side: copy the patched slot into a named read-only section and
// publish the name through the environment. Read-only is load-bearing: the
// bytes feed the privileged settings WebUI in every renderer, so a writable
// section would be an HTML injection vector into `chrome://settings`. The
// DACL grants read to Everyone and to RestrictedCode (the renderer's
// restricting SID), so a renderer's pre-lockdown token can open the section
// at the same point it maps the pak.
void PublishPatchedEntry(uint8_t* buffer, size_t buffer_size,
                          uint16_t resource_id) {
  // After an in-app restart the new browser inherits the old browser's
  // section name, which dies with that process; drop it and republish under
  // this pid.
  SetEnvironmentVariableW(kPakBlobEnv, nullptr);

  const auto slot = FindResourceSlot(buffer, buffer_size, resource_id);
  if (!slot) {
    return;
  }

  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, FALSE};
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:(A;;GR;;;WD)(A;;GR;;;RC)", SDDL_REVISION_1,
          &sa.lpSecurityDescriptor, nullptr)) {
    return;
  }

  const std::wstring name =
      L"Local\\ChromePlusPakBlob_" + std::to_wstring(GetCurrentProcessId());
  const DWORD size = static_cast<DWORD>(sizeof(PakBlobHeader) + slot->length);
  HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                      0, size, name.c_str());
  const DWORD create_error = GetLastError();
  LocalFree(sa.lpSecurityDescriptor);
  if (!section) {
    return;
  }
  // A pre-existing name means another process squatted it; leave children on
  // the decompress fallback rather than trust its contents.
  if (create_error == ERROR_ALREADY_EXISTS) {
    CloseHandle(section);
    return;
  }

  auto* view =
      static_cast<uint8_t*>(MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, size));
  if (!view) {
    CloseHandle(section);
    return;
  }
  auto* header = reinterpret_cast<PakBlobHeader*>(view);
  header->resource_id = resource_id;
  header->length = slot->length;
  memcpy(view + sizeof(PakBlobHeader), buffer + slot->offset, slot->length);
  UnmapViewOfFile(view);

  published_blob_section = section;
  SetEnvironmentVariableW(kPakBlobEnv, name.c_str());
  DebugLog(L"PakPatch: published resource {} ({} bytes) as {}", resource_id,
           slot->length, name);
}

// Renderer fast path: overwrite this process's copy-on-write pak view with
// the browser's already-patched bytes. The slot is re-derived from this
// process's own pak index and the lengths must match, so a section built from
// a different pak is rejected and the copy cannot write outside the slot.
bool ApplyPatchedEntry(uint8_t* buffer, size_t buffer_size) {
  wchar_t name[64];
  DWORD len = GetEnvironmentVariableW(kPakBlobEnv, name, ARRAYSIZE(name));
  if (len == 0 || len >= ARRAYSIZE(name)) {
    return false;
  }

  HANDLE section = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
  if (!section) {
    return false;
  }

  bool applied = false;
  if (const auto* view = static_cast<const uint8_t*>(
          MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0))) {
    const auto* header = reinterpret_cast<const PakBlobHeader*>(view);
    std::optional<PakResourceSlot> slot;
    if (header->resource_id <= 0xFFFF) {
      slot =
          FindResourceSlot(buffer, buffer_size,
                       static_cast<uint16_t>(header->resource_id));
    }
    if (slot && slot->length == header->length) {
      memcpy(buffer + slot->offset, view + sizeof(PakBlobHeader), slot->length);
      applied = true;
      DebugLog(L"PakPatch: applied published resource {}", header->resource_id);
    }
    UnmapViewOfFile(view);
  }
  CloseHandle(section);
  return applied;
}

// The #172 settings-page injection, run on each candidate decompressed pak
// entry until it finds the one holding the settings-about-page HTML.
bool PatchSettingsHtml(uint8_t* begin, uint32_t size, size_t& new_len) {
  BYTE search_start[] = R"(</settings-about-page>)";
  auto match = SearchMemory(
      std::span<uint8_t>(begin, size),
      std::span<const uint8_t>(search_start, sizeof(search_start) - 1));
  if (match.empty()) {
    return false;
  }

  // Compress the HTML for writing patch information.
  std::string html(reinterpret_cast<char*>(begin), size);
  compression_html(html);

  // RemoveUpdateError
  // if (IsNeedPortable())
  {
    ReplaceStringInPlace(html, R"(?hidden="${!this.showUpdateStatus_}")",
                         R"(hidden="true")");
    ReplaceStringInPlace(html, R"(?hidden="${!this.shouldShowIcons_()}")",
                         R"(hidden="true")");
  }

  const char product_title[] =
      R"({aboutBrowserVersion}</div><div class="secondary">Powered by <a target="_blank" href="https://github.com/Bush2021/chrome_plus">Chrome++ Next</a> )" RELEASE_VER_STR BUILD_ARCH
      R"(</div>)";
  ReplaceStringInPlace(html, R"({aboutBrowserVersion}</div>)", product_title);

  if (html.length() > size) {
    return false;
  }
  memcpy(begin, html.c_str(), html.length());
  new_len = html.length();
  return true;
}

// One flow per process kind: the browser locates and patches the entry
// itself, then publishes the id and the patched bytes for its children; a
// renderer takes the cheapest tier available -- published bytes (no
// decompression), targeted decompress (one entry), full content scan. Runs
// inside `MyMapViewOfFile` after both hooks have detached themselves, so the
// section create/open/map calls in the publish and apply helpers reach the
// real APIs, not our hooks.
void PatchResourcesPak(uint8_t* buffer, size_t buffer_size) {
  const bool is_browser = IsBrowserProcess();
  if (!is_browser && ApplyPatchedEntry(buffer, buffer_size)) {
    return;
  }

  const uint16_t target_id = GetPakTargetId();
  uint16_t matched_id =
      TraversalGZIPFile(buffer, buffer_size, PatchSettingsHtml, target_id);
  if (matched_id == 0 && target_id != 0) {
    // The inherited id missed, so the pak was replaced (browser updated
    // between sessions); redo the full content scan.
    matched_id =
        TraversalGZIPFile(buffer, buffer_size, PatchSettingsHtml, 0);
  }

  if (is_browser && matched_id != 0) {
    SetEnvironmentVariableW(kPakTargetIdEnv,
                            std::to_wstring(matched_id).c_str());
    PublishPatchedEntry(buffer, buffer_size, matched_id);
  }
}

HANDLE WINAPI MyMapViewOfFile(_In_ HANDLE hFileMappingObject,
                              _In_ DWORD dwDesiredAccess,
                              _In_ DWORD dwFileOffsetHigh,
                              _In_ DWORD dwFileOffsetLow,
                              _In_ SIZE_T dwNumberOfBytesToMap) {
  if (hFileMappingObject == resources_pak_map) {
    // Modify it to be modifiable.
    LPVOID buffer =
        RawMapViewOfFile(hFileMappingObject, FILE_MAP_COPY, dwFileOffsetHigh,
                         dwFileOffsetLow, dwNumberOfBytesToMap);

    // No more hook needed.
    resources_pak_map = nullptr;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(reinterpret_cast<LPVOID*>(&RawMapViewOfFile),
                 reinterpret_cast<void*>(MyMapViewOfFile));
    auto status = DetourTransactionCommit();
    if (status != NO_ERROR) {
      WarnLog(L"PakPatch: cannot unhook MapViewOfFile (error " + std::to_wstring(status) + L")");
    }

    if (buffer) {
      // Every offset read out of the pak is validated against this, so it has to
      // be the real view length. `dwNumberOfBytesToMap` is 0 when the caller
      // mapped the whole file, and with a non-zero file offset it would be a
      // partial length; `VirtualQuery` reports the mapped region instead. Its
      // value is page-rounded, i.e. an upper bound on the file, which is the
      // safe direction for a bounds check.
      size_t buffer_size = dwNumberOfBytesToMap;
      MEMORY_BASIC_INFORMATION region{};
      if (VirtualQuery(buffer, &region, sizeof(region))) {
        if (dwNumberOfBytesToMap == 0 || region.RegionSize < buffer_size) {
          buffer_size = region.RegionSize;
        }
      } else if (buffer_size == 0) {
        // No size from either source. The bounds check would then reject every
        // pak and the patch would silently not apply, so say so instead.
        DebugLog(L"PakPatch: cannot size the mapped pak view ({})",
                 GetLastError());
      }
      PatchResourcesPak(static_cast<uint8_t*>(buffer), buffer_size);
    }

    return buffer;
  }

  return RawMapViewOfFile(hFileMappingObject, dwDesiredAccess, dwFileOffsetHigh,
                          dwFileOffsetLow, dwNumberOfBytesToMap);
}

// Identify `resources.pak` where it is mapped, by querying the handle, rather
// than where it is opened. Chrome maps the pak with
// `CreateFileMapping`/`MapViewOfFile` (`base::MemoryMappedFile`), so this hook
// catches it directly in every process that loads it -- including the renderer,
// which opens the pak by path itself (`AddDataPackFromPath` in
// `ChromeMainDelegate::PreSandboxStartup`, before sandbox lockdown). The
// previous `CreateFile` hook existed only to record that handle so this hook
// could match it later; querying the handle here drops that second hook.
// `GetFileInformationByHandleEx(FileNameInfo)` reads the path of a handle the
// process already holds, which the sandbox permits -- it brokers new opens
// (`NtCreateFile`/`NtOpenFile`), not operations on existing handles.
// https://chromium.googlesource.com/chromium/src/+/main/docs/design/sandbox.md
bool IsResourcesPak(HANDLE hFile) {
  if (hFile == nullptr || hFile == INVALID_HANDLE_VALUE) {
    return false;
  }
  alignas(FILE_NAME_INFO)
      BYTE buffer[sizeof(FILE_NAME_INFO) + MAX_PATH * 2 * sizeof(wchar_t)];
  auto* info = reinterpret_cast<FILE_NAME_INFO*>(buffer);
  if (!GetFileInformationByHandleEx(hFile, FileNameInfo, info,
                                    sizeof(buffer))) {
    return false;
  }
  std::wstring_view name(info->FileName,
                         info->FileNameLength / sizeof(wchar_t));
  return name.ends_with(L"resources.pak");
}

HANDLE WINAPI MyCreateFileMapping(_In_ HANDLE hFile,
                                  _In_opt_ LPSECURITY_ATTRIBUTES lpAttributes,
                                  _In_ DWORD flProtect,
                                  _In_ DWORD dwMaximumSizeHigh,
                                  _In_ DWORD dwMaximumSizeLow,
                                  _In_opt_ LPCTSTR lpName) {
  if (IsResourcesPak(hFile)) {
    // Force copy-on-write so the mapped view can be patched in memory.
    resources_pak_map =
        RawCreateFileMapping(hFile, lpAttributes, PAGE_WRITECOPY,
                             dwMaximumSizeHigh, dwMaximumSizeLow, lpName);

    // No more hook needed.
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(reinterpret_cast<LPVOID*>(&RawCreateFileMapping),
                 reinterpret_cast<void*>(MyCreateFileMapping));
    auto status = DetourTransactionCommit();
    if (status != NO_ERROR) {
      WarnLog(L"PakPatch: cannot unhook CreateFileMapping (error " + std::to_wstring(status) + L")");
    }

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<LPVOID*>(&RawMapViewOfFile),
                 reinterpret_cast<void*>(MyMapViewOfFile));
    status = DetourTransactionCommit();
    if (status != NO_ERROR) {
      WarnLog(L"PakPatch: cannot hook MapViewOfFile (error " + std::to_wstring(status) + L"); the resources.pak patch will not be applied");
    }

    return resources_pak_map;
  }
  return RawCreateFileMapping(hFile, lpAttributes, flProtect, dwMaximumSizeHigh,
                              dwMaximumSizeLow, lpName);
}

}  // namespace

void PakPatch() {
  DetourTransactionBegin();
  DetourUpdateThread(GetCurrentThread());
  DetourAttach(reinterpret_cast<LPVOID*>(&RawCreateFileMapping),
               reinterpret_cast<void*>(MyCreateFileMapping));
  auto status = DetourTransactionCommit();
  if (status != NO_ERROR) {
    // Without this hook the pak is never patched, so the settings page and the
    // hide-this-tab entry look untouched. That is indistinguishable from the
    // feature not existing, which is why it is reported in release builds.
    WarnLog(L"PakPatch: cannot hook CreateFileMapping (error " +
            std::to_wstring(status) +
            L"); the resources.pak patch will not be applied");
  }
}
