#pragma once

#include <functional>
#include <vector>
#include "ugui/widgets.h"
#include "ulib/image.h"
#include "ulib/imageenhance.h"

// Turning photos of pages into scans: adjust the four corners, pick a filter, flatten - for as many pages
//  as the user photographs.  Each finished page waits in the dialog (the review grid) until the user
//  confirms the whole set; only then does the caller add them, so a discarded or redone page never
//  reaches the document or its undo history.
// Corners and filter are two steps rather than one screen, because cropping wants the whole photo as
//  large as possible while choosing a filter wants to show the flattened result.
// The dialog only produces Images; whether they become floating elements or new pages is the caller's
//  decision (see ScribbleApp::scanDocument).

class ScanCornerWidget;
class ScanPreviewWidget;

// one page waiting in the dialog, with everything needed to redo it
struct PendingScan
{
  // the photo, kept JPEG-encoded while it waits (a few MB rather than ~48 MB raw for 12 MP) and decoded
  //  again only if its corners are reopened
  Image::EncodeBuff photoJpeg;
  Point quad[4];
  int filter = 0;
  int quarterTurns = 0;
  Image result{0, 0};  // flattened and filtered at output size: what gets inserted
  Image thumbnail{0, 0};
};

class ScanDialog : public PopupDialog
{
public:
  explicit ScanDialog(Image photo);

  // another photo for the set, from Add More's capture; opens its corner step
  void addPhoto(Image photo);
  // Add More: the caller fetches another photo (camera, picker) and hands it to addPhoto() - possibly
  //  later, since the mobile pickers are asynchronous; the dialog stays open meanwhile
  std::function<void()> onAddMore;

  // the pages in order - only meaningful once the dialog has finished with ACCEPTED
  std::vector<Image> takeResults();

  // the review grid's CSS, added to the window stylesheet by createStylesheet() (resources.cpp)
  static const char* stylesheet;

private:
  enum Step { CORNERS, FILTER, GRID };
  void showStep(Step newStep);
  void updatePreview();
  Image renderScan(int maxDimension, bool fast);
  bool commitCurrent();
  void finishCurrent();
  void editPending(int index);
  void discardSelected();
  void addMore();
  void rebuildGrid();
  void updateGridBar();
  int selectedCount() const;

  Image source;  // the photo being edited; null while only the grid is shown
  int editIndex = -1;  // the pending scan being redone, -1 for a new photo
  bool gridShown = false;  // once the set has been seen as a grid, finishing a page returns to it
  std::vector<PendingScan> pending;
  std::vector<Button*> gridCells;
  std::vector<char> cellSelected;

  ScanCornerWidget* cornerWidget;
  ScanPreviewWidget* previewWidget;
  Widget* gridPanel;
  Widget* gridView;
  SvgText* gridCountText;
  Button* editCornersBtn;
  Button* discardBtn;
  ComboBox* comboFilter;
  Button* rotateBtn;
  Button* backBtn;
  Button* nextBtn;
  Button* addMoreBtn;
  int quarterTurns = 0;
  Step step = CORNERS;
};
