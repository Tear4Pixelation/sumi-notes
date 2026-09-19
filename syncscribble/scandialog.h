#pragma once

#include "ugui/widgets.h"
#include "ulib/image.h"
#include "ulib/imageenhance.h"

// Turning a photo of a page into a scan: adjust the four corners, pick a filter, flatten.
// Two steps rather than one screen, because cropping wants the whole photo as large as possible while
//  choosing a filter wants to show the flattened result - doing both at once makes each of them cramped.
// The dialog only produces an Image; whether that becomes a floating element or a new page is the
//  caller's decision (see ScribbleApp::scanDocument).

class ScanCornerWidget;
class ScanPreviewWidget;

class ScanDialog : public Dialog
{
public:
  explicit ScanDialog(Image photo);

  // the flattened, filtered image - only meaningful once the dialog has finished with ACCEPTED
  Image takeResult() { return std::move(result); }

private:
  void showStep(int newStep);
  void updatePreview();
  Image renderScan(int maxDimension, bool fast);

  Image source;
  Image result{0, 0};
  ScanCornerWidget* cornerWidget;
  ScanPreviewWidget* previewWidget;
  ComboBox* comboFilter;
  Button* rotateBtn;
  Button* backBtn;
  Button* nextBtn;
  int quarterTurns = 0;
  int step = 0;
};
