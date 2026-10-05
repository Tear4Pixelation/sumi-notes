#include "scandialog.h"

#include "ulib/imagewarp.h"
#include "ulib/painter.h"
#include "ulib/quaddetect.h"
#include "ulib/threadutil.h"
#include "usvg/svgpainter.h"

#include "basics.h"
#include "scribbleapp.h"
#include "mainwindow.h"

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

ScanCornerWidget::ScanCornerWidget(const Image* photo) : image(photo)
{
  if(!detectDocumentQuad(image->constPixels(), image->width, image->height, quad))
    defaultDocumentQuad(image->width, image->height, 0.1, quad);

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

// The bottom row is laid out explicitly rather than in the platform's default order: createPopupDialogNode()
//  reverses the button row on mobile, which put Done on the left on the iPad.  Here the way forward (Next,
//  then Done) always sits at the bottom right and Add More at the left, the buttons are only
//  BOTTOM_BUTTON_WIDTH wide rather than splitting the row between them, and the space between them is
//  empty - so on a tablet, a thumb reaching for Done cannot land on Add More.
static const real BOTTOM_BUTTON_WIDTH = 150;

ScanDialog::ScanDialog(Image photo) : PopupDialog(createPopupDialogNode(false)), source(std::move(photo))
{
  setTitle(_("Scan Document"));
  cornerWidget = new ScanCornerWidget(&source);
  cornerWidget->node->setAttribute("box-anchor", "fill");
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

  Widget* dialogBody = selectFirst(".body-container");
  dialogBody->node->setAttribute("box-anchor", "fill");
  Widget* column = createColumn();
  column->node->setAttribute("box-anchor", "fill");
  column->addWidget(cornerWidget);
  column->addWidget(previewWidget);
  column->addWidget(filterRow);
  dialogBody->addWidget(column);

  // Back and Cancel are navigation rather than outcomes, so they sit in a header around the title -
  //  back arrow to its left, cross at the far right - and the bottom row is left with the ways forward.
  //  The template's title text is moved into that header rather than recreated, so it keeps its styling.
  backBtn = createToolbutton(SvgGui::useFile("icons/ic_menu_back.svg"), _("Back"));
  backBtn->onClicked = [this](){ showStep(0); };
  // cancelBtn is also what Dialog runs for Escape / Android back
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

  addMoreBtn = addButton(_("Add More"), [this](){
    result = renderScan(OUTPUT_MAX_DIM, false);
    finish(ADD_MORE);
  });
  selectFirst(".dialog-buttons")->addWidget(createStretch());
  nextBtn = addButton(_("Next"), [this](){ showStep(1); });
  acceptBtn = addButton(_("Done"), [this](){
    result = renderScan(OUTPUT_MAX_DIM, false);
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
  showStep(0);
}

void ScanDialog::showStep(int newStep)
{
  step = newStep;
  cornerWidget->setVisible(step == 0);
  previewWidget->setVisible(step == 1);
  comboFilter->setVisible(step == 1);
  rotateBtn->setVisible(step == 1);
  backBtn->setVisible(step == 1);
  nextBtn->setVisible(step == 0);
  addMoreBtn->setVisible(step == 1);
  acceptBtn->setVisible(step == 1);
  if(step == 1)
    updatePreview();
}

void ScanDialog::updatePreview()
{
  previewWidget->preview = renderScan(PREVIEW_MAX_DIM, true);
  previewWidget->node->invalidate(true);
}

// fast skips supersampling and the thread pool: at preview size the difference is invisible, and it keeps
//  changing a filter feeling immediate
Image ScanDialog::renderScan(int maxDimension, bool fast)
{
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
