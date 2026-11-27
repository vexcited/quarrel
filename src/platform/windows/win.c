#include <objbase.h>
#include <shlobj.h>
#include <stdcountof.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "win.h"

wchar_t *win_wide(const char *s) {
  if (s == nullptr) {
    return nullptr;
  }

  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, nullptr, 0);
  wchar_t *out = n > 0 ? malloc((size_t)n * sizeof *out) : nullptr;

  if (out != nullptr &&
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, out, n) != n) {
    free(out);
    return nullptr;
  }

  return out;
}

char *win_utf8(const wchar_t *s) {
  if (s == nullptr) {
    return nullptr;
  }

  int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
  char *out = n > 0 ? malloc((size_t)n) : nullptr;

  if (out != nullptr &&
      WideCharToMultiByte(CP_UTF8, 0, s, -1, out, n, nullptr, nullptr) != n) {
    free(out);
    return nullptr;
  }

  return out;
}

bool win_com(void) {
  static int state;

  // another model taken first is used as it is.
  if (state == 0) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    state = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE ? 1 : -1;
  }

  return state > 0;
}

bool win_folder(const GUID *known, char *out, size_t cap) {
  wchar_t *base = nullptr;

  if (FAILED(SHGetKnownFolderPath(known, KF_FLAG_CREATE, nullptr, &base))) {
    CoTaskMemFree(base);
    return false;
  }

  wchar_t ours[MAX_PATH + 16];
  bool made = _snwprintf(ours, countof(ours), L"%ls\\Quarrel", base) > 0;

  CoTaskMemFree(base);

  if (!made || (!CreateDirectoryW(ours, nullptr) &&
                GetLastError() != ERROR_ALREADY_EXISTS)) {
    return false;
  }

  char *path = win_utf8(ours);
  bool fits = path != nullptr && (size_t)snprintf(out, cap, "%s", path) < cap;

  free(path);
  return fits;
}

bool win_denied(const wchar_t *capability) {
  static const struct {
    HKEY root;
    const wchar_t *under;
  } KEYS[] = {
    {HKEY_LOCAL_MACHINE, L""},
    {HKEY_CURRENT_USER, L""},
    {HKEY_CURRENT_USER, L"\\NonPackaged"},
  };

  for (size_t i = 0; i < countof(KEYS); i++) {
    wchar_t key[160], value[16] = L"";
    DWORD size = sizeof value;

    _snwprintf(key, countof(key),
               L"Software\\Microsoft\\Windows\\CurrentVersion"
               L"\\CapabilityAccessManager\\ConsentStore\\%ls%ls",
               capability, KEYS[i].under);

    if (RegGetValueW(KEYS[i].root, key, L"Value", RRF_RT_REG_SZ, nullptr, value,
                     &size) == ERROR_SUCCESS &&
        wcscmp(value, L"Deny") == 0) {
      return true;
    }
  }

  return false;
}
