// Camera backend for Linux: V4L2 directly, with mmap streaming.  No libv4l or GStreamer dependency -
//  every UVC webcam offers MJPEG and/or YUYV, and those two are all this needs to understand.

#include "../camera.h"

#include <algorithm>
#include <thread>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

static int xioctl(int fd, unsigned long request, void* arg)
{
  int res;
  do { res = ioctl(fd, request, arg); } while(res < 0 && errno == EINTR);
  return res;
}

// a UVC camera usually shows up as two nodes, the second carrying only metadata - device_caps (rather
//  than capabilities, which describes the whole physical device) is what tells them apart
static bool isVideoCapture(int fd, std::string* name)
{
  v4l2_capability cap = {};
  if(xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0)
    return false;
  unsigned int caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
  if(!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING))
    return false;
  if(name)
    *name = (const char*)cap.card;
  return true;
}

std::vector<CameraInfo> Camera::list()
{
  std::vector<std::pair<int, std::string>> nodes;
  if(DIR* dir = opendir("/dev")) {
    while(dirent* entry = readdir(dir)) {
      if(strncmp(entry->d_name, "video", 5) == 0 && entry->d_name[5] >= '0' && entry->d_name[5] <= '9')
        nodes.emplace_back(atoi(entry->d_name + 5), std::string("/dev/") + entry->d_name);
    }
    closedir(dir);
  }
  std::sort(nodes.begin(), nodes.end());

  std::vector<CameraInfo> cameras;
  for(auto& node : nodes) {
    int fd = ::open(node.second.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if(fd < 0)
      continue;
    std::string name;
    if(isVideoCapture(fd, &name))
      cameras.push_back({node.second, name.empty() ? node.second : name});
    ::close(fd);
  }
  return cameras;
}

namespace {

class V4L2Camera : public Camera
{
public:
  explicit V4L2Camera(const CameraInfo& info);
  ~V4L2Camera() override;

private:
  bool start(const std::string& path);
  bool chooseFormat();
  void captureLoop();
  void convertFrame(const unsigned char* data, size_t len);

  int fd = -1;
  unsigned int pixelFormat = 0;
  int width = 0, height = 0, stride = 0;
  std::vector<std::pair<void*, size_t>> buffers;
  std::thread thread;
  std::atomic<bool> stopping{false};
};

V4L2Camera::V4L2Camera(const CameraInfo& info)
{
  if(start(info.id))
    thread = std::thread(&V4L2Camera::captureLoop, this);
}

V4L2Camera::~V4L2Camera()
{
  stopping = true;
  if(thread.joinable())
    thread.join();
  if(fd >= 0) {
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(fd, VIDIOC_STREAMOFF, &type);
  }
  for(auto& buffer : buffers)
    munmap(buffer.first, buffer.second);
  if(fd >= 0)
    ::close(fd);
}

// The largest frame wins, since a scan wants every pixel it can get.  MJPEG is preferred at equal size:
//  a webcam's YUYV mode is bandwidth limited, typically to a few fps at its full resolution.
bool V4L2Camera::chooseFormat()
{
  unsigned int bestFormat = 0;
  long bestArea = 0;
  int bestWidth = 0, bestHeight = 0;
  for(unsigned int format : {V4L2_PIX_FMT_MJPEG, V4L2_PIX_FMT_YUYV}) {
    v4l2_frmsizeenum size = {};
    size.pixel_format = format;
    for(size.index = 0; xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) == 0; ++size.index) {
      int frameWidth, frameHeight;
      if(size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
        frameWidth = size.discrete.width;
        frameHeight = size.discrete.height;
      }
      else {
        frameWidth = size.stepwise.max_width;
        frameHeight = size.stepwise.max_height;
      }
      if(long(frameWidth)*frameHeight > bestArea) {
        bestArea = long(frameWidth)*frameHeight;
        bestFormat = format;
        bestWidth = frameWidth;
        bestHeight = frameHeight;
      }
      if(size.type != V4L2_FRMSIZE_TYPE_DISCRETE)
        break;
    }
  }
  if(!bestFormat)
    return false;

  v4l2_format fmt = {};
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = bestWidth;
  fmt.fmt.pix.height = bestHeight;
  fmt.fmt.pix.pixelformat = bestFormat;
  fmt.fmt.pix.field = V4L2_FIELD_ANY;
  if(xioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
    return false;
  // the driver may adjust any of these, so read them back rather than trusting the request
  pixelFormat = fmt.fmt.pix.pixelformat;
  width = fmt.fmt.pix.width;
  height = fmt.fmt.pix.height;
  stride = fmt.fmt.pix.bytesperline ? fmt.fmt.pix.bytesperline : width*2;
  return pixelFormat == V4L2_PIX_FMT_MJPEG || pixelFormat == V4L2_PIX_FMT_YUYV;
}

bool V4L2Camera::start(const std::string& path)
{
  fd = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if(fd < 0) {
    setError(errno == EBUSY ? "The camera is in use by another application" : "The camera could not be opened");
    return false;
  }
  if(!isVideoCapture(fd, NULL) || !chooseFormat()) {
    setError("The camera does not offer a supported video format");
    return false;
  }

  v4l2_requestbuffers req = {};
  req.count = 4;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  if(xioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
    setError(errno == EBUSY ? "The camera is in use by another application" : "The camera could not be started");
    return false;
  }
  for(unsigned int ii = 0; ii < req.count; ++ii) {
    v4l2_buffer buf = {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = ii;
    if(xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0)
      break;
    void* mem = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
    if(mem == MAP_FAILED)
      break;
    buffers.emplace_back(mem, buf.length);
    if(xioctl(fd, VIDIOC_QBUF, &buf) < 0)
      break;
  }
  v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if(buffers.size() < 2 || xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
    setError("The camera could not be started");
    return false;
  }
  return true;
}

void V4L2Camera::convertFrame(const unsigned char* data, size_t len)
{
  if(pixelFormat == V4L2_PIX_FMT_MJPEG) {
    publishFrame(decodeMjpegFrame(data, len));
    return;
  }
  if(len < size_t(stride)*height)
    return;  // a short frame from a hiccup on the bus
  Image frame(width, height, Image::JPEG);
  yuyvToRGBA(data, width, height, stride, frame.bytes());
  publishFrame(std::move(frame));
}

void V4L2Camera::captureLoop()
{
  while(!stopping) {
    // a timeout rather than blocking, so that closing the dialog never waits on a camera that stalled
    pollfd pfd = {fd, POLLIN, 0};
    int ready = poll(&pfd, 1, 100);
    if(ready < 0 && errno != EINTR) {
      setError("The camera stopped responding");
      return;
    }
    if(ready <= 0)
      continue;
    v4l2_buffer buf = {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if(xioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
      if(errno == EAGAIN)
        continue;
      setError("The camera was disconnected");
      return;
    }
    if(!(buf.flags & V4L2_BUF_FLAG_ERROR) && buf.index < buffers.size())
      convertFrame((const unsigned char*)buffers[buf.index].first, buf.bytesused);
    xioctl(fd, VIDIOC_QBUF, &buf);
  }
}

}  // namespace

std::unique_ptr<Camera> Camera::open(const CameraInfo& info)
{
  return std::unique_ptr<Camera>(new V4L2Camera(info));
}
