#include "scandialog.h"

#include "ulib/imagewarp.h"
#include "ulib/painter.h"
#include "ulib/quaddetect.h"
#include "ulib/threadutil.h"
#include "usvg/svgpainter.h"

#include "basics.h"
#include "scribbleapp.h"
#include "mainwindow.h"
#include "tagdoclist.h"

#include <algorithm>
#include <memory>

static const int PREVIEW_MAX_DIM = 700;    // enough to judge a filter, small enough to feel instant
static const int OUTPUT_MAX_DIM = 2200;    // about 200 dpi across a sheet of A4
static const real HANDLE_RADIUS = 11;
// a dragged corner moves this fraction of the finger's travel, so it can be placed more finely than a
//  finger can point, and it stays visible beside the finger instead of underneath it
static const real DRAG_RATIO = 0.4;
static const real LOUPE_SIZE = 150;
static const real LOUPE_ZOOM = 3;
static const Color OUTLINE_COLOR = Color(0xFF00C0FF);

// The photo with the page outline drawn on it and a draggable handle at each corner.
class ScanCornerWidget : public CustomWidget
{
public:
  explicit ScanCornerWidget(const Image* photo);
  void draw(SvgPainter* svgp) const override;

  Point quad[4];

private:
  real photoScale() const;
  Point photoOrigin() const;
  Point photoToWidget(const Point& p) const { return photoOrigin() + p*photoScale(); }
  int nearestCorner(const Point& local) const;
  void drawLoupe(Painter* painter) const;

  const Image* image;
  int dragIndex = -1;
  Point dragStartLocal;    // where the finger went down, in widget coordinates
  Point dragStartCorner;   // where the dragged corner was then, in photo coordinates
};

// the page outline found in a new photo, or an inset rectangle when there is none to find
static void initialQuad(const Image& photo, Point quad[4])
{
  if(!detectDocumentQuad(photo.constPixels(), photo.width, photo.height, quad))
    defaultDocumentQuad(photo.width, photo.height, 0.1, quad);
}

// photo is the dialog's own source image, which is replaced (not reallocated) for each photo; the dialog
//  sets quad whenever it does that
ScanCornerWidget::ScanCornerWidget(const Image* photo) : image(photo)
{
  onPrepareLayout = [this](){ return Rect::wh(400, 400); };

  addHandler([this](SvgGui* gui, SDL_Event* event) -> bool {
    Point local = Point(event->tfinger.x, event->tfinger.y) - node->bounds().origin();
    if(event->type == SDL_FINGERDOWN && event->tfinger.fingerId == SDL_BUTTON_LMASK) {
      // a touch anywhere grabs the nearest corner, so the finger never has to land on the handle
      dragIndex = nearestCorner(local);
      dragStartLocal = local;
      dragStartCorner = quad[dragIndex];
      gui->setPressed(this);
      node->invalidate(true);
      return true;
    }
    if(event->type == SDL_FINGERMOTION && gui->pressedWidget == this && dragIndex >= 0) {
      Point moved = dragStartCorner + (local - dragStartLocal)*(DRAG_RATIO/photoScale());
      moved.x = std::min(real(image->width), std::max(real(0), moved.x));
      moved.y = std::min(real(image->height), std::max(real(0), moved.y));
      Point previous = quad[dragIndex];
      quad[dragIndex] = moved;
      // a drag that would fold the quad over itself has no sensible flattening, so just refuse it
      if(!isConvexQuad(quad))
        quad[dragIndex] = previous;
      node->invalidate(true);
      return true;
    }
    if(event->type == SDL_FINGERUP && dragIndex >= 0) {
      dragIndex = -1;
      node->invalidate(true);
      return true;
    }
    return false;
  });
}

real ScanCornerWidget::photoScale() const
{
  if(image->width <= 0 || image->height <= 0)
    return 1;
  return std::min(mBounds.width()/image->width, mBounds.height()/image->height);
}

Point ScanCornerWidget::photoOrigin() const
{
  real scale = photoScale();
  return Point((mBounds.width() - image->width*scale)/2, (mBounds.height() - image->height*scale)/2);
}

int ScanCornerWidget::nearestCorner(const Point& local) const
{
  int best = 0;
  real bestDist = photoToWidget(quad[0]).dist(local);
  for(int ii = 1; ii < 4; ++ii) {
    real dist = photoToWidget(quad[ii]).dist(local);
    if(dist < bestDist) {
      bestDist = dist;
      best = ii;
    }
  }
  return best;
}

void ScanCornerWidget::drawLoupe(Painter* painter) const
{
  Point handlePos = photoToWidget(quad[dragIndex]);
  // keep the loupe away from the finger, which is covering the corner being dragged
  bool handleOnRight = handlePos.x > mBounds.width()/2;
  Rect loupeRect = Rect::ltwh(handleOnRight ? 8 : mBounds.width() - LOUPE_SIZE - 8,
      8, LOUPE_SIZE, LOUPE_SIZE);
  painter->save();
  painter->clipRect(loupeRect);
  painter->fillRect(loupeRect, Color::BLACK);
  painter->translate(loupeRect.center());
  painter->scale(LOUPE_ZOOM, LOUPE_ZOOM);
  painter->translate(-quad[dragIndex].x, -quad[dragIndex].y);
  painter->drawImage(Rect::wh(image->width, image->height), *image);
  painter->restore();
  // crosshair at the exact point being placed, and a frame so the loupe reads as an inset
  painter->setFillBrush(Color::NONE);
  painter->setStrokeBrush(OUTLINE_COLOR);
  painter->setStrokeWidth(1.5);
  Path2D crosshair;
  crosshair.addLine(loupeRect.center() - Point(10, 0), loupeRect.center() + Point(10, 0));
  crosshair.addLine(loupeRect.center() - Point(0, 10), loupeRect.center() + Point(0, 10));
  crosshair.addRect(loupeRect);
  painter->drawPath(crosshair);
}

void ScanCornerWidget::draw(SvgPainter* svgp) const
{
  Painter* painter = svgp->p;
  real scale = photoScale();
  Point origin = photoOrigin();
  painter->save();
  painter->fillRect(mBounds, Color::BLACK);
  painter->drawImage(Rect::ltwh(origin.x, origin.y, image->width*scale, image->height*scale), *image);

  Path2D outline;
  outline.moveTo(photoToWidget(quad[0]));
  for(int ii = 1; ii < 4; ++ii)
    outline.lineTo(photoToWidget(quad[ii]));
  outline.closeSubpath();
  painter->setFillBrush(Color::NONE);
  painter->setStrokeBrush(OUTLINE_COLOR);
  painter->setStrokeWidth(2);
  painter->drawPath(outline);

  for(int ii = 0; ii < 4; ++ii) {
    Point handlePos = photoToWidget(quad[ii]);
    Path2D handle;
    handle.addEllipse(handlePos.x, handlePos.y, HANDLE_RADIUS, HANDLE_RADIUS);
    painter->setFillBrush(ii == dragIndex ? Color(0xFFFFFFFF) : Color(0x80FFFFFF));
    painter->setStrokeBrush(OUTLINE_COLOR);
    painter->setStrokeWidth(2);
    painter->drawPath(handle);
  }
  if(dragIndex >= 0)
    drawLoupe(painter);
  painter->restore();
}

// Shows the flattened, filtered result.
class ScanPreviewWidget : public CustomWidget
{
public:
  ScanPreviewWidget() { onPrepareLayout = [this](){ return Rect::wh(400, 400); }; }
  void draw(SvgPainter* svgp) const override;

  Image preview{0, 0};
};

void ScanPreviewWidget::draw(SvgPainter* svgp) const
{
  Painter* painter = svgp->p;
  painter->save();
  painter->fillRect(mBounds, Color(0xFF303030));
  if(!preview.isNull() && preview.width > 0 && preview.height > 0) {
    real scale = std::min(mBounds.width()/preview.width, mBounds.height()/preview.height);
    real drawWidth = preview.width*scale, drawHeight = preview.height*scale;
    Rect dest = Rect::ltwh((mBounds.width() - drawWidth)/2, (mBounds.height() - drawHeight)/2,
        drawWidth, drawHeight);
    painter->drawImage(dest, preview);
  }
  painter->restore();
}

// The review grid: a thumbnail per pending page, picked the way document list cards are in select mode
//  (ring and check badge, docs/agent/ui-floating-bar.md), with the actions in a floating bar.
static const real THUMB_W = 132;
static const real THUMB_H = 176;
static const real CELL_GAP = 16;
static const int THUMB_PIXELS = 2*176;  // twice the slot, so the thumbnail stays sharp on a 2x screen
static const real BADGE_SIZE = 24;
static const real BADGE_INSET = 8;
static const real CHECK_SIZE = 16;
static const real BAR_LABEL_WIDTH = 96;  // fixed, so "3 pages" -> "1 selected" never slides the buttons

const char* ScanDialog::stylesheet = R"#(
.scan-dialog .scan-grid-bg { fill: var(--doclist-bg); }
.scan-dialog .scan-cell, .scan-dialog .scan-thumb-sizer { fill: none; }
.scan-dialog .scan-thumb-outline { fill: none; stroke: var(--floating-outline); stroke-width: 1; }
.scan-dialog .scan-page-num { fill: var(--icon); font-size: 14; }
.scan-dialog .scan-cell.hovered .scan-page-num { fill: var(--hovered-text); }
.scan-dialog .scan-cell.checked .scan-page-num { fill: var(--checked); }
.scan-dialog .select-ring { display: none; fill: none; stroke: var(--checked); stroke-width: 3; }
.scan-dialog .scan-cell.checked .select-ring { display: block; }
.scan-dialog .select-badge-bg { fill: var(--badge-bg); fill-opacity: var(--badge-bg-opacity); stroke: var(--badge-edge); stroke-width: 1.5; }
.scan-dialog .scan-cell.checked .select-badge-bg { fill: var(--checked); fill-opacity: 1; stroke: var(--checked); }
.scan-dialog .select-check { display: none; }
.scan-dialog .scan-cell.checked .select-check { display: block; fill: var(--on-checked); color: var(--on-checked); }
.scan-dialog .selectbar-bg { fill: var(--floating-bg); stroke: var(--floating-outline); stroke-width: 1; }
.scan-dialog .selectbar-count { fill: var(--text); font-size: 15; }
.scan-dialog .selectbar-sizer { fill: none; }
.scan-dialog .selectbar-btn-bg { fill: none; }
.scan-dialog .selectbar-btn.hovered .selectbar-btn-bg { fill: var(--floating-hovered); }
.scan-dialog .selectbar-btn.pressed .selectbar-btn-bg { fill: var(--floating-pressed); }
.scan-dialog .selectbar-btn.disabled .icon { fill: var(--icon-disabled); color: var(--icon-disabled); }
.scan-dialog .selectbar-delete .icon { fill: var(--danger); color: var(--danger); }
.scan-dialog .selectbar-delete.disabled .icon { fill: var(--icon-disabled); color: var(--icon-disabled); }
)#";

// The bottom row is laid out explicitly rather than in the platform's default order: createPopupDialogNode()
//  reverses the button row on mobile, which put Done on the left on the iPad.  Here the way forward (Next,
//  then Done) always sits at the bottom right and Add More at the left, the buttons are only
//  BOTTOM_BUTTON_WIDTH wide rather than splitting the row between them, and the space between them is
//  empty - so on a tablet, a thumb reaching for Done cannot land on Add More.
static const real BOTTOM_BUTTON_WIDTH = 150;

ScanDialog::ScanDialog(Image photo) : PopupDialog(createPopupDialogNode(false)), source(std::move(photo))
{
  node->addClass("scan-dialog");
  setTitle(_("Scan Document"));
  cornerWidget = new ScanCornerWidget(&source);
  cornerWidget->node->setAttribute("box-anchor", "fill");
  initialQuad(source, cornerWidget->quad);
  previewWidget = new ScanPreviewWidget;
  previewWidget->node->setAttribute("box-anchor", "fill");

  // The combo index is used directly as a ScanFilter, so the order of these is load bearing: reordering
  //  the list would silently change what each entry does.  The static_assert catches the enum being
  //  renumbered at compile time; the ASSERT below catches this table being reordered, which can only be
  //  checked at run time (debug builds) since the rows are data rather than types.
  static_assert(SCAN_ORIGINAL == 0 && SCAN_COLOR == 1 && SCAN_GREYSCALE == 2 && SCAN_MONO == 3,
      "ScanFilter values must stay 0..3 and in this order - the scan dialog indexes the combo by them");
  static const struct { ScanFilter filter; const char* name; } FILTERS[] = {
    { SCAN_ORIGINAL,  "Original" },
    { SCAN_COLOR,     "Color document" },
    { SCAN_GREYSCALE, "Greyscale" },
    { SCAN_MONO,      "Black and white" }
  };
  std::vector<std::string> filterNames;
  for(size_t ii = 0; ii < sizeof(FILTERS)/sizeof(FILTERS[0]); ++ii) {
    ASSERT(size_t(FILTERS[ii].filter) == ii && "ScanFilter order must match the combo box order");
    filterNames.push_back(_(FILTERS[ii].name));
  }
  comboFilter = createComboBox(filterNames);
  comboFilter->setIndex(SCAN_COLOR);
  comboFilter->onChanged = [this](const char*){ updatePreview(); };

  rotateBtn = createPushbutton(_("Rotate"));
  rotateBtn->onClicked = [this](){
    quarterTurns = (quarterTurns + 1) % 4;
    updatePreview();
  };

  Widget* filterRow = createRow({comboFilter, rotateBtn});
  filterRow->setMargins(6, 0);

  // the review grid: wrapping rows of cells in a vertical scroller, the floating bar over its corner
  gridView = new Widget(new SvgG());
  gridView->node->setAttribute("box-anchor", "top hfill");  // vertical scrolling only; top, or a short grid floats
  gridView->node->setAttribute("layout", "flex");
  gridView->node->setAttribute("flex-direction", "row");
  gridView->node->setAttribute("flex-wrap", "wrap");
  gridView->node->setAttribute("justify-content", "flex-start");
  // room at the bottom so the last row can scroll clear of the floating bar
  gridView->setMargins(CELL_GAP/2, CELL_GAP/2, 43 + 56 + CELL_GAP, CELL_GAP/2);
  ScrollWidget* gridScroll = new ScrollWidget(new SvgDocument(), gridView);
  gridScroll->node->setAttribute("box-anchor", "fill");

  gridPanel = new Widget(new SvgG());
  gridPanel->node->setAttribute("box-anchor", "fill");
  gridPanel->node->setAttribute("layout", "box");
  SvgRect* gridBg = new SvgRect(Rect::wh(400, 400));
  gridBg->addClass("scan-grid-bg");
  gridBg->setAttribute("box-anchor", "fill");
  gridPanel->containerNode()->addChild(gridBg);
  gridPanel->addWidget(gridScroll);

  // the floating bar, exactly as ui-floating-bar.md has it: count label, Edit Corners, Discard
  Widget* gridBar = new Widget(new SvgG());
  gridBar->node->addClass("selectbar");
  gridBar->node->setAttribute("box-anchor", "bottom right");
  gridBar->node->setAttribute("layout", "box");
  gridBar->setMargins(0, 48, 43, 0);
  SvgRect* gridBarBg = new SvgRect(Rect::wh(20, 20), 14, 14);
  gridBarBg->addClass("selectbar-bg");
  gridBarBg->setAttribute("box-anchor", "fill");
  gridBar->containerNode()->addChild(gridBarBg);
  Widget* gridBarRow = new Widget(new SvgG());
  gridBarRow->node->setAttribute("layout", "flex");
  gridBarRow->node->setAttribute("flex-direction", "row");
  gridBarRow->node->setAttribute("align-items", "center");
  gridBarRow->setMargins(6);
  // the label sits on a fixed-width sizer (box layout, left aligned) so its text can change freely
  Widget* countBox = new Widget(new SvgG());
  countBox->node->setAttribute("layout", "box");
  countBox->setMargins(0, 14, 0, 12);
  SvgRect* countSizer = new SvgRect(Rect::wh(BAR_LABEL_WIDTH, 1));
  countSizer->addClass("selectbar-sizer");
  countBox->containerNode()->addChild(countSizer);
  gridCountText = createTextNode("");
  gridCountText->addClass("selectbar-count");
  gridCountText->setAttribute("box-anchor", "left");
  countBox->containerNode()->addChild(gridCountText);
  gridBarRow->addWidget(countBox);
  editCornersBtn = TagDocList::createSelectBarButton("icons/ic_menu_editbox.svg", _("Edit Corners"));
  editCornersBtn->onClicked = [this](){
    for(size_t ii = 0; ii < cellSelected.size(); ++ii) {
      if(cellSelected[ii]) {
        editPending(int(ii));
        return;
      }
    }
  };
  gridBarRow->addWidget(editCornersBtn);
  discardBtn = TagDocList::createSelectBarButton("icons/ic_menu_discard.svg", _("Discard"));
  discardBtn->node->addClass("selectbar-delete");
  discardBtn->onClicked = [this](){ discardSelected(); };
  gridBarRow->addWidget(discardBtn);
  gridBar->addWidget(gridBarRow);
  gridPanel->addWidget(gridBar);

  Widget* dialogBody = selectFirst(".body-container");
  dialogBody->node->setAttribute("box-anchor", "fill");
  Widget* column = createColumn();
  column->node->setAttribute("box-anchor", "fill");
  column->addWidget(cornerWidget);
  column->addWidget(previewWidget);
  column->addWidget(gridPanel);
  column->addWidget(filterRow);
  dialogBody->addWidget(column);

  // Back and Cancel are navigation rather than outcomes, so they sit in a header around the title -
  //  back arrow to its left, cross at the far right - and the bottom row is left with the ways forward.
  //  The template's title text is moved into that header rather than recreated, so it keeps its styling.
  backBtn = createToolbutton(SvgGui::useFile("icons/ic_menu_back.svg"), _("Back"));
  backBtn->onClicked = [this](){
    if(step == FILTER) {
      showStep(CORNERS);
      return;
    }
    // from the corners of a new or reopened page back to the grid: the photo is dropped, a reopened
    //  page keeps what it had
    source = Image(0, 0);
    editIndex = -1;
    showStep(GRID);
  };
  // cancelBtn is also what Dialog runs for Escape / Android back; it drops every pending page
  cancelBtn = createToolbutton(SvgGui::useFile("icons/ic_menu_cancel.svg"), _("Cancel"));
  cancelBtn->onClicked = [this](){ finish(CANCELLED); };
  SvgNode* titleNode = selectFirst(".window-title")->node;
  SvgContainerNode* layoutNode = selectFirst(".dialog-layout")->containerNode();
  layoutNode->removeChild(titleNode);
  Widget* header = createRow({backBtn});
  header->containerNode()->addChild(titleNode);
  header->addWidget(createStretch());
  header->addWidget(cancelBtn);
  layoutNode->addChild(header->node, dialogBody->node);

  addMoreBtn = addButton(_("Add More"), [this](){ addMore(); });
  selectFirst(".dialog-buttons")->addWidget(createStretch());
  nextBtn = addButton(_("Next"), [this](){ showStep(FILTER); });
  acceptBtn = addButton(_("Done"), [this](){
    if(step == FILTER)
      finishCurrent();
    else if(!pending.empty())
      finish(ACCEPTED);
  });
  for(Button* button : {addMoreBtn, nextBtn, acceptBtn}) {
    button->node->setAttribute("box-anchor", "vfill");  // not "fill", which would share out the row
    // an invisible rect sets the width: the background rect is hfill, so its own width does not count
    SvgRect* widthRect = new SvgRect(Rect::wh(BOTTOM_BUTTON_WIDTH, 1));
    widthRect->setAttribute("fill", "none");
    button->containerNode()->addChild(widthRect, button->containerNode()->firstChild());
  }

  Rect parentBounds = ScribbleApp::win->winBounds();
  setWinBounds(Rect::centerwh(parentBounds.center(),
      std::min(parentBounds.width() - 40, Dim(720)), std::min(parentBounds.height() - 40, Dim(820))));
  showStep(CORNERS);
}

void ScanDialog::addPhoto(Image photo)
{
  if(photo.isNull() || photo.width <= 0 || photo.height <= 0)
    return;
  source = std::move(photo);
  initialQuad(source, cornerWidget->quad);
  // the filter carries over from the previous page - a set is nearly always one kind of paper
  quarterTurns = 0;
  editIndex = -1;
  showStep(CORNERS);
}

std::vector<Image> ScanDialog::takeResults()
{
  std::vector<Image> results;
  for(PendingScan& scan : pending)
    results.push_back(std::move(scan.result));
  pending.clear();
  return results;
}

void ScanDialog::showStep(Step newStep)
{
  step = newStep;
  if(step == GRID) {
    gridShown = true;
    rebuildGrid();
  }
  cornerWidget->setVisible(step == CORNERS);
  previewWidget->setVisible(step == FILTER);
  gridPanel->setVisible(step == GRID);
  comboFilter->setVisible(step == FILTER);
  rotateBtn->setVisible(step == FILTER);
  // from the corners, Back returns to the grid when there is one to return to
  backBtn->setVisible(step == FILTER || (step == CORNERS && !pending.empty()));
  nextBtn->setVisible(step == CORNERS);
  addMoreBtn->setVisible(step != CORNERS);
  acceptBtn->setVisible(step != CORNERS);
  acceptBtn->setEnabled(step != GRID || !pending.empty());
  if(step == FILTER)
    updatePreview();
}

void ScanDialog::updatePreview()
{
  previewWidget->preview = renderScan(PREVIEW_MAX_DIM, true);
  previewWidget->node->invalidate(true);
}

// Flattens the page being edited at output size and files it under pending: in place of the page it
//  reopened, or after the others.  The photo is kept JPEG-encoded so the corners can be reopened.
bool ScanDialog::commitCurrent()
{
  PendingScan scan;
  scan.result = renderScan(OUTPUT_MAX_DIM, false);
  if(scan.result.isNull() || scan.result.width <= 0 || scan.result.height <= 0)
    return false;
  std::copy(cornerWidget->quad, cornerWidget->quad + 4, scan.quad);
  scan.filter = comboFilter->index();
  scan.quarterTurns = quarterTurns;
  // a supersampled warp of the whole result is a box-filtered downscale, so text does not alias
  real thumbScale = real(THUMB_PIXELS)/std::max(scan.result.width, scan.result.height);
  int thumbWidth = std::max(1, int(scan.result.width*thumbScale + 0.5));
  int thumbHeight = std::max(1, int(scan.result.height*thumbScale + 0.5));
  scan.thumbnail = Image(thumbWidth, thumbHeight, Image::JPEG);
  Point wholeResult[4] = { Point(0, 0), Point(scan.result.width, 0),
      Point(scan.result.width, scan.result.height), Point(0, scan.result.height) };
  warpQuadRGBA(scan.result.constPixels(), scan.result.width, scan.result.height,
      scan.thumbnail.pixels(), thumbWidth, thumbHeight, wholeResult, NULL);
  bool reopened = editIndex >= 0 && editIndex < int(pending.size());
  // a reopened page's photo is the one already stored; a photo decoded from a JPEG file returns its own
  //  bytes here, so only a camera frame is actually encoded
  scan.photoJpeg = reopened ? std::move(pending[editIndex].photoJpeg) : source.encodeJPEG(92);
  source = Image(0, 0);
  if(reopened)
    pending[editIndex] = std::move(scan);
  else
    pending.push_back(std::move(scan));
  editIndex = -1;
  return true;
}

// Done on the filter step: a lone page is inserted right away, as before there was a grid; once the set
//  has more than one page, or has been seen as a grid, Done goes back to the grid, where the set is
//  confirmed as a whole
void ScanDialog::finishCurrent()
{
  if(!commitCurrent())
    return;
  if(pending.size() == 1 && !gridShown)
    finish(ACCEPTED);
  else
    showStep(GRID);
}

void ScanDialog::addMore()
{
  // from the filter step the page in hand joins the set first; the grid shows while the next photo is
  //  taken, and is what stays if that capture is cancelled
  if(step == FILTER && !commitCurrent())
    return;
  if(step != GRID)
    showStep(GRID);
  if(onAddMore)
    onAddMore();
}

void ScanDialog::editPending(int index)
{
  if(index < 0 || index >= int(pending.size()))
    return;
  const PendingScan& scan = pending[index];
  Image photo = Image::decodeBuffer(scan.photoJpeg.data(), scan.photoJpeg.size());
  if(photo.isNull() || photo.width <= 0 || photo.height <= 0)
    return;
  source = std::move(photo);
  std::copy(scan.quad, scan.quad + 4, cornerWidget->quad);
  comboFilter->setIndex(scan.filter);
  quarterTurns = scan.quarterTurns;
  editIndex = index;
  showStep(CORNERS);
}

void ScanDialog::discardSelected()
{
  std::vector<PendingScan> kept;
  for(size_t ii = 0; ii < pending.size(); ++ii) {
    if(ii >= cellSelected.size() || !cellSelected[ii])
      kept.push_back(std::move(pending[ii]));
  }
  pending.swap(kept);
  showStep(GRID);
}

int ScanDialog::selectedCount() const
{
  return int(std::count(cellSelected.begin(), cellSelected.end(), 1));
}

// Rebuilt whenever the grid is shown, since a page may have been added, redone or discarded; selection
//  starts empty each time
void ScanDialog::rebuildGrid()
{
  ScribbleApp::gui->deleteContents(gridView);
  gridCells.clear();
  cellSelected.assign(pending.size(), 0);
  for(size_t ii = 0; ii < pending.size(); ++ii) {
    // the slot is always the full thumbnail size, so the rows line up whatever shape the pages are; the
    //  page sits at the slot's bottom, on its number, as a page card does in the document list
    std::string svg = fstring(R"(
      <g class="scan-cell" layout="flex" flex-direction="column" align-items="center">
        <g class="scan-thumb-slot" layout="box">
          <rect class="scan-thumb-sizer" width="%.2f" height="%.2f"/>
          <g class="scan-thumb-holder" layout="box" box-anchor="bottom"></g>
        </g>
        <text class="scan-page-num" margin="6 0 0 0"></text>
      </g>
    )", THUMB_W, THUMB_H);
    std::unique_ptr<SvgNode> proto(loadSVGFragment(svg.c_str()));
    Button* cell = new Button(proto->clone());
    cell->setMargins(CELL_GAP/2);
    static_cast<SvgText*>(cell->containerNode()->selectFirst(".scan-page-num"))
        ->addText(fstring("%d", int(ii) + 1).c_str());

    const Image& thumbnail = pending[ii].thumbnail;
    real fit = std::min(THUMB_W/thumbnail.width, THUMB_H/thumbnail.height);
    Rect face = Rect::wh(thumbnail.width*fit, thumbnail.height*fit);
    SvgContainerNode* holderNode = cell->selectFirst(".scan-thumb-holder")->containerNode();
    holderNode->addChild(new SvgImage(thumbnail.copy(), face));
    SvgRect* outline = new SvgRect(face);
    outline->addClass("scan-thumb-outline");
    holderNode->addChild(outline);
    // the marks hug the page rather than the slot
    SvgRect* ring = new SvgRect(Rect::wh(20, 20), 6, 6);
    ring->addClass("select-ring");
    ring->setAttribute("box-anchor", "fill");
    holderNode->addChild(ring);
    SvgG* badge = new SvgG();
    badge->addClass("select-badge");
    badge->setAttribute("box-anchor", "top right");
    badge->setAttribute("layout", "box");
    badge->setAttribute("margin", fstring("%g %g 0 0", BADGE_INSET, BADGE_INSET).c_str());
    SvgRect* badgeBg = new SvgRect(Rect::wh(BADGE_SIZE, BADGE_SIZE), BADGE_SIZE/2, BADGE_SIZE/2);
    badgeBg->addClass("select-badge-bg");
    badge->addChild(badgeBg);
    SvgUse* check = new SvgUse(Rect::wh(CHECK_SIZE, CHECK_SIZE), "", SvgGui::useFile("icons/ic_menu_accept.svg"));
    check->addClass("icon");
    check->addClass("select-check");
    badge->addChild(check);
    holderNode->addChild(badge);

    cell->onClicked = [this, ii](){
      cellSelected[ii] = !cellSelected[ii];
      gridCells[ii]->setChecked(cellSelected[ii]);
      updateGridBar();
    };
    gridCells.push_back(cell);
    gridView->addWidget(cell);
  }
  updateGridBar();
}

// buttons that would do nothing are disabled rather than hidden, so the bar never changes shape
void ScanDialog::updateGridBar()
{
  int nselected = selectedCount();
  std::string label = nselected > 0 ? fstring(_("%d selected"), nselected)
      : pending.size() == 1 ? std::string(_("1 page")) : fstring(_("%d pages"), int(pending.size()));
  gridCountText->clearText();
  gridCountText->addText(label.c_str());
  editCornersBtn->setEnabled(nselected == 1);
  discardBtn->setEnabled(nselected > 0);
  acceptBtn->setEnabled(step != GRID || !pending.empty());
}

// fast skips supersampling and the thread pool: at preview size the difference is invisible, and it keeps
//  changing a filter feeling immediate
Image ScanDialog::renderScan(int maxDimension, bool fast)
{
  if(source.isNull() || source.width <= 0 || source.height <= 0)
    return Image(0, 0);
  int dstw = 0, dsth = 0;
  quadOutputSize(cornerWidget->quad, maxDimension, &dstw, &dsth);
  if(dstw <= 0 || dsth <= 0)
    return Image(0, 0);
  Image flattened(dstw, dsth, Image::JPEG);
  if(fast) {
    Point dstQuad[4] = { Point(0, 0), Point(dstw, 0), Point(dstw, dsth), Point(0, dsth) };
    warpPerspectiveRGBA(source.constPixels(), source.width, source.height,
        flattened.pixels(), dstw, dsth, Transform3D::quadToQuad(dstQuad, cornerWidget->quad), 1, NULL);
  }
  else {
    ThreadPool pool(0);
    warpQuadRGBA(source.constPixels(), source.width, source.height,
        flattened.pixels(), dstw, dsth, cornerWidget->quad, &pool);
  }
  enhanceDocument(flattened.pixels(), dstw, dsth, ScanFilter(comboFilter->index()));

  int turns = quarterTurns % 4;
  if(turns == 0)
    return flattened;
  int rotatedWidth = (turns % 2) ? dsth : dstw;
  int rotatedHeight = (turns % 2) ? dstw : dsth;
  Image rotated(rotatedWidth, rotatedHeight, flattened.encoding);
  rotateQuarterTurns(flattened.constPixels(), dstw, dsth, rotated.pixels(), turns);
  return rotated;
}
