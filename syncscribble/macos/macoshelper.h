#ifndef MACOSHELPER_H
#define MACOSHELPER_H

#ifdef __cplusplus
extern "C" {
#endif
extern void macosWaitEvent(void);
//extern void macosPumpEvents(void);
extern void macosWakeEventLoop(void);
extern void macosDisableMouseCoalescing(void);
extern int macosOpenUrl(const char* url);
extern int macosClipboardChangeCount(void);
// NSOpenPanel / NSSavePanel; a malloc'd path, or NULL if cancelled (FilePicker in filepicker.cpp)
extern char* macosFileDialog(int save, const char* title, const char* name, const char* exts);
#ifdef __cplusplus
}
#endif

#endif
