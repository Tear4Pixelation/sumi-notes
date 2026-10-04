#import <AVFoundation/AVFoundation.h>
#include "camera_avf.h"

static NSArray<AVCaptureDevice*>* videoDevices(void)
{
  NSMutableArray<AVCaptureDeviceType>* types = [NSMutableArray arrayWithObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 140000
  if(@available(macOS 14.0, *)) {
    [types addObject:AVCaptureDeviceTypeExternal];
    [types addObject:AVCaptureDeviceTypeContinuityCamera];
  }
  else
#endif
  {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [types addObject:AVCaptureDeviceTypeExternalUnknown];
#pragma clang diagnostic pop
  }
  AVCaptureDeviceDiscoverySession* discovery = [AVCaptureDeviceDiscoverySession
      discoverySessionWithDeviceTypes:types mediaType:AVMediaTypeVideo position:AVCaptureDevicePositionUnspecified];
  return discovery.devices;
}

void macCameraList(MacCameraAddFn add, void* user)
{
  @autoreleasepool {
    for(AVCaptureDevice* device in videoDevices())
      add(user, device.uniqueID.UTF8String, device.localizedName.UTF8String);
  }
}

@interface MacCamera : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@end

@implementation MacCamera
{
@public
  AVCaptureSession* session;
  dispatch_queue_t queue;
  MacCameraFrameFn onFrame;
  MacCameraErrorFn onError;
  void* user;
  BOOL closed;  // only touched on queue
  id errorObserver;
}

- (void)reportError:(const char*)msg
{
  dispatch_async(queue, ^{
    if(!self->closed)
      self->onError(self->user, msg);
  });
}

// The largest format wins, as on the other platforms: a scan wants every pixel it can get, and the
//  session presets top out at the video sizes rather than the sensor's.
static void chooseLargestFormat(AVCaptureDevice* device)
{
  AVCaptureDeviceFormat* best = nil;
  int64_t bestArea = 0;
  for(AVCaptureDeviceFormat* format in device.formats) {
    CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(format.formatDescription);
    if((int64_t)dims.width*dims.height > bestArea) {
      bestArea = (int64_t)dims.width*dims.height;
      best = format;
    }
  }
  if(best && [device lockForConfiguration:nil]) {
    device.activeFormat = best;
    [device unlockForConfiguration];
  }
}

- (void)startWithDevice:(AVCaptureDevice*)device
{
  if(closed)
    return;  // permission was granted after the dialog had already closed
  NSError* error = nil;
  AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
  if(!input) {
    [self reportError:"The camera could not be opened"];
    return;
  }
  session = [[AVCaptureSession alloc] init];
  [session beginConfiguration];
  if(![session canAddInput:input]) {
    [session commitConfiguration];
    [self reportError:"The camera is in use by another application"];
    return;
  }
  [session addInput:input];
  AVCaptureVideoDataOutput* output = [[AVCaptureVideoDataOutput alloc] init];
  output.videoSettings = @{ (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA) };
  output.alwaysDiscardsLateVideoFrames = YES;
  [output setSampleBufferDelegate:self queue:queue];
  if([session canAddOutput:output])
    [session addOutput:output];
  // after addInput, since adding the input applies the session preset over the device's format
  chooseLargestFormat(device);
  [session commitConfiguration];

  __weak MacCamera* weakSelf = self;
  errorObserver = [[NSNotificationCenter defaultCenter] addObserverForName:AVCaptureSessionRuntimeErrorNotification
      object:session queue:nil usingBlock:^(NSNotification* note) {
        [weakSelf reportError:"The camera stopped responding"];
      }];
  // startRunning blocks until the camera is up, so keep it off the main thread
  dispatch_async(queue, ^{
    if(!self->closed)
      [self->session startRunning];
  });
}

- (void)captureOutput:(AVCaptureOutput*)output didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
    fromConnection:(AVCaptureConnection*)connection
{
  if(closed)
    return;
  CVImageBufferRef pixels = CMSampleBufferGetImageBuffer(sampleBuffer);
  if(!pixels || CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess)
    return;
  onFrame(user, (const unsigned char*)CVPixelBufferGetBaseAddress(pixels), (int)CVPixelBufferGetWidth(pixels),
      (int)CVPixelBufferGetHeight(pixels), (int)CVPixelBufferGetBytesPerRow(pixels));
  CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
}

@end

static NSMutableSet* openCameras = nil;  // ARC owns the objects; this keeps them alive between open and close

void* macCameraOpen(const char* id, MacCameraFrameFn onFrame, MacCameraErrorFn onError, void* user)
{
  @autoreleasepool {
    MacCamera* camera = [[MacCamera alloc] init];
    camera->queue = dispatch_queue_create("sumi.camera", DISPATCH_QUEUE_SERIAL);
    camera->onFrame = onFrame;
    camera->onError = onError;
    camera->user = user;
    if(!openCameras)
      openCameras = [NSMutableSet set];
    [openCameras addObject:camera];

    AVCaptureDevice* device = [AVCaptureDevice deviceWithUniqueID:@(id)];
    if(!device) {
      [camera reportError:"The camera was disconnected"];
      return (__bridge void*)camera;
    }
    // without NSCameraUsageDescription in Info.plist, asking for access kills the app outright
    switch([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]) {
    case AVAuthorizationStatusAuthorized:
      [camera startWithDevice:device];
      break;
    case AVAuthorizationStatusNotDetermined:
    {
      __weak MacCamera* weakCamera = camera;
      [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL granted) {
        dispatch_async(dispatch_get_main_queue(), ^{
          MacCamera* strongCamera = weakCamera;
          if(!strongCamera)
            return;
          if(granted)
            [strongCamera startWithDevice:device];
          else
            [strongCamera reportError:"Camera access was not allowed"];
        });
      }];
      break;
    }
    default:
      [camera reportError:"Camera access is turned off for Sumi in System Settings, Privacy & Security"];
      break;
    }
    return (__bridge void*)camera;
  }
}

void macCameraClose(void* handle)
{
  @autoreleasepool {
    MacCamera* camera = (__bridge MacCamera*)handle;
    // closed is set on the queue, so once this returns no frame or error callback is still running or
    //  can start - the C++ object they point at is about to go away
    dispatch_sync(camera->queue, ^{ camera->closed = YES; });
    if(camera->errorObserver)
      [[NSNotificationCenter defaultCenter] removeObserver:camera->errorObserver];
    [camera->session stopRunning];
    [openCameras removeObject:camera];
  }
}
