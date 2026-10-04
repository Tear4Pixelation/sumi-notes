#pragma once

#include <functional>
#include <string>
#include "ulib/platformutil.h"

union SDL_Event;

// The operating system's own file dialogs, everywhere a file is picked - never Sumi's DocumentList, which
//  remains only as the last resort where no system dialog exists (wasm, or a Linux desktop with neither
//  the xdg-desktop-portal nor zenity/kdialog). See docs/agent/document-library.md.
//
// Results always arrive later, from the main event loop, never from inside openFile()/saveFile(): iOS and
//  Android only report a pick asynchronously, so callers are written for that everywhere, and the same
//  code path is what runs (and gets tested) on desktop, where the dialog itself is modal.
namespace FilePicker {

// path of a file the app can read, or "" if the user cancelled. On Android, and on iOS for a file outside
//  the app's own storage, it is a copy in temporary storage, named like the original.
typedef std::function<void(const std::string& path)> PickedFn;
// writes the file to save at the given path; returns false on failure
typedef std::function<bool(const std::string& path)> WriteFn;
// the classic in-app browser, supplied by ScribbleApp: (save, exts, suggested name) -> path or ""
typedef std::function<std::string(bool save, const char* exts, const std::string& name)> FallbackFn;

// exts: space separated, without dots, e.g. "pdf" or "svgz svg html"; NULL or "" for any file
void openFile(const char* title, const char* exts, PickedFn onPicked);

// Asks where to save, then has write() produce the file. On desktop write() gets the destination itself;
//  on Android and iOS it fills a temporary file, which the system's export dialog then copies where the
//  user chooses - nothing there keeps referring to the destination afterwards. onDone (optional) gets
//  the destination, or "" if cancelled or the write failed.
void saveFile(const char* title, const std::string& suggestedName, const char* exts, WriteFn write,
    PickedFn onDone = PickedFn());

#if !PLATFORM_MOBILE
// Desktop only, synchronous: the destination chosen in the system's save dialog, or "". Save As needs
//  this, since what it saves keeps living at that path.
std::string savePath(const char* title, const std::string& suggestedName, const char* exts);
#endif

void setFallback(FallbackFn fallback);
void setTempDir(const std::string& dir);

// ScribbleApp's event handler passes every event here first; true if it was a picker result
bool handleEvent(SDL_Event* event);

// platform code reports a result: path "" or NULL = cancelled. Safe from any thread.
void deliver(int requestId, const char* path);

}  // namespace FilePicker
