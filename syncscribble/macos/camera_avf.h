#pragma once

// Plain C interface to the AVFoundation camera code in camera_avf.m, so the C++ side (camera_mac.cpp)
//  needs no Objective-C++ build rule.  Callbacks run on an AVFoundation queue, never the main thread.

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*MacCameraAddFn)(void* user, const char* id, const char* name);
// bgra is only valid during the call
typedef void (*MacCameraFrameFn)(void* user, const unsigned char* bgra, int width, int height, int stride);
typedef void (*MacCameraErrorFn)(void* user, const char* msg);

void macCameraList(MacCameraAddFn add, void* user);
// never NULL; failures, including permission denied, are reported through onError
void* macCameraOpen(const char* id, MacCameraFrameFn onFrame, MacCameraErrorFn onError, void* user);
// blocks until no further callback can run
void macCameraClose(void* camera);

#ifdef __cplusplus
}
#endif
