#pragma once

#include <memory>
#include "ugui/widgets.h"
#include "camera.h"

// Live camera preview with a capture button - the desktop counterpart of the system camera UI that
//  Android and iOS hand scanning off to.  Only shown when Camera::list() found something; "Choose File"
//  keeps the file picker reachable for a photo taken some other way.

class CameraPreviewWidget;

class CameraDialog : public PopupDialog
{
public:
  enum { CHOOSE_FILE = 2 };  // a result beside ACCEPTED and CANCELLED

  explicit CameraDialog(std::vector<CameraInfo> cameras);
  ~CameraDialog();  // not virtual in ugui; fine, since the dialog only ever lives on the stack

  // the captured frame - only meaningful once the dialog has finished with ACCEPTED
  Image takePhoto();

private:
  void openCamera(int index);
  int pollCamera();

  std::vector<CameraInfo> cameraList;
  std::unique_ptr<Camera> camera;
  CameraPreviewWidget* previewWidget;
  ComboBox* comboCamera = NULL;
};
