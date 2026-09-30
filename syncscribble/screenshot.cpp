#include "screenshot.h"

#include "ulib/painter.h"
#include "usvg/svgpainter.h"

#include "basics.h"
#include "page.h"
#include "element.h"
#include "scribbleapp.h"
#include "mainwindow.h"

// Page units are 150 per inch, so 2 is ~300 dpi - enough that a printed-size exercise stays sharp when
//  zoomed, and above the dpi PDF pages are imported at, so the capture never loses what the page had.
static constexpr Dim SHOT_SCALE = 2;
static constexpr int SHOT_MAX_DIM = 4000;
static constexpr real HANDLE_RADIUS = 11;
static constexpr real TOUCH_RADIUS = 30;
// the image is inset so a corner handle sits wholly inside the widget - one on the widget's edge is half
//  outside it, and a press on that half never reaches the handler
static constexpr real IMAGE_INSET = 24;
static constexpr int MIN_CROP_PX = 8;
static const Color OUTLINE_COLOR = Color(0xFF00C0FF);
static const Color OUTSIDE_SHADE = Color(0x99000000);
static const Color DIALOG_BACKDROP = Color(0xFF303030);
// dialog layout: top/bottom padding (PopupDialog's is for a text title, far too much above a button row),
//  gap between the header row and the preview, and between Copy and Add to page
static constexpr real HEADER_PADDING = 8;
static constexpr real PREVIEW_GAP = 12;
static constexpr real BUTTON_SPACING = 8;

Dim screenshotScale(const Rect& region)
{
  Dim longest = std::max(region.width(), region.height());
  return longest > 0 ? std::min(SHOT_SCALE, SHOT_MAX_DIM/longest) : SHOT_SCALE;
}

void whiteToAlpha(unsigned char* rgba, int w, int h)
{
  size_t count = size_t(w)*h;
  for(size_t i = 0; i < count; ++i, rgba += 4) {
    // composite over white first, so a translucent pixel is judged by how it actually looked
    int alpha = rgba[3];
    int composite[3];
    for(int channel = 0; channel < 3; ++channel)
      composite[channel] = (rgba[channel]*alpha + 255*(255 - alpha) + 127)/255;
    int lowest = std::min(composite[0], std::min(composite[1], composite[2]));
    int outAlpha = 255 - lowest;
    if(outAlpha == 0) {
      rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
      continue;
    }
    // over white, (c - lowest)*255/outAlpha at outAlpha reproduces c exactly
    for(int channel = 0; channel < 3; ++channel)
      rgba[channel] = (unsigned char)(((composite[channel] - lowest)*255 + outAlpha/2)/outAlpha);
    rgba[3] = (unsigned char)outAlpha;
  }
}

Image renderPageRegion(Page* page, const Rect& region, Dim scale, int layers)
{
  int width = std::max(1, int(std::ceil(region.width()*scale)));
  int height = std::max(1, int(std::ceil(region.height()*scale)));
  Image image(width, height, Image::PNG);
  if(!page->ensureLoaded())
    return image;
  {
    // a fresh painter has no night mode map, so this is the document's own colors whatever the view shows
    Painter painter(Painter::PAINT_SW | Painter::SRGB_AWARE, &image);
    painter.setBackgroundColor(Color::WHITE);
    painter.beginFrame();
    painter.setsRGBAdjAlpha(true);
    painter.scale(scale, scale);
    painter.translate(-region.left, -region.top);
    bool forceNormal = Element::FORCE_NORMAL_DRAW;
    Element::FORCE_NORMAL_DRAW = true;  // selected ink is drawn as ink, not as a selection
    SvgPainter svgPainter(&painter);
    // The rule layer is paper, ruling lines and - for an imported PDF or a scan - the page image.  The
    //  paper is never drawn: it belongs to the page copied *from*, and leaving it out is what lets the
    //  capture sit on whatever paper it goes to.  Lines and image are the user's choice.
    if(page->ruleNode) {
      for(SvgNode* child : page->ruleNode->children()) {
        bool image = child->type() == SvgNode::IMAGE;
        if(image ? (layers & SHOT_BACKGROUND) : (layers & SHOT_RULING) && !child->hasClass("pagerect"))
          svgPainter.drawNode(child, region);
      }
    }
    if(page->contentNode && (layers & SHOT_INK))
      svgPainter.drawNode(page->contentNode, region);
    Element::FORCE_NORMAL_DRAW = forceNormal;
    painter.endFrame();
  }
  whiteToAlpha(image.bytes(), image.width, image.height);
  return image;
}

class ScreenshotCropWidget : public CustomWidget
{
public:
  explicit ScreenshotCropWidget(const Image* shot);
  void draw(SvgPainter* svgp) const override;

  Rect crop;  // image pixels

private:
  real imageScale() const;
  Point imageOrigin() const;
  Point imageToWidget(const Point& p) const { return imageOrigin() + p*imageScale(); }
  Point widgetToImage(const Point& p) const { return (p - imageOrigin())/imageScale(); }
  Point corner(int index) const;
  int hitTestCorner(const Point& local) const;
  void moveCorner(int index, Point to);

  const Image* image;
  int dragIndex = -1;
};

ScreenshotCropWidget::ScreenshotCropWidget(const Image* shot) : image(shot)
{
  crop = Rect::wh(image->width, image->height);
  onPrepareLayout = [this](){ return Rect::wh(400, 300); };

  addHandler([this](SvgGui* gui, SDL_Event* event) -> bool {
    Point local = Point(event->tfinger.x, event->tfinger.y) - node->bounds().origin();
    if(event->type == SDL_FINGERDOWN && event->tfinger.fingerId == SDL_BUTTON_LMASK) {
      int hit = hitTestCorner(local);
      if(hit < 0)
        return false;
      dragIndex = hit;
      gui->setPressed(this);
      node->invalidate(true);
      return true;
    }
    if(event->type == SDL_FINGERMOTION && gui->pressedWidget == this && dragIndex >= 0) {
      moveCorner(dragIndex, widgetToImage(local));
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

real ScreenshotCropWidget::imageScale() const
{
  if(image->width <= 0 || image->height <= 0)
    return 1;
  // fit, but never blow a small capture up past 2x - a few words scaled to fill the dialog look broken
  real width = std::max(real(1), mBounds.width() - 2*IMAGE_INSET);
  real height = std::max(real(1), mBounds.height() - 2*IMAGE_INSET);
  return std::min(real(2), std::min(width/image->width, height/image->height));
}

Point ScreenshotCropWidget::imageOrigin() const
{
  real scale = imageScale();
  return Point((mBounds.width() - image->width*scale)/2, (mBounds.height() - image->height*scale)/2);
}

// 0 top-left, 1 top-right, 2 bottom-right, 3 bottom-left
Point ScreenshotCropWidget::corner(int index) const
{
  return Point(index == 1 || index == 2 ? crop.right : crop.left, index >= 2 ? crop.bottom : crop.top);
}

int ScreenshotCropWidget::hitTestCorner(const Point& local) const
{
  int best = -1;
  real bestDist = TOUCH_RADIUS;
  for(int ii = 0; ii < 4; ++ii) {
    real dist = imageToWidget(corner(ii)).dist(local);
    if(dist < bestDist) {
      bestDist = dist;
      best = ii;
    }
  }
  return best;
}

// Each corner owns one vertical and one horizontal edge; it may not cross the opposite ones, so the
//  crop cannot flip inside out or shrink below a few pixels.  No aspect ratio is kept, by design.
void ScreenshotCropWidget::moveCorner(int index, Point to)
{
  real x = std::min(real(image->width), std::max(real(0), to.x));
  real y = std::min(real(image->height), std::max(real(0), to.y));
  if(index == 0 || index == 3)
    crop.left = std::min(x, crop.right - MIN_CROP_PX);
  else
    crop.right = std::max(x, crop.left + MIN_CROP_PX);
  if(index <= 1)
    crop.top = std::min(y, crop.bottom - MIN_CROP_PX);
  else
    crop.bottom = std::max(y, crop.top + MIN_CROP_PX);
}

void ScreenshotCropWidget::draw(SvgPainter* svgp) const
{
  Painter* painter = svgp->p;
  real scale = imageScale();
  Point origin = imageOrigin();
  Rect imageRect = Rect::ltwh(origin.x, origin.y, image->width*scale, image->height*scale);
  painter->save();
  painter->fillRect(mBounds, DIALOG_BACKDROP);
  // the capture is transparent where the paper was, so show it on white as it will look on light paper
  painter->fillRect(imageRect, Color::WHITE);
  painter->drawImage(imageRect, *image);

  Rect cropRect = Rect::corners(imageToWidget(corner(0)), imageToWidget(corner(2)));
  // shade what the crop leaves out
  painter->fillRect(Rect::ltrb(imageRect.left, imageRect.top, imageRect.right, cropRect.top), OUTSIDE_SHADE);
  painter->fillRect(Rect::ltrb(imageRect.left, cropRect.bottom, imageRect.right, imageRect.bottom),
      OUTSIDE_SHADE);
  painter->fillRect(Rect::ltrb(imageRect.left, cropRect.top, cropRect.left, cropRect.bottom), OUTSIDE_SHADE);
  painter->fillRect(Rect::ltrb(cropRect.right, cropRect.top, imageRect.right, cropRect.bottom), OUTSIDE_SHADE);

  painter->setFillBrush(Color::NONE);
  painter->setStrokeBrush(OUTLINE_COLOR);
  painter->setStrokeWidth(2);
  painter->drawPath(Path2D().addRect(cropRect));
  for(int ii = 0; ii < 4; ++ii) {
    Point handlePos = imageToWidget(corner(ii));
    Path2D handle;
    handle.addEllipse(handlePos.x, handlePos.y, HANDLE_RADIUS, HANDLE_RADIUS);
    painter->setFillBrush(ii == dragIndex ? Color(0xFFFFFFFF) : Color(0xC0FFFFFF));
    painter->setStrokeBrush(OUTLINE_COLOR);
    painter->drawPath(handle);
  }
  painter->restore();
}

ScreenshotDialog::ScreenshotDialog(RenderFn render) : PopupDialog(createPopupDialogNode()),
    renderFn(render), source(render(SHOT_DEFAULT))
{
  cropWidget = new ScreenshotCropWidget(&source);
  cropWidget->node->setAttribute("box-anchor", "fill");

  // close and title on the left, the two choices on the right, in place of the usual bottom button row
  selectFirst(".dialogpopup-title")->setVisible(false);
  // hidden, not just empty: PopupDialog's BUTTON_GAP margin would still leave a strip at the bottom
  selectFirst(".dialog-buttons")->setVisible(false);
  // the header sits right at the top edge, so trim the popup's vertical padding (sides stay as they are)
  selectFirst(".dialog-layout")->setMargins(HEADER_PADDING, PopupDialog::PADDING);
  Button* closeBtn = createToolbutton(SvgGui::useFile("icons/ic_menu_cancel.svg"), _("Cancel"));
  closeBtn->onClicked = [this](){ finish(CANCELLED); };
  closeBtn->setMargins(0, 4, 0, 0);  // the title's own margin is fixed by CSS
  Widget* title = new Widget(createTextNode(_("Screenshot")));
  title->node->addClass("window-title dialogpopup-title");
  Button* copyBtn = createPushbutton(_("Copy"));
  copyBtn->onClicked = [this](){ choice = CHOICE_COPY; finish(ACCEPTED); };
  Button* addBtn = createPushbutton(_("Add to page"));
  addBtn->onClicked = [this](){ choice = CHOICE_ADD; finish(ACCEPTED); };
  acceptBtn = addBtn;  // Enter adds; Esc cancels
  addBtn->setMargins(0, 0, 0, BUTTON_SPACING);
  // #pushbutton is box-anchor="fill", which in a row makes both buttons share the free width with the
  //  stretch; vfill keeps them at their text's width
  for(Button* btn : {copyBtn, addBtn})
    btn->node->setAttribute("box-anchor", "vfill");
  Widget* header = createRow({closeBtn, title, createStretch(), copyBtn, addBtn});
  header->setMargins(0, 0, PREVIEW_GAP, 0);

  Widget* dialogBody = selectFirst(".body-container");
  dialogBody->node->setAttribute("box-anchor", "fill");
  dialogBody->setMargins(0);  // the TITLE_GAP is for the hidden stock title
  Widget* column = createColumn();
  column->node->setAttribute("box-anchor", "fill");
  cbRuling = createCheckBox(_("Ruling"), false);
  cbBackground = createCheckBox(_("PDF / image background"), true);
  cbInk = createCheckBox(_("Annotations"), true);
  for(CheckBox* cb : {cbRuling, cbBackground, cbInk}) {
    cb->onToggled = [this](bool){ rerender(); };
    cb->setMargins(0, 10);
  }
  Widget* optionsRow = createRow({cbRuling, cbBackground, cbInk});
  optionsRow->setMargins(6, 0);

  column->addWidget(header);
  column->addWidget(cropWidget);
  column->addWidget(optionsRow);
  dialogBody->addWidget(column);

  Rect parentBounds = ScribbleApp::win->winBounds();
  setWinBounds(Rect::centerwh(parentBounds.center(),
      std::min(parentBounds.width() - 40, Dim(820)), std::min(parentBounds.height() - 40, Dim(680))));
}

Rect ScreenshotDialog::cropRect() const
{
  const Rect& crop = cropWidget->crop;
  return Rect::ltrb(std::floor(crop.left), std::floor(crop.top), std::ceil(crop.right), std::ceil(crop.bottom));
}

Image ScreenshotDialog::takeCropped() const
{
  Image out = source.cropped(cropRect());
  out.encoding = Image::PNG;  // keeps the transparency
  return out;
}

void ScreenshotDialog::rerender()
{
  int layers = (cbRuling->isChecked() ? SHOT_RULING : 0) | (cbBackground->isChecked() ? SHOT_BACKGROUND : 0)
      | (cbInk->isChecked() ? SHOT_INK : 0);
  // same region and scale, so the same size: the crop, which is in pixels, stays where it was
  source = renderFn(layers);
  cropWidget->node->invalidate(true);
}
