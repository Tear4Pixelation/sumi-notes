// Camera backend for Windows: Media Foundation's source reader, asked for RGB32 so that it inserts
//  whatever decoder the camera's native format (usually MJPEG or NV12/YUY2) needs.

#include "../camera.h"

#include <thread>
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

template<class T> static void releaseCom(T*& obj)
{
  if(obj) {
    obj->Release();
    obj = NULL;
  }
}

static std::string wideToUtf8(const wchar_t* wide)
{
  if(!wide)
    return "";
  int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
  if(len <= 1)
    return "";
  std::string utf8(len - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, &utf8[0], len, NULL, NULL);
  return utf8;
}

static std::wstring utf8ToWide(const std::string& utf8)
{
  int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, NULL, 0);
  if(len <= 1)
    return L"";
  std::wstring wide(len - 1, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], len);
  return wide;
}

// COM and MFStartup are both reference counted per thread/process, so each use brackets its own.  The
//  main thread may already be in a single-threaded apartment (SDL, OLE drag and drop); asking for the
//  same kind there is harmless, and CoUninitialize is only owed when CoInitializeEx succeeded.
struct MediaFoundationScope
{
  explicit MediaFoundationScope(DWORD apartment) {
    comResult = CoInitializeEx(NULL, apartment);
    started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
  }
  ~MediaFoundationScope() {
    if(started)
      MFShutdown();
    if(SUCCEEDED(comResult))
      CoUninitialize();
  }
  HRESULT comResult;
  bool started;
};

static IMFAttributes* videoCaptureAttributes(const std::wstring* symbolicLink)
{
  IMFAttributes* attrs = NULL;
  if(FAILED(MFCreateAttributes(&attrs, 2)))
    return NULL;
  attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  if(symbolicLink)
    attrs->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, symbolicLink->c_str());
  return attrs;
}

static std::string allocatedString(IMFActivate* device, REFGUID key)
{
  WCHAR* value = NULL;
  UINT32 len = 0;
  if(FAILED(device->GetAllocatedString(key, &value, &len)))
    return "";
  std::string utf8 = wideToUtf8(value);
  CoTaskMemFree(value);
  return utf8;
}

std::vector<CameraInfo> Camera::list()
{
  std::vector<CameraInfo> cameras;
  MediaFoundationScope scope(COINIT_APARTMENTTHREADED);
  if(!scope.started)
    return cameras;
  IMFAttributes* attrs = videoCaptureAttributes(NULL);
  if(!attrs)
    return cameras;
  IMFActivate** devices = NULL;
  UINT32 count = 0;
  if(SUCCEEDED(MFEnumDeviceSources(attrs, &devices, &count))) {
    for(UINT32 ii = 0; ii < count; ++ii) {
      std::string id = allocatedString(devices[ii], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
      std::string name = allocatedString(devices[ii], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
      if(!id.empty())
        cameras.push_back({id, name.empty() ? id : name});
      devices[ii]->Release();
    }
    CoTaskMemFree(devices);
  }
  attrs->Release();
  return cameras;
}

namespace {

class MFCamera : public Camera
{
public:
  explicit MFCamera(const CameraInfo& info) : symbolicLink(utf8ToWide(info.id))
  {
    thread = std::thread(&MFCamera::captureThread, this);
  }
  ~MFCamera() override
  {
    stopping = true;
    if(thread.joinable())
      thread.join();
  }

private:
  void captureThread();
  bool configure(IMFSourceReader* reader);
  void convertSample(IMFSample* sample);

  std::wstring symbolicLink;
  std::thread thread;
  std::atomic<bool> stopping{false};
  UINT32 width = 0, height = 0;
  LONG defaultStride = 0;
};

static const DWORD VIDEO_STREAM = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;

// The largest native mode wins, as on the other platforms: a scan wants every pixel it can get.  Then
//  ask for RGB32 on top of it - the reader converts.
bool MFCamera::configure(IMFSourceReader* reader)
{
  IMFMediaType* best = NULL;
  UINT64 bestArea = 0;
  for(DWORD ii = 0; ; ++ii) {
    IMFMediaType* native = NULL;
    if(FAILED(reader->GetNativeMediaType(VIDEO_STREAM, ii, &native)))
      break;
    UINT32 nativeWidth = 0, nativeHeight = 0;
    MFGetAttributeSize(native, MF_MT_FRAME_SIZE, &nativeWidth, &nativeHeight);
    if(UINT64(nativeWidth)*nativeHeight > bestArea) {
      bestArea = UINT64(nativeWidth)*nativeHeight;
      releaseCom(best);
      best = native;
    }
    else
      native->Release();
  }
  if(best) {
    reader->SetCurrentMediaType(VIDEO_STREAM, NULL, best);
    best->Release();
  }

  IMFMediaType* output = NULL;
  if(FAILED(MFCreateMediaType(&output)))
    return false;
  output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
  HRESULT res = reader->SetCurrentMediaType(VIDEO_STREAM, NULL, output);
  output->Release();
  if(FAILED(res))
    return false;

  IMFMediaType* current = NULL;
  if(FAILED(reader->GetCurrentMediaType(VIDEO_STREAM, &current)))
    return false;
  MFGetAttributeSize(current, MF_MT_FRAME_SIZE, &width, &height);
  UINT32 stride = 0;
  // negative for a bottom-up image; absent means top-down with no padding
  defaultStride = SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? (LONG)stride : LONG(width*4);
  current->Release();
  return width > 0 && height > 0;
}

void MFCamera::convertSample(IMFSample* sample)
{
  IMFMediaBuffer* buffer = NULL;
  if(FAILED(sample->ConvertToContiguousBuffer(&buffer)))
    return;
  BYTE* scan0 = NULL;
  LONG pitch = 0;
  IMF2DBuffer* buffer2d = NULL;
  BYTE* locked = NULL;
  // a 2D buffer knows its own orientation; a plain one needs the media type's stride to find row 0
  if(SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2d))) && SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch))) {}
  else {
    releaseCom(buffer2d);
    DWORD len = 0;
    if(FAILED(buffer->Lock(&locked, NULL, &len)) || len < DWORD(abs(defaultStride))*height) {
      if(locked)
        buffer->Unlock();
      buffer->Release();
      return;
    }
    pitch = defaultStride;
    scan0 = pitch < 0 ? locked + size_t(-pitch)*(height - 1) : locked;
  }

  Image frame(width, height, Image::JPEG);
  unsigned char* out = frame.bytes();
  for(UINT32 row = 0; row < height; ++row) {
    const BYTE* in = scan0 + ptrdiff_t(row)*pitch;
    for(UINT32 col = 0; col < width; ++col, in += 4, out += 4) {
      out[0] = in[2];  // RGB32 is B, G, R, X in memory
      out[1] = in[1];
      out[2] = in[0];
      out[3] = 255;
    }
  }

  if(buffer2d) {
    buffer2d->Unlock2D();
    buffer2d->Release();
  }
  else
    buffer->Unlock();
  buffer->Release();
  publishFrame(std::move(frame));
}

void MFCamera::captureThread()
{
  MediaFoundationScope scope(COINIT_MULTITHREADED);
  if(!scope.started) {
    setError("The camera could not be opened");
    return;
  }
  IMFAttributes* deviceAttrs = videoCaptureAttributes(&symbolicLink);
  IMFMediaSource* source = NULL;
  HRESULT res = deviceAttrs ? MFCreateDeviceSource(deviceAttrs, &source) : E_FAIL;
  releaseCom(deviceAttrs);
  if(FAILED(res)) {
    // E_ACCESSDENIED is the camera privacy switch in Windows Settings
    setError(res == E_ACCESSDENIED ? "Camera access is turned off in Windows Settings, Privacy & security"
        : "The camera could not be opened");
    return;
  }

  IMFAttributes* readerAttrs = NULL;
  IMFSourceReader* reader = NULL;
  if(SUCCEEDED(MFCreateAttributes(&readerAttrs, 1))) {
    // lets the reader convert to RGB32 itself rather than refusing any type the camera lacks
    readerAttrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    res = MFCreateSourceReaderFromMediaSource(source, readerAttrs, &reader);
    releaseCom(readerAttrs);
  }
  if(!reader || FAILED(res) || !configure(reader)) {
    setError("The camera does not offer a supported video format");
    releaseCom(reader);
    source->Shutdown();
    source->Release();
    return;
  }

  while(!stopping) {
    DWORD streamIndex = 0, flags = 0;
    LONGLONG timestamp = 0;
    IMFSample* sample = NULL;
    res = reader->ReadSample(VIDEO_STREAM, 0, &streamIndex, &flags, &timestamp, &sample);
    if(FAILED(res) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
      setError(res == MF_E_VIDEO_RECORDING_DEVICE_LOCKED ? "The camera is in use by another application"
          : "The camera stopped responding");
      releaseCom(sample);
      break;
    }
    if(flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
      IMFMediaType* current = NULL;
      if(SUCCEEDED(reader->GetCurrentMediaType(VIDEO_STREAM, &current))) {
        MFGetAttributeSize(current, MF_MT_FRAME_SIZE, &width, &height);
        UINT32 stride = 0;
        defaultStride = SUCCEEDED(current->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ? (LONG)stride : LONG(width*4);
        current->Release();
      }
    }
    if(sample) {
      convertSample(sample);
      sample->Release();
    }
  }
  reader->Release();
  source->Shutdown();
  source->Release();
}

}  // namespace

std::unique_ptr<Camera> Camera::open(const CameraInfo& info)
{
  return std::unique_ptr<Camera>(new MFCamera(info));
}
