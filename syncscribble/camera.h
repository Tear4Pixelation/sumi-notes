#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "ulib/image.h"

// Live camera capture, for scanning a page on desktop.  Android and iOS do not use this: there the
//  system camera UI takes the photo (see ScribbleApp::scanDocument).  On desktop nothing like that
//  exists, so each OS gets its own small backend - V4L2 on Linux, Media Foundation on Windows,
//  AVFoundation on macOS - and every other platform compiles the stub in camera.cpp, which reports no
//  cameras so the scan falls back to the file picker.
// Frames arrive on a backend-owned thread; the UI polls takeFrame() from a timer rather than being
//  woken per frame, since a dropped preview frame is harmless and a timer needs no cross-thread event.

struct CameraInfo
{
  std::string id;  // device path, symbolic link or unique id - whatever the backend opens by
  std::string name;
};

class Camera
{
public:
  // cameras that can deliver video right now; cheap enough to call each time a scan starts
  static std::vector<CameraInfo> list();
  // starts capturing immediately; never NULL - a failure shows up as error() instead
  static std::unique_ptr<Camera> open(const CameraInfo& info);

  virtual ~Camera() {}
  // moves the newest frame into *frame if one arrived since the last call (RGBA, top row first)
  bool takeFrame(Image* frame);
  // empty unless the camera could not be opened or stopped delivering frames
  std::string error() const;

protected:
  // for backends, from their capture thread
  void publishFrame(Image&& frame);
  void setError(const std::string& msg);

private:
  mutable std::mutex mutex;
  Image latest{0, 0};
  bool hasNew = false;
  std::string errorMsg;
};

// UVC webcams send MJPEG frames without the Huffman tables (DHT), relying on the decoder to assume the
//  standard ones from the JPEG spec.  stb_image does not assume them, and worse, does not fail either -
//  it decodes such a frame into noise - so insert them.  Exposed for the backends.
Image decodeMjpegFrame(const unsigned char* data, size_t len);
// packed YUYV 4:2:2 (BT.601, limited range) to RGBA
void yuyvToRGBA(const unsigned char* src, int width, int height, int srcStride, unsigned char* dst);
