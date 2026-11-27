#include <windows.h>

#include <dpapi.h>
#include <knownfolders.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wincred.h>

#include "win.h"

/* ========================================================================== */
/* secrets are generic credentials of the credential manager, named           */
/* Quarrel/<key>, which hold 2560 bytes. a token is far less, a larger value  */
/* goes to a file of the roaming folder, sealed to the user by dpapi.         */
/* preferences are strings of HKCU\Software\Quarrel.                          */
/* ========================================================================== */

#define PREFS L"Software\\Quarrel"

static wchar_t *target(const char *key) {
  size_t len = strlen(key) + sizeof "Quarrel/";
  char *named = malloc(len);

  if (named == nullptr) {
    return nullptr;
  }

  snprintf(named, len, "Quarrel/%s", key);

  wchar_t *wide = win_wide(named);

  free(named);
  return wide;
}

// the sealed file of a key, named by its bytes in hex: a key may hold what
// a file name may not.
static bool sealed_path(const char *key, char *out, size_t cap) {
  char folder[1024];
  size_t n = 0;

  if (!win_folder(&FOLDERID_RoamingAppData, folder, sizeof folder)) {
    return false;
  }

  n = (size_t)snprintf(out, cap, "%s\\", folder);

  for (const uint8_t *c = (const uint8_t *)key; *c && n + 3 < cap; c++) {
    n += (size_t)snprintf(out + n, cap - n, "%02x", *c);
  }

  return n + 8 < cap && snprintf(out + n, cap - n, ".secret") > 0;
}

static bool seal(const char *key, const char *value, size_t len) {
  char path[1200];
  DATA_BLOB plain = {(DWORD)len, (BYTE *)value}, sealed = {};

  if (!sealed_path(key, path, sizeof path) ||
      !CryptProtectData(&plain, L"Quarrel", nullptr, nullptr, nullptr,
                        CRYPTPROTECT_UI_FORBIDDEN, &sealed)) {
    return false;
  }

  char writing[1216];

  snprintf(writing, sizeof writing, "%s.part", path);

  FILE *file = fopen(writing, "wb");
  bool ok = file != nullptr &&
            fwrite(sealed.pbData, 1, sealed.cbData, file) == sealed.cbData;

  ok = file != nullptr && fclose(file) == 0 && ok;
  LocalFree(sealed.pbData);
  return ok && rename(writing, path) == 0;
}

static char *unseal(const char *key, bool *missing) {
  char path[1200];
  FILE *file =
    sealed_path(key, path, sizeof path) ? fopen(path, "rb") : nullptr;

  *missing = file == nullptr;

  if (file == nullptr) {
    return nullptr;
  }

  uint8_t *bytes = nullptr;
  size_t len = 0, cap = 0;

  for (;;) {
    if (len == cap) {
      cap = cap ? cap * 2 : 4096;

      uint8_t *grown = realloc(bytes, cap);

      if (grown == nullptr) {
        break;
      }

      bytes = grown;
    }

    size_t n = fread(bytes + len, 1, cap - len, file);

    len += n;

    if (n == 0) {
      break;
    }
  }

  fclose(file);

  DATA_BLOB sealed = {(DWORD)len, bytes}, plain = {};
  char *value = nullptr;

  if (bytes != nullptr &&
      CryptUnprotectData(&sealed, nullptr, nullptr, nullptr, nullptr,
                         CRYPTPROTECT_UI_FORBIDDEN, &plain)) {
    value = malloc(plain.cbData + 1);

    if (value != nullptr) {
      memcpy(value, plain.pbData, plain.cbData);
      value[plain.cbData] = '\0';
    }

    LocalFree(plain.pbData);
  }

  free(bytes);
  return value;
}

static void unlink_sealed(const char *key) {
  char path[1200];
  wchar_t *wide =
    sealed_path(key, path, sizeof path) ? win_wide(path) : nullptr;

  if (wide != nullptr) {
    DeleteFileW(wide);
  }

  free(wide);
}

static bool forget(const wchar_t *name) {
  return CredDeleteW(name, CRED_TYPE_GENERIC, 0) ||
         GetLastError() == ERROR_NOT_FOUND;
}

bool platform_secret_set(const char *key, const char *value) {
  wchar_t *name = key != nullptr && value != nullptr ? target(key) : nullptr;

  if (name == nullptr) {
    return false;
  }

  size_t len = strlen(value);
  bool kept = false;

  if (len <= CRED_MAX_CREDENTIAL_BLOB_SIZE) {
    CREDENTIALW credential = {
      .Type = CRED_TYPE_GENERIC,
      .TargetName = name,
      .CredentialBlobSize = (DWORD)len,
      .CredentialBlob = (BYTE *)value,
      .Persist = CRED_PERSIST_LOCAL_MACHINE,
      .UserName = L"Quarrel",
    };

    kept = CredWriteW(&credential, 0);

    if (kept) {
      unlink_sealed(key);
    }
  }
  else {
    kept = seal(key, value, len) && forget(name);
  }

  free(name);
  return kept;
}

char *platform_secret_get(const char *key, bool *missing) {
  wchar_t *name = key != nullptr ? target(key) : nullptr;
  CREDENTIALW *found = nullptr;
  bool none = false;
  char *value = nullptr;

  if (name != nullptr && CredReadW(name, CRED_TYPE_GENERIC, 0, &found)) {
    value = malloc(found->CredentialBlobSize + 1);

    if (value != nullptr) {
      memcpy(value, found->CredentialBlob, found->CredentialBlobSize);
      value[found->CredentialBlobSize] = '\0';
    }

    CredFree(found);
  }
  else if (name != nullptr && GetLastError() == ERROR_NOT_FOUND) {
    value = unseal(key, &none);
  }

  if (missing != nullptr) {
    *missing = none;
  }

  free(name);
  return value;
}

bool platform_secret_delete(const char *key) {
  wchar_t *name = key != nullptr ? target(key) : nullptr;
  bool gone = name != nullptr && forget(name);

  if (gone) {
    unlink_sealed(key);
  }

  free(name);
  return gone;
}

void platform_pref_set(const char *key, const char *value) {
  wchar_t *name = win_wide(key);
  wchar_t *data = win_wide(value);

  if (name == nullptr) {
    free(data);
    return;
  }

  if (value == nullptr) {
    RegDeleteKeyValueW(HKEY_CURRENT_USER, PREFS, name);
  }
  else if (data != nullptr) {
    RegSetKeyValueW(HKEY_CURRENT_USER, PREFS, name, REG_SZ, data,
                    (DWORD)((wcslen(data) + 1) * sizeof *data));
  }

  free(name);
  free(data);
}

char *platform_pref_get(const char *key) {
  wchar_t *name = win_wide(key);
  DWORD size = 0;
  char *value = nullptr;

  if (name != nullptr &&
      RegGetValueW(HKEY_CURRENT_USER, PREFS, name, RRF_RT_REG_SZ, nullptr,
                   nullptr, &size) == ERROR_SUCCESS) {
    wchar_t *data = malloc(size + sizeof(wchar_t));

    if (data != nullptr &&
        RegGetValueW(HKEY_CURRENT_USER, PREFS, name, RRF_RT_REG_SZ, nullptr,
                     data, &size) == ERROR_SUCCESS) {
      value = win_utf8(data);
    }

    free(data);
  }

  free(name);
  return value;
}
