#include <initguid.h>
#include <windows.h>

#include <inttypes.h>
#include <notificationactivationcallback.h>
#include <roapi.h>
#include <shobjidl.h>
#include <stdcountof.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.data.xml.dom.h>
#include <windows.ui.notifications.h>
#include <winstring.h>

#include <civue/window.h>

#include "win.h"

/* ========================================================================== */
/* toasts through winrt, as an unpackaged app shows them: under an app user   */
/* model id the registry names, with no sound of the system's since Quarrel   */
/* plays Discord's. a toast is tagged with the app's id in digits, and kept   */
/* while it may be taken back.                                                */
/* ========================================================================== */

#define AUMID L"com.vexcited.quarrel"
#define GROUP L"quarrel"

// the class windows tells of what is pressed, see below.
DEFINE_GUID(ACTIVATOR, 0x8b156694, 0x5ca4, 0x4a65, 0x9b, 0x60, 0x00, 0x77, 0xe4,
            0x94, 0xe0, 0xc4);

// the most kept at once, the action center showing as many of an app.
constexpr size_t KEPT_MOST = 20;

typedef __x_ABI_CWindows_CUI_CNotifications_CIToastNotificationManagerStatics
  Manager;
typedef __x_ABI_CWindows_CUI_CNotifications_CIToastNotificationManagerStatics2
  Manager2;
#define IID_MANAGER2 \
  IID___x_ABI_CWindows_CUI_CNotifications_CIToastNotificationManagerStatics2
typedef __x_ABI_CWindows_CUI_CNotifications_CIToastNotificationFactory Factory;
typedef __x_ABI_CWindows_CUI_CNotifications_CIToastNotificationHistory History;
typedef __x_ABI_CWindows_CUI_CNotifications_CIToastNotifier Notifier;
typedef __x_ABI_CWindows_CUI_CNotifications_CIToastNotification Toast;
typedef __x_ABI_CWindows_CUI_CNotifications_CIToastNotification2 Toast2;
typedef __x_ABI_CWindows_CData_CXml_CDom_CIXmlDocument Xml;
typedef __x_ABI_CWindows_CData_CXml_CDom_CIXmlDocumentIO XmlIO;

typedef struct {
  uint64_t id;
  Toast *toast;
} Shown;

static struct {
  bool tried;
  Manager *manager;
  Notifier *notifier;
  Factory *factory;
  History *history;
  Shown shown[KEPT_MOST];
  size_t nshown;
  void (*activated)(void *ctx, uint64_t id, PlatformNotifyAction action);
  void *activated_ctx;
} N;

// the taskbar groups the windows of the process under it, which it is told
// before any opens.
[[gnu::constructor]] static void name_process(void) {
  SetCurrentProcessExplicitAppUserModelID(AUMID);
}

static HSTRING string_of(const wchar_t *s) {
  HSTRING made = nullptr;

  WindowsCreateString(s, (UINT32)wcslen(s), &made);
  return made;
}

static void *factory_of(const wchar_t *name, const IID *iid) {
  HSTRING class = string_of(name);
  void *made = nullptr;

  if (FAILED(RoGetActivationFactory(class, iid, &made))) {
    made = nullptr;
  }

  WindowsDeleteString(class);
  return made;
}

static void set_string(const wchar_t *key, const wchar_t *name,
                       const wchar_t *value) {
  RegSetKeyValueW(HKEY_CURRENT_USER, key, name, REG_SZ, value,
                  (DWORD)((wcslen(value) + 1) * sizeof *value));
}

// an unpackaged app shows toasts under an id the registry gives a name and
// the class of what is pressed, which windows launches this program for
// when it does not run.
static void register_id(void) {
  wchar_t class[40], exe[MAX_PATH], key[96], command[MAX_PATH + 2];
  DWORD len = GetModuleFileNameW(nullptr, exe, countof(exe));

  StringFromGUID2(&ACTIVATOR, class, countof(class));
  set_string(L"Software\\Classes\\AppUserModelId\\" AUMID, L"DisplayName",
             L"Quarrel");
  set_string(L"Software\\Classes\\AppUserModelId\\" AUMID, L"CustomActivator",
             class);

  if (len > 0 && len < countof(exe)) {
    _snwprintf(key, countof(key),
               L"Software\\Classes\\CLSID\\%ls\\LocalServer32", class);
    _snwprintf(command, countof(command), L"\"%ls\"", exe);
    set_string(key, nullptr, command);
  }
}

static void listen(void);

static bool start(void) {
  if (N.tried) {
    return N.notifier != nullptr;
  }

  N.tried = true;
  listen();

  HRESULT hr = RoInitialize(RO_INIT_SINGLETHREADED);

  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
    return false;
  }

  N.manager = factory_of(
    RuntimeClass_Windows_UI_Notifications_ToastNotificationManager,
    &IID___x_ABI_CWindows_CUI_CNotifications_CIToastNotificationManagerStatics);
  N.factory = factory_of(
    RuntimeClass_Windows_UI_Notifications_ToastNotification,
    &IID___x_ABI_CWindows_CUI_CNotifications_CIToastNotificationFactory);

  Manager2 *manager2 =
    factory_of(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager,
               &IID_MANAGER2);

  if (manager2 != nullptr) {
    manager2->lpVtbl->get_History(manager2, &N.history);
    manager2->lpVtbl->Release(manager2);
  }

  HSTRING id = string_of(AUMID);

  if (N.manager != nullptr && N.factory != nullptr &&
      FAILED(N.manager->lpVtbl->CreateToastNotifierWithId(N.manager, id,
                                                          &N.notifier))) {
    N.notifier = nullptr;
  }

  WindowsDeleteString(id);
  return N.notifier != nullptr;
}

/* ========================================================================== */
/* what is pressed, which windows tells an unpackaged app through a com class */
/* it registers alone: the toasts' own events never come to one. it says the  */
/* arguments of the toast or of its button, which name the toast's id.        */
/* ========================================================================== */

typedef struct {
  uint64_t id;
  PlatformNotifyAction action;
} Pressed;

static Shown *find(uint64_t id) {
  for (size_t i = 0; i < N.nshown; i++) {
    if (N.shown[i].id == id) {
      return &N.shown[i];
    }
  }

  return nullptr;
}

static void forget(Shown *s) {
  s->toast->lpVtbl->Release(s->toast);

  // the oldest first.
  size_t at = (size_t)(s - N.shown);

  memmove(s, s + 1, (N.nshown - at - 1) * sizeof *s);
  N.nshown--;
}

// one pressed in the action center may be older than the ones kept, or
// than the process, which windows launched for it.
static void on_pressed(void *ctx) {
  Pressed p = *(Pressed *)ctx;
  Shown *s = find(p.id);

  free(ctx);

  if (s != nullptr) {
    forget(s);
  }

  // what is pressed is taken away, the window comes forward but to decline.
  platform_notify_close(p.id);

  if (p.action != PLATFORM_NOTIFY_DECLINED) {
    civue_window_raise();
  }

  if (N.activated != nullptr) {
    N.activated(N.activated_ctx, p.id, p.action);
  }
}

static HRESULT STDMETHODCALLTYPE activate(INotificationActivationCallback *,
                                          LPCWSTR, LPCWSTR args,
                                          const NOTIFICATION_USER_INPUT_DATA *,
                                          ULONG) {
  wchar_t verb[8] = L"";
  unsigned long long id = 0;
  Pressed *p = nullptr;

  if (args == nullptr || swscanf(args, L"%7ls %llu", verb, &id) != 2 ||
      id == 0 || (p = malloc(sizeof *p)) == nullptr) {
    return S_OK;
  }

  *p = (Pressed){
    id,
    wcscmp(verb, L"answer") == 0    ? PLATFORM_NOTIFY_ANSWERED
    : wcscmp(verb, L"decline") == 0 ? PLATFORM_NOTIFY_DECLINED
                                    : PLATFORM_NOTIFY_OPENED,
  };
  platform_post(on_pressed, p);
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE
callback_query(INotificationActivationCallback *self, REFIID iid, void **out) {
  if (IsEqualIID(iid, &IID_IUnknown) ||
      IsEqualIID(iid, &IID_INotificationActivationCallback)) {
    *out = self;
    return S_OK;
  }

  *out = nullptr;
  return E_NOINTERFACE;
}

// the callback and its factory last as long as the process.
static ULONG STDMETHODCALLTYPE
callback_retain(INotificationActivationCallback *) {
  return 1;
}

static const INotificationActivationCallbackVtbl CALLBACK_VTBL = {
  callback_query, callback_retain, callback_retain, activate};
static INotificationActivationCallback callback = {
  (INotificationActivationCallbackVtbl *)&CALLBACK_VTBL};

static HRESULT STDMETHODCALLTYPE factory_query(IClassFactory *self, REFIID iid,
                                               void **out) {
  if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IClassFactory)) {
    *out = self;
    return S_OK;
  }

  *out = nullptr;
  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE factory_retain(IClassFactory *) {
  return 1;
}

static HRESULT STDMETHODCALLTYPE create(IClassFactory *, IUnknown *outer,
                                        REFIID iid, void **out) {
  if (outer != nullptr) {
    *out = nullptr;
    return CLASS_E_NOAGGREGATION;
  }

  return callback_query(&callback, iid, out);
}

static HRESULT STDMETHODCALLTYPE lock(IClassFactory *, BOOL) {
  return S_OK;
}

static const IClassFactoryVtbl FACTORY_VTBL = {factory_query, factory_retain,
                                               factory_retain, create, lock};
static IClassFactory activator = {(IClassFactoryVtbl *)&FACTORY_VTBL};

// the main thread's apartment takes the calls, through its messages.
static void listen(void) {
  static bool listening;
  DWORD cookie = 0;

  if (listening || !win_com()) {
    return;
  }

  listening = true;
  register_id();
  CoRegisterClassObject(&ACTIVATOR, (IUnknown *)&activator, CLSCTX_LOCAL_SERVER,
                        REGCLS_MULTIPLEUSE, &cookie);
}

/* ========================================================================== */
/* the toast's xml.                                                           */
/* ========================================================================== */

typedef struct {
  char *v;
  size_t len, cap;
} Text;

static void add(Text *t, const char *s, size_t n) {
  if (t->len + n + 1 > t->cap) {
    size_t cap = (t->len + n + 1) * 2;
    char *grown = realloc(t->v, cap);

    if (grown == nullptr) {
      abort();
    }

    t->v = grown;
    t->cap = cap;
  }

  memcpy(t->v + t->len, s, n);
  t->len += n;
  t->v[t->len] = '\0';
}

static void add_str(Text *t, const char *s) {
  add(t, s, strlen(s));
}

// text as xml reads it back the same, without what xml may not hold.
static void add_escaped(Text *t, const char *s) {
  for (; s != nullptr && *s; s++) {
    unsigned char c = (unsigned char)*s;

    switch (c) {
      case '&':
        add_str(t, "&amp;");
        break;
      case '<':
        add_str(t, "&lt;");
        break;
      case '>':
        add_str(t, "&gt;");
        break;
      case '"':
        add_str(t, "&quot;");
        break;
      case '\'':
        add_str(t, "&apos;");
        break;
      default:
        if (c >= 0x20 || c == '\t' || c == '\n') {
          add(t, s, 1);
        }
    }
  }
}

// a path as the uri of a file, every byte past the plain ones escaped.
static void add_file_uri(Text *t, const char *path) {
  add_str(t, "file:///");

  for (const unsigned char *c = (const unsigned char *)path; *c; c++) {
    char hex[4];

    if (*c == '\\') {
      add_str(t, "/");
    }
    else if ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
             (*c >= '0' && *c <= '9') || strchr("-._~/:", *c) != nullptr) {
      add(t, (const char *)c, 1);
    }
    else {
      snprintf(hex, sizeof hex, "%%%02X", *c);
      add_str(t, hex);
    }
  }
}

// a call rings until it is taken back, with its buttons.
static char *xml_of(const PlatformNotification *n) {
  Text t = {};

  char id[24];

  snprintf(id, sizeof id, "%" PRIu64, n->id);
  add_str(&t, "<toast launch=\"open ");
  add_str(&t, id);
  add_str(&t, n->call ? "\" scenario=\"incomingCall\">" : "\">");
  add_str(&t, "<visual><binding template=\"ToastGeneric\"><text>");
  add_escaped(&t, n->title);
  add_str(&t, "</text><text>");
  add_escaped(&t, n->body);
  add_str(&t, "</text>");

  if (n->image != nullptr) {
    add_str(&t, "<image placement=\"appLogoOverride\" hint-crop=\"circle\" "
                "src=\"");
    add_file_uri(&t, n->image);
    add_str(&t, "\"/>");
  }

  add_str(&t, "</binding></visual>");

  if (n->call) {
    add_str(&t, "<actions><action content=\"Answer\" arguments=\"answer ");
    add_str(&t, id);
    add_str(&t, "\" activationType=\"foreground\"/><action content=\"Decline\" "
                "arguments=\"decline ");
    add_str(&t, id);
    add_str(&t, "\" activationType=\"background\"/></actions>");
  }

  add_str(&t, "<audio silent=\"true\"/></toast>");
  return t.v;
}

// windows' utf-16 of the xml, what is not utf-8 replaced.
static HSTRING hstring_of(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  wchar_t *wide = n > 0 ? malloc((size_t)n * sizeof *wide) : nullptr;
  HSTRING made = nullptr;

  if (wide != nullptr) {
    MultiByteToWideChar(CP_UTF8, 0, s, -1, wide, n);
    WindowsCreateString(wide, (UINT32)(n - 1), &made);
  }

  free(wide);
  return made;
}

static Xml *document(const char *text) {
  HSTRING class = string_of(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument);
  HSTRING xml = hstring_of(text);
  IInspectable *made = nullptr;
  Xml *doc = nullptr;
  XmlIO *io = nullptr;

  if (SUCCEEDED(RoActivateInstance(class, &made)) &&
      SUCCEEDED(made->lpVtbl->QueryInterface(
        made, &IID___x_ABI_CWindows_CData_CXml_CDom_CIXmlDocumentIO,
        (void **)&io)) &&
      SUCCEEDED(io->lpVtbl->LoadXml(io, xml))) {
    made->lpVtbl->QueryInterface(
      made, &IID___x_ABI_CWindows_CData_CXml_CDom_CIXmlDocument, (void **)&doc);
  }

  if (io != nullptr) {
    io->lpVtbl->Release(io);
  }

  if (made != nullptr) {
    made->lpVtbl->Release(made);
  }

  WindowsDeleteString(xml);
  WindowsDeleteString(class);
  return doc;
}

static HSTRING tag_of(uint64_t id) {
  wchar_t digits[24];

  _snwprintf(digits, 24, L"%llu", (unsigned long long)id);
  return string_of(digits);
}

void platform_notify(const PlatformNotification *n) {
  if (n == nullptr || n->id == 0 || !start()) {
    return;
  }

  char *text = xml_of(n);
  Xml *doc = document(text);
  Toast *toast = nullptr;

  free(text);

  if (doc == nullptr || FAILED(N.factory->lpVtbl->CreateToastNotification(
                          N.factory, doc, &toast))) {
    if (doc != nullptr) {
      doc->lpVtbl->Release(doc);
    }

    return;
  }

  doc->lpVtbl->Release(doc);

  Toast2 *tagged = nullptr;

  if (SUCCEEDED(toast->lpVtbl->QueryInterface(
        toast, &IID___x_ABI_CWindows_CUI_CNotifications_CIToastNotification2,
        (void **)&tagged))) {
    HSTRING tag = tag_of(n->id), group = string_of(GROUP);

    tagged->lpVtbl->put_Tag(tagged, tag);
    tagged->lpVtbl->put_Group(tagged, group);
    tagged->lpVtbl->Release(tagged);
    WindowsDeleteString(tag);
    WindowsDeleteString(group);
  }

  if (N.nshown == KEPT_MOST) {
    forget(&N.shown[0]);
  }

  Shown *s = &N.shown[N.nshown++];
  *s = (Shown){n->id, toast};

  if (FAILED(N.notifier->lpVtbl->Show(N.notifier, toast))) {
    forget(s);
  }
}

void platform_notify_close(uint64_t id) {
  if (N.notifier == nullptr || id == 0) {
    return;
  }

  Shown *s = find(id);

  if (s != nullptr) {
    N.notifier->lpVtbl->Hide(N.notifier, s->toast);
    forget(s);
  }

  // one shown before it was kept, or that the action center holds.
  if (N.history != nullptr) {
    HSTRING tag = tag_of(id), group = string_of(GROUP), app = string_of(AUMID);

    N.history->lpVtbl->RemoveGroupedTagWithId(N.history, tag, group, app);
    WindowsDeleteString(tag);
    WindowsDeleteString(group);
    WindowsDeleteString(app);
  }
}

void platform_notify_watch(void (*fn)(void *ctx, uint64_t id,
                                      PlatformNotifyAction action),
                           void *ctx) {
  N.activated = fn;
  N.activated_ctx = ctx;
  // at launch, as winrt takes a frame or more to start.
  start();
}

// nothing is asked of a notifier the app never used.
void platform_notify_clear(void) {
  if (N.notifier == nullptr) {
    return;
  }

  while (N.nshown > 0) {
    N.notifier->lpVtbl->Hide(N.notifier, N.shown[0].toast);
    forget(&N.shown[0]);
  }

  if (N.history != nullptr) {
    HSTRING app = string_of(AUMID);

    N.history->lpVtbl->ClearWithId(N.history, app);
    WindowsDeleteString(app);
  }
}
