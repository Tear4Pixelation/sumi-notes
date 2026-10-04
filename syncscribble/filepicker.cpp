#include "filepicker.h"
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include "ulib/platformutil.h"
#include "ulib/fileutil.h"
#include "ulib/stringutil.h"
#include "ugui/svggui.h"

#if PLATFORM_IOS
#include "ios/ioshelper.h"
#elif PLATFORM_ANDROID
#include "android/androidhelper.h"
#elif PLATFORM_OSX
#include "macos/macoshelper.h"
#elif PLATFORM_WIN
#include <windows.h>
#include <shobjidl.h>
#elif PLATFORM_LINUX
#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace FilePicker {

static std::mutex requestMutex;
static std::map<int, PickedFn> requests;
static int nextRequestId = 1;
static Uint32 eventType = 0;
static FallbackFn fallbackFn;
static std::string tempDir;

void setFallback(FallbackFn fallback) { fallbackFn = fallback; }
void setTempDir(const std::string& dir) { tempDir = dir; }

static int addRequest(PickedFn onResult)
{
  if(!eventType)
    eventType = SDL_RegisterEvents(1);
  std::lock_guard<std::mutex> lock(requestMutex);
  int requestId = nextRequestId++;
  requests[requestId] = std::move(onResult);
  return requestId;
}

void deliver(int requestId, const char* path)
{
  SvgGui::pushUserEvent(eventType, requestId, new std::string(path ? path : ""));
  PLATFORM_WakeEventLoop();
}

bool handleEvent(SDL_Event* event)
{
  if(!eventType || event->type != eventType)
    return false;
  std::unique_ptr<std::string> path(static_cast<std::string*>(event->user.data1));
  PickedFn onResult;
  {
    std::lock_guard<std::mutex> lock(requestMutex);
    auto request = requests.find(event->user.code);
    if(request == requests.end())
      return true;
    onResult = std::move(request->second);
    requests.erase(request);
  }
  if(onResult)
    onResult(*path);
  return true;
}

static std::vector<std::string> extList(const char* exts)
{
  std::vector<std::string> result;
  for(const StringRef& ext : splitStringRef(StringRef(exts ? exts : ""), " ", true))
    result.push_back(ext.toString());
  return result;
}

#if !PLATFORM_MOBILE

enum Status { PICKED, CANCELLED, UNAVAILABLE };

// "*.pdf *.PDF" - Linux file systems are case sensitive, and so are the portal's and zenity's globs
static std::string globList(const std::vector<std::string>& exts, const char* sep, bool upperToo)
{
  std::string globs;
  for(const std::string& ext : exts) {
    globs += (globs.empty() ? "*." : std::string(sep) + "*.") + ext;
    if(upperToo) {
      std::string upper = ext;
      std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
      globs += std::string(sep) + "*." + upper;
    }
  }
  return globs;
}

#if PLATFORM_WIN

static Status systemPick(bool save, const char* title, const std::string& name, const char* exts, std::string* result)
{
  HRESULT comInit = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  IFileDialog* dialog = NULL;
  HRESULT hr = save ?
      CoCreateInstance(__uuidof(FileSaveDialog), NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS((IFileSaveDialog**)&dialog)) :
      CoCreateInstance(__uuidof(FileOpenDialog), NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS((IFileOpenDialog**)&dialog));
  if(FAILED(hr)) {
    if(SUCCEEDED(comInit))
      CoUninitialize();
    return UNAVAILABLE;
  }
  std::vector<std::string> extv = extList(exts);
  std::wstring wtitle = utf8_to_wstr(title ? title : "");
  std::wstring wglobs = utf8_to_wstr(globList(extv, ";", false).c_str());
  std::wstring wname = utf8_to_wstr(name.c_str());
  std::wstring wext = utf8_to_wstr(extv.empty() ? "" : extv.front().c_str());
  dialog->SetTitle(wtitle.c_str());
  COMDLG_FILTERSPEC filters[] = {{wglobs.c_str(), wglobs.c_str()}, {L"*.*", L"*.*"}};
  if(!extv.empty())
    dialog->SetFileTypes(2, filters);
  if(save) {
    dialog->SetFileName(wname.c_str());
    if(!wext.empty())
      dialog->SetDefaultExtension(wext.c_str());
  }
  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
  Status status = CANCELLED;
  if(SUCCEEDED(dialog->Show(GetActiveWindow()))) {
    IShellItem* item = NULL;
    PWSTR wpath = NULL;
    if(SUCCEEDED(dialog->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &wpath))) {
      *result = wstr_to_utf8(wpath);
      status = PICKED;
      CoTaskMemFree(wpath);
    }
    if(item)
      item->Release();
  }
  dialog->Release();
  if(SUCCEEDED(comInit))
    CoUninitialize();
  return status;
}

#elif PLATFORM_OSX

static Status systemPick(bool save, const char* title, const std::string& name, const char* exts, std::string* result)
{
  char* path = macosFileDialog(save, title ? title : "", name.c_str(), exts ? exts : "");
  if(!path)
    return CANCELLED;
  *result = path;
  free(path);
  return PICKED;
}

#elif PLATFORM_LINUX

// The xdg-desktop-portal FileChooser, which is what the desktop itself (GNOME, KDE, or whatever portal
//  backend is configured) shows - including inside Flatpak. libdbus is loaded at run time rather than
//  linked, so the binary still starts without it; SDL does the same.
namespace Portal {

typedef struct DBusConnection DBusConnection;
typedef struct DBusMessage DBusMessage;
// layouts from dbus-errors.h / dbus-message.h; the iterator is opaque to us, so it only needs to be at
//  least as large as libdbus's own (which is 72 bytes on 64 bit)
struct DBusError { const char* name; const char* message; unsigned int dummy; void* padding1; };
struct DBusMessageIter { void* storage[16]; };
typedef unsigned int dbus_bool_t;
enum { BUS_SESSION = 0 };

static struct Api {
  bool loaded = false;
  void (*error_init)(DBusError*);
  void (*error_free)(DBusError*);
  dbus_bool_t (*error_is_set)(const DBusError*);
  DBusConnection* (*bus_get_private)(int, DBusError*);
  const char* (*bus_get_unique_name)(DBusConnection*);
  void (*bus_add_match)(DBusConnection*, const char*, DBusError*);
  void (*connection_set_exit_on_disconnect)(DBusConnection*, dbus_bool_t);
  void (*connection_close)(DBusConnection*);
  void (*connection_unref)(DBusConnection*);
  dbus_bool_t (*connection_read_write)(DBusConnection*, int);
  DBusMessage* (*connection_pop_message)(DBusConnection*);
  DBusMessage* (*connection_send_with_reply_and_block)(DBusConnection*, DBusMessage*, int, DBusError*);
  DBusMessage* (*message_new_method_call)(const char*, const char*, const char*, const char*);
  void (*message_unref)(DBusMessage*);
  dbus_bool_t (*message_is_signal)(DBusMessage*, const char*, const char*);
  const char* (*message_get_path)(DBusMessage*);
  void (*message_iter_init_append)(DBusMessage*, DBusMessageIter*);
  dbus_bool_t (*message_iter_append_basic)(DBusMessageIter*, int, const void*);
  dbus_bool_t (*message_iter_open_container)(DBusMessageIter*, int, const char*, DBusMessageIter*);
  dbus_bool_t (*message_iter_close_container)(DBusMessageIter*, DBusMessageIter*);
  dbus_bool_t (*message_iter_init)(DBusMessage*, DBusMessageIter*);
  int (*message_iter_get_arg_type)(DBusMessageIter*);
  void (*message_iter_get_basic)(DBusMessageIter*, void*);
  dbus_bool_t (*message_iter_next)(DBusMessageIter*);
  void (*message_iter_recurse)(DBusMessageIter*, DBusMessageIter*);
} dbus;

static bool loadDBus()
{
  static bool tried = false;
  if(tried)
    return dbus.loaded;
  tried = true;
  void* lib = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
  if(!lib)
    return false;
  bool ok = true;
#define LOAD_DBUS_FN(field) \
  ok = ok && (*(void**)&dbus.field = dlsym(lib, "dbus_" #field)) != NULL
  LOAD_DBUS_FN(error_init); LOAD_DBUS_FN(error_free); LOAD_DBUS_FN(error_is_set);
  LOAD_DBUS_FN(bus_get_private); LOAD_DBUS_FN(bus_get_unique_name); LOAD_DBUS_FN(bus_add_match);
  LOAD_DBUS_FN(connection_set_exit_on_disconnect); LOAD_DBUS_FN(connection_close);
  LOAD_DBUS_FN(connection_unref); LOAD_DBUS_FN(connection_read_write); LOAD_DBUS_FN(connection_pop_message);
  LOAD_DBUS_FN(connection_send_with_reply_and_block); LOAD_DBUS_FN(message_new_method_call);
  LOAD_DBUS_FN(message_unref); LOAD_DBUS_FN(message_is_signal); LOAD_DBUS_FN(message_get_path);
  LOAD_DBUS_FN(message_iter_init_append); LOAD_DBUS_FN(message_iter_append_basic);
  LOAD_DBUS_FN(message_iter_open_container); LOAD_DBUS_FN(message_iter_close_container);
  LOAD_DBUS_FN(message_iter_init); LOAD_DBUS_FN(message_iter_get_arg_type);
  LOAD_DBUS_FN(message_iter_get_basic); LOAD_DBUS_FN(message_iter_next); LOAD_DBUS_FN(message_iter_recurse);
#undef LOAD_DBUS_FN
  dbus.loaded = ok;
  return ok;
}

static void appendString(DBusMessageIter* iter, const char* str) { dbus.message_iter_append_basic(iter, 's', &str); }

// one a{sv} entry; fill() writes the value inside the variant
static void appendOption(DBusMessageIter* dict, const char* key, const char* signature,
    const std::function<void(DBusMessageIter*)>& fill)
{
  DBusMessageIter entry, variant;
  dbus.message_iter_open_container(dict, 'e', NULL, &entry);
  appendString(&entry, key);
  dbus.message_iter_open_container(&entry, 'v', signature, &variant);
  fill(&variant);
  dbus.message_iter_close_container(&entry, &variant);
  dbus.message_iter_close_container(dict, &entry);
}

// filters: a(sa(us)) - (label, [(0 = glob, pattern)])
static void appendFilter(DBusMessageIter* filters, const std::string& label, const std::vector<std::string>& globs)
{
  DBusMessageIter filter, patterns, pattern;
  dbus.message_iter_open_container(filters, 'r', NULL, &filter);
  appendString(&filter, label.c_str());
  dbus.message_iter_open_container(&filter, 'a', "(us)", &patterns);
  for(const std::string& glob : globs) {
    unsigned int globType = 0;
    dbus.message_iter_open_container(&patterns, 'r', NULL, &pattern);
    dbus.message_iter_append_basic(&pattern, 'u', &globType);
    appendString(&pattern, glob.c_str());
    dbus.message_iter_close_container(&patterns, &pattern);
  }
  dbus.message_iter_close_container(filters, &patterns);
  dbus.message_iter_close_container(filters, &filter);
}

static std::string uriToPath(const std::string& uri)
{
  if(!StringRef(uri).startsWith("file://"))
    return "";
  std::string path;
  for(size_t i = strlen("file://"); i < uri.size(); ++i) {
    if(uri[i] == '%' && i + 2 < uri.size()) {
      path.push_back(char(strtol(uri.substr(i + 1, 2).c_str(), NULL, 16)));
      i += 2;
    }
    else
      path.push_back(uri[i]);
  }
  return path;
}

// the Response signal: (u response, a{sv} results); results["uris"] is an array of file:// URIs
static Status parseResponse(DBusMessage* msg, std::string* result)
{
  DBusMessageIter args, results, entry, value, uris;
  unsigned int response = 2;
  if(!dbus.message_iter_init(msg, &args) || dbus.message_iter_get_arg_type(&args) != 'u')
    return CANCELLED;
  dbus.message_iter_get_basic(&args, &response);
  if(response != 0 || !dbus.message_iter_next(&args) || dbus.message_iter_get_arg_type(&args) != 'a')
    return CANCELLED;
  for(dbus.message_iter_recurse(&args, &results); dbus.message_iter_get_arg_type(&results) == 'e';
      dbus.message_iter_next(&results)) {
    const char* key = NULL;
    dbus.message_iter_recurse(&results, &entry);
    dbus.message_iter_get_basic(&entry, &key);
    if(!key || strcmp(key, "uris") != 0 || !dbus.message_iter_next(&entry))
      continue;
    dbus.message_iter_recurse(&entry, &value);  // into the variant
    if(dbus.message_iter_get_arg_type(&value) != 'a')
      continue;
    dbus.message_iter_recurse(&value, &uris);
    if(dbus.message_iter_get_arg_type(&uris) == 's') {
      const char* uri = NULL;
      dbus.message_iter_get_basic(&uris, &uri);
      *result = uriToPath(uri ? uri : "");
      return result->empty() ? CANCELLED : PICKED;
    }
  }
  return CANCELLED;
}

static Status pick(bool save, const char* title, const std::string& name, const char* exts, std::string* result)
{
  if(!loadDBus())
    return UNAVAILABLE;
  DBusError err;
  dbus.error_init(&err);
  DBusConnection* conn = dbus.bus_get_private(BUS_SESSION, &err);
  if(!conn) {
    dbus.error_free(&err);
    return UNAVAILABLE;
  }
  dbus.connection_set_exit_on_disconnect(conn, 0);

  // subscribe before asking, so a quick answer cannot be missed: the request's object path is
  //  predictable from our unique bus name and the token we pass
  static int tokenCount = 0;
  std::string token = fstring("sumi%d_%d", int(getpid()), ++tokenCount);
  std::string sender = dbus.bus_get_unique_name(conn) + 1;  // drop ':'
  std::replace(sender.begin(), sender.end(), '.', '_');
  std::string handle = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
  std::string match = "type='signal',interface='org.freedesktop.portal.Request',member='Response',path='";
  dbus.bus_add_match(conn, (match + handle + "'").c_str(), NULL);

  DBusMessage* call = dbus.message_new_method_call("org.freedesktop.portal.Desktop",
      "/org/freedesktop/portal/desktop", "org.freedesktop.portal.FileChooser", save ? "SaveFile" : "OpenFile");
  DBusMessageIter args, options, filters;
  dbus.message_iter_init_append(call, &args);
  appendString(&args, "");  // parent window: none we can name across Wayland/X11
  appendString(&args, title ? title : "");
  dbus.message_iter_open_container(&args, 'a', "{sv}", &options);
  appendOption(&options, "handle_token", "s", [&](DBusMessageIter* val){ appendString(val, token.c_str()); });
  appendOption(&options, "modal", "b", [](DBusMessageIter* val){
    dbus_bool_t modal = 1;
    dbus.message_iter_append_basic(val, 'b', &modal);
  });
  if(save)
    appendOption(&options, "current_name", "s", [&](DBusMessageIter* val){ appendString(val, name.c_str()); });
  std::vector<std::string> extv = extList(exts);
  if(!extv.empty()) {
    std::vector<std::string> globs;
    for(const StringRef& glob : splitStringRef(StringRef(globList(extv, " ", true)), " ", true))
      globs.push_back(glob.toString());
    std::string label = globList(extv, ", ", false);
    appendOption(&options, "filters", "a(sa(us))", [&](DBusMessageIter* val){
      dbus.message_iter_open_container(val, 'a', "(sa(us))", &filters);
      appendFilter(&filters, label, globs);
      appendFilter(&filters, "*", {"*"});
      dbus.message_iter_close_container(val, &filters);
    });
  }
  dbus.message_iter_close_container(&args, &options);

  DBusMessage* reply = dbus.connection_send_with_reply_and_block(conn, call, 10000, &err);
  dbus.message_unref(call);
  Status status = UNAVAILABLE;  // no portal, or no FileChooser backend: fall back
  if(reply) {
    // portals older than 0.9 do not honor handle_token and return a different path
    DBusMessageIter replyArgs;
    const char* replyHandle = NULL;
    if(dbus.message_iter_init(reply, &replyArgs) && dbus.message_iter_get_arg_type(&replyArgs) == 'o')
      dbus.message_iter_get_basic(&replyArgs, &replyHandle);
    if(replyHandle && handle != replyHandle) {
      handle = replyHandle;
      dbus.bus_add_match(conn, (match + handle + "'").c_str(), NULL);
    }
    dbus.message_unref(reply);
    // wait for the user; keep pumping SDL so the window stays responsive to the compositor (input is
    //  discarded afterwards - it was aimed at a window behind a modal dialog)
    status = CANCELLED;
    bool waiting = true;
    while(waiting && dbus.connection_read_write(conn, 50)) {
      while(DBusMessage* msg = dbus.connection_pop_message(conn)) {
        if(dbus.message_is_signal(msg, "org.freedesktop.portal.Request", "Response")
            && handle == dbus.message_get_path(msg)) {
          status = parseResponse(msg, result);
          waiting = false;
        }
        dbus.message_unref(msg);
      }
      SDL_PumpEvents();
    }
    SDL_FlushEvents(SDL_KEYDOWN, SDL_MULTIGESTURE);
  }
  else
    PLATFORM_LOG("File chooser portal unavailable: %s\n", err.message ? err.message : "");
  dbus.error_free(&err);
  dbus.connection_close(conn);
  dbus.connection_unref(conn);
  return status;
}

}  // namespace Portal

static std::string shellQuote(const std::string& str)
{
  std::string quoted = "'";
  for(char ch : str)
    quoted += ch == '\'' ? std::string("'\\''") : std::string(1, ch);
  return quoted + "'";
}

static bool hasCommand(const char* name)
{
  return system(fstring("command -v %s >/dev/null 2>&1", name).c_str()) == 0;
}

// zenity (GTK) or kdialog (KDE), for desktops without a portal; exit status 1 means cancelled
static Status runDialogCommand(const std::string& cmd, std::string* result)
{
  FILE* pipe = popen(cmd.c_str(), "r");
  if(!pipe)
    return UNAVAILABLE;
  std::string output;
  char buff[1024];
  while(fgets(buff, sizeof(buff), pipe))
    output += buff;
  int exitCode = pclose(pipe);
  // input queued while the dialog was up was aimed at it, not at us
  SDL_PumpEvents();
  SDL_FlushEvents(SDL_KEYDOWN, SDL_MULTIGESTURE);
  if(exitCode == -1 || !WIFEXITED(exitCode) || WEXITSTATUS(exitCode) > 1)
    return UNAVAILABLE;
  while(!output.empty() && (output.back() == '\n' || output.back() == '\r'))
    output.pop_back();
  if(WEXITSTATUS(exitCode) == 1 || output.empty())
    return CANCELLED;
  *result = output;
  return PICKED;
}

static Status systemPick(bool save, const char* title, const std::string& name, const char* exts, std::string* result)
{
  // SUMI_FILE_PICKER=portal|zenity|kdialog forces one; =none uses the in-app list; =fixed:/some/path
  //  answers every pick with that path without showing anything (scripted tests in the agent display,
  //  where a portal dialog would open on the user's own desktop instead)
  const char* forced = getenv("SUMI_FILE_PICKER");
  std::string mode = forced ? forced : "";
  if(StringRef(mode).startsWith("fixed:")) {
    *result = mode.substr(strlen("fixed:"));
    return result->empty() ? CANCELLED : PICKED;
  }
  if(mode == "none")
    return UNAVAILABLE;
  std::vector<std::string> extv = extList(exts);
  std::string home = getenv("HOME") ? getenv("HOME") : "/";
  if(mode.empty() || mode == "portal") {
    Status status = Portal::pick(save, title, name, exts, result);
    if(status != UNAVAILABLE || mode == "portal")
      return status;
  }
  if((mode.empty() || mode == "zenity") && hasCommand("zenity")) {
    std::string cmd = "zenity --file-selection --title=" + shellQuote(title ? title : "");
    if(save)
      cmd += " --save --filename=" + shellQuote(FSPath(home, name).c_str());
    if(!extv.empty())
      cmd += " --file-filter=" + shellQuote(globList(extv, ", ", false) + " | " + globList(extv, " ", true))
          + " --file-filter=" + shellQuote("* | *");
    Status status = runDialogCommand(cmd + " 2>/dev/null", result);
    if(status != UNAVAILABLE)
      return status;
  }
  if((mode.empty() || mode == "kdialog") && hasCommand("kdialog")) {
    std::string cmd = "kdialog --title " + shellQuote(title ? title : "")
        + (save ? " --getsavefilename " + shellQuote(FSPath(home, name).c_str()) : " --getopenfilename " + shellQuote(home));
    if(!extv.empty())
      cmd += " " + shellQuote(globList(extv, " ", true) + "|" + globList(extv, ", ", false));
    Status status = runDialogCommand(cmd + " 2>/dev/null", result);
    if(status != UNAVAILABLE)
      return status;
  }
  return UNAVAILABLE;
}

#else

static Status systemPick(bool, const char*, const std::string&, const char*, std::string*) { return UNAVAILABLE; }

#endif  // PLATFORM_WIN / OSX / LINUX

std::string savePath(const char* title, const std::string& suggestedName, const char* exts)
{
  std::string path;
  Status status = systemPick(true, title, suggestedName, exts, &path);
  if(status == UNAVAILABLE)
    return fallbackFn ? fallbackFn(true, exts, suggestedName) : "";
  // a dialog that does not add the extension itself (the portal leaves it to us)
  std::vector<std::string> extv = extList(exts);
  if(status == PICKED && !extv.empty() && FSPath(path).extension().empty())
    path += "." + extv.front();
  return status == PICKED ? path : "";
}

#endif  // !PLATFORM_MOBILE

void openFile(const char* title, const char* exts, PickedFn onPicked)
{
  int requestId = addRequest(std::move(onPicked));
#if PLATFORM_IOS
  iosPickFile(requestId, exts ? exts : "");
#elif PLATFORM_ANDROID
  AndroidHelper::pickFile(requestId, exts ? exts : "");
#else
  std::string path;
  if(systemPick(false, title, "", exts, &path) == UNAVAILABLE)
    path = fallbackFn ? fallbackFn(false, exts, "") : "";
  deliver(requestId, path.c_str());
#endif
}

void saveFile(const char* title, const std::string& suggestedName, const char* exts, WriteFn write, PickedFn onDone)
{
#if PLATFORM_MOBILE
  // written first, then handed to the system, which copies it wherever the user picks
  FSPath staged(FSPath(tempDir, "export/"), suggestedName);
  createPath(staged.parent().c_str());
  removeFile(staged.c_str());
  int requestId = addRequest(std::move(onDone));
  if(!write(staged.c_str())) {
    deliver(requestId, NULL);
    return;
  }
#if PLATFORM_IOS
  iosExportFile(requestId, staged.c_str());
#else
  AndroidHelper::exportFile(requestId, staged.c_str(), suggestedName.c_str());
#endif
#else
  std::string dest = savePath(title, suggestedName, exts);
  bool written = !dest.empty() && write(dest);
  deliver(addRequest(std::move(onDone)), written ? dest.c_str() : NULL);
#endif
}

}  // namespace FilePicker

#if PLATFORM_IOS
// called by the pickers in ioshelper.m
void filePicked(int requestId, const char* path) { FilePicker::deliver(requestId, path); }
#endif
