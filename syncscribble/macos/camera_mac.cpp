// Camera backend for macOS: the Camera class over the AVFoundation code in camera_avf.m.

#include "../camera.h"
#include "camera_avf.h"

static void addCamera(void* user, const char* id, const char* name)
{
  static_cast<std::vector<CameraInfo>*>(user)->push_back({id ? id : "", name ? name : ""});
}

std::vector<CameraInfo> Camera::list()
{
  std::vector<CameraInfo> cameras;
  macCameraList(addCamera, &cameras);
  return cameras;
}

namespace {

class MacCamera : public Camera
{
public:
  explicit MacCamera(const CameraInfo& info) { handle = macCameraOpen(info.id.c_str(), onFrame, onError, this); }
  ~MacCamera() override { macCameraClose(handle); }

private:
  static void onFrame(void* user, const unsigned char* bgra, int width, int height, int stride);
  static void onError(void* user, const char* msg) { static_cast<MacCamera*>(user)->setError(msg); }

  void* handle;
};

void MacCamera::onFrame(void* user, const unsigned char* bgra, int width, int height, int stride)
{
  Image frame(width, height, Image::JPEG);
  unsigned char* out = frame.bytes();
  for(int row = 0; row < height; ++row) {
    const unsigned char* in = bgra + size_t(row)*stride;
    for(int col = 0; col < width; ++col, in += 4, out += 4) {
      out[0] = in[2];
      out[1] = in[1];
      out[2] = in[0];
      out[3] = 255;
    }
  }
  static_cast<MacCamera*>(user)->publishFrame(std::move(frame));
}

}  // namespace

std::unique_ptr<Camera> Camera::open(const CameraInfo& info)
{
  return std::unique_ptr<Camera>(new MacCamera(info));
}
