#include "screenshot.h"

#include "ulib/painter.h"
#include "usvg/svgpainter.h"

#include "basics.h"
#include "page.h"
#include "element.h"
#include "scribbleapp.h"
#include "mainwindow.h"
#include "tagdoclist.h"
#include "ulib/stringutil.h"

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
// a press this close to a polygon edge puts a point into it rather than adding one at the end
static constexpr real EDGE_TOUCH_RADIUS = 14;
// what the crop cuts away is shaded with the backdrop color at this opacity; a handle at rest is translucent
static constexpr float OUTSIDE_SHADE_ALPHA = 0.7f;
static constexpr float HANDLE_REST_ALPHA = 0.75f;
// the longest straight piece of a rounded corner, in image pixels, for the cropped result
static constexpr real OUTLINE_SEG_PX = 0.5;
// dialog layout: top/bottom padding (PopupDialog's is for a text title, far too much above a button row),
//  gap between the header row and the preview, and between Copy and Add to page
static constexpr real HEADER_PADDING = 8;
static constexpr real PREVIEW_GAP = 12;
static constexpr real BUTTON_SPACING = 8;
// shape bar: gap above it, the fixed width of its hint (the longest wording, with room for a translation)
//  and of the radius slider
static constexpr real BAR_GAP = 10;
static constexpr real HINT_WIDTH = 190;
static constexpr real RADIUS_SLIDER_WIDTH = 120;

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
  if(!((layers & SHOT_BACKGROUND) && (layers & SHOT_KEEP_WHITE)))
    whiteToAlpha(image.bytes(), image.width, image.height);
  return image;
}


// Original pixels (screenshot.md): an edited image carries the shape and its original, so the next edit
//  starts from the whole picture.  Underscored like __layer: attributes of ours on standard SVG elements.
const char* IMAGE_CROP_ATTR = "__crop";
const char* IMAGE_ORIG_ATTR = "__croporig";

Image cropToShape(const Image& src, const CropShape& shape, Rect* pixelBounds)
{
  std::vector<Point> outline = roundedOutline(shape.corners(), shape.radius, OUTLINE_SEG_PX);
  Rect bounds = cropPixelBounds(outline, src.width, src.height);
  if(pixelBounds)
    *pixelBounds = bounds;
  if(bounds.width() < 1 || bounds.height() < 1)
    return Image(0, 0);
  Image out = src.cropped(bounds);
  if(out.width <= 0 || out.height <= 0)
    return out;
  if(shape.kind == CropShape::RECT && shape.radius <= 0) {
    out.encoding = src.encoding == Image::JPEG ? Image::JPEG : Image::PNG;
    return out;
  }
  for(Point& pt : outline)
    pt = pt - bounds.origin();
  applyOutlineMask(out.bytes(), out.width, out.height, outline);
  out.invalidate();
  out.encoding = Image::PNG;  // the transparency outside the shape
  return out;
}

void setImageOriginal(SvgImage* node, const Image& original, const CropShape& shape, const char* origDataUri)
{
  if(shape.isWholeImage(original.width, original.height)) {
    node->removeAttr(IMAGE_CROP_ATTR);
    node->removeAttr(IMAGE_ORIG_ATTR);
    return;
  }
  node->setAttribute(IMAGE_CROP_ATTR, shape.serialize().c_str());
  if(origDataUri) {
    node->setAttribute(IMAGE_ORIG_ATTR, origDataUri);
    return;
  }
  // the bytes the original was loaded from, as they were (a photo's JPEG is not re-encoded); else encode it
  Image::EncodeBuff bytes = !original.encData.empty() ? original.encData
      : original.encode(original.encoding == Image::JPEG && !original.hasTransparency() ? Image::JPEG : Image::PNG);
  bool jpeg = bytes.size() > 2 && bytes[0] == 0xFF && bytes[1] == 0xD8;
  std::string uri = std::string("data:image/") + (jpeg ? "jpeg" : "png") + ";base64,"
      + base64_encode(bytes.data(), bytes.size());
  node->setAttribute(IMAGE_ORIG_ATTR, uri.c_str());
}

bool getImageOriginal(const SvgImage* node, Image* original, CropShape* shape)
{
  const char* cropStr = node->getStringAttr(IMAGE_CROP_ATTR);
  const char* origStr = node->getStringAttr(IMAGE_ORIG_ATTR);
  if(!cropStr || !origStr || strncmp(origStr, "data:image/", 11) != 0)
    return false;
  CropShape parsed;
  const char* comma = strchr(origStr, ',');
  if(!comma || !CropShape::parse(cropStr, &parsed))
    return false;
  std::vector<unsigned char> bytes = base64_decode(comma + 1, strlen(comma + 1));
  Image decoded = Image::decodeBuffer(bytes.data(), bytes.size());
  if(decoded.width <= 0 || decoded.height <= 0)
    return false;
  *original = std::move(decoded);
  *shape = parsed;
  return true;
}

class ImageCropWidget : public CustomWidget
{
public:
  ImageCropWidget(const Image* img, const CropShape& initial);
  void draw(SvgPainter* svgp) const override;

  CropShape shape() const;
  CropShape::Kind kind() const { return currKind; }
  void setKind(CropShape::Kind kind);
  int pointCount() const { return int(polyPoints.size()); }
  void removeLastPoint();
  // the radius at which the shape's shorter side is all corner - the slider's full scale
  real maxRadius() const;
  void setRadius(real r) { radius = r; node->invalidate(true); }
  real getRadius() const { return radius; }

  std::function<void()> onChanged;

private:
  real imageScale() const;
  Point imageOrigin() const;
  Point imageToWidget(const Point& p) const { return imageOrigin() + p*imageScale(); }
  Point widgetToImage(const Point& p) const { return (p - imageOrigin())/imageScale(); }
  Point clampToImage(Point p) const;
  std::vector<Point> handles() const;
  int hitTestHandle(const Point& local) const;
  int hitTestEdge(const Point& local) const;
  void moveCorner(int index, Point to);
  Color themeColor(const char* attr) const;

  const Image* image;
  CropShape::Kind currKind = CropShape::RECT;
  Rect crop;  // image pixels
  std::vector<Point> polyPoints;
  real radius = 0;
  int dragIndex = -1;
};

ImageCropWidget::ImageCropWidget(const Image* img, const CropShape& initial) : image(img)
{
  node->addClass("imagecrop-view");  // its colors: theme variables via the stylesheet below
  Rect whole = Rect::wh(image->width, image->height);
  currKind = initial.kind;
  radius = initial.radius;
  polyPoints = initial.points;
  crop = initial.kind == CropShape::RECT ? Rect(initial.rect).rectIntersect(whole) : whole;
  if(initial.kind == CropShape::POLYGON && !polyPoints.empty()) {
    // switching to the rectangle starts from the polygon's bounds
    Rect bbox;
    for(const Point& pt : polyPoints)
      bbox.rectUnion(pt);
    if(bbox.width() >= MIN_CROP_PX && bbox.height() >= MIN_CROP_PX)
      crop = bbox.rectIntersect(whole);
  }
  if(!crop.isValid() || crop.width() < 1 || crop.height() < 1)
    crop = whole;
  onPrepareLayout = [this](){ return Rect::wh(400, 300); };

  addHandler([this](SvgGui* gui, SDL_Event* event) -> bool {
    Point local = Point(event->tfinger.x, event->tfinger.y) - node->bounds().origin();
    if(event->type == SDL_FINGERDOWN && event->tfinger.fingerId == SDL_BUTTON_LMASK) {
      int hit = hitTestHandle(local);
      if(hit < 0 && currKind == CropShape::POLYGON) {
        // a press on an edge puts a point into it; anywhere else adds the next point - and either is then
        //  dragged, so a point can be placed precisely by pressing and sliding
        Point pos = clampToImage(widgetToImage(local));
        int edge = hitTestEdge(local);
        hit = edge >= 0 ? edge + 1 : int(polyPoints.size());
        polyPoints.insert(polyPoints.begin() + hit, pos);
      }
      if(hit < 0)
        return false;
      dragIndex = hit;
      gui->setPressed(this);
      node->invalidate(true);
      if(onChanged)
        onChanged();
      return true;
    }
    if(event->type == SDL_FINGERMOTION && gui->pressedWidget == this && dragIndex >= 0) {
      if(currKind == CropShape::POLYGON) {
        if(dragIndex < int(polyPoints.size()))
          polyPoints[dragIndex] = clampToImage(widgetToImage(local));
      }
      else
        moveCorner(dragIndex, widgetToImage(local));
      node->invalidate(true);
      return true;
    }
    if(event->type == SDL_FINGERUP && dragIndex >= 0) {
      dragIndex = -1;
      node->invalidate(true);
      if(onChanged)
        onChanged();
      return true;
    }
    return false;
  });
}

CropShape ImageCropWidget::shape() const
{
  CropShape out;
  out.kind = currKind;
  out.rect = crop;
  out.points = polyPoints;
  out.radius = std::max(real(0), radius);
  return out;
}

void ImageCropWidget::setKind(CropShape::Kind kind)
{
  currKind = kind;
  dragIndex = -1;
  node->invalidate(true);
  if(onChanged)
    onChanged();
}

void ImageCropWidget::removeLastPoint()
{
  if(!polyPoints.empty())
    polyPoints.pop_back();
  node->invalidate(true);
  if(onChanged)
    onChanged();
}

real ImageCropWidget::maxRadius() const
{
  std::vector<Point> corners = shape().corners();
  if(corners.size() < 3)
    return std::min(image->width, image->height)/real(2);
  Rect bbox;
  for(const Point& pt : corners)
    bbox.rectUnion(pt);
  return std::max(real(1), std::min(bbox.width(), bbox.height())/2);
}

real ImageCropWidget::imageScale() const
{
  if(image->width <= 0 || image->height <= 0)
    return 1;
  // fit, but never blow a small capture up past 2x - a few words scaled to fill the dialog look broken
  real width = std::max(real(1), mBounds.width() - 2*IMAGE_INSET);
  real height = std::max(real(1), mBounds.height() - 2*IMAGE_INSET);
  return std::min(real(2), std::min(width/image->width, height/image->height));
}

Point ImageCropWidget::imageOrigin() const
{
  real scale = imageScale();
  return Point((mBounds.width() - image->width*scale)/2, (mBounds.height() - image->height*scale)/2);
}

Point ImageCropWidget::clampToImage(Point p) const
{
  return Point(std::min(real(image->width), std::max(real(0), p.x)),
      std::min(real(image->height), std::max(real(0), p.y)));
}

// rectangle: its corners, 0 top-left, 1 top-right, 2 bottom-right, 3 bottom-left; polygon: its points
std::vector<Point> ImageCropWidget::handles() const
{
  return currKind == CropShape::POLYGON ? polyPoints : CropShape{CropShape::RECT, crop}.corners();
}

int ImageCropWidget::hitTestHandle(const Point& local) const
{
  std::vector<Point> pts = handles();
  int best = -1;
  real bestDist = TOUCH_RADIUS;
  for(int ii = 0; ii < int(pts.size()); ++ii) {
    real dist = imageToWidget(pts[ii]).dist(local);
    if(dist < bestDist) {
      bestDist = dist;
      best = ii;
    }
  }
  return best;
}

// the polygon edge (index of its first point) a press lands on, or -1; the closing edge counts once the
//  polygon is closed, i.e. has three points
int ImageCropWidget::hitTestEdge(const Point& local) const
{
  int count = int(polyPoints.size());
  int edges = count >= 3 ? count : count - 1;
  int best = -1;
  real bestDist = EDGE_TOUCH_RADIUS;
  for(int ii = 0; ii < edges; ++ii) {
    real dist = distToSegment(imageToWidget(polyPoints[ii]), imageToWidget(polyPoints[(ii + 1) % count]), local);
    if(dist < bestDist) {
      bestDist = dist;
      best = ii;
    }
  }
  return best;
}

// Each corner owns one vertical and one horizontal edge; it may not cross the opposite ones, so the
//  crop cannot flip inside out or shrink below a few pixels.  No aspect ratio is kept, by design.
void ImageCropWidget::moveCorner(int index, Point to)
{
  Point clamped = clampToImage(to);
  if(index == 0 || index == 3)
    crop.left = std::min(clamped.x, crop.right - MIN_CROP_PX);
  else
    crop.right = std::max(clamped.x, crop.left + MIN_CROP_PX);
  if(index <= 1)
    crop.top = std::min(clamped.y, crop.bottom - MIN_CROP_PX);
  else
    crop.bottom = std::max(clamped.y, crop.top + MIN_CROP_PX);
}

// fill: the backdrop (and, translucent, the shade over what is cut away); stroke: the outline and handle
//  rims; color: the handles - all theme variables, set by imageCropStylesheet on .imagecrop-view
Color ImageCropWidget::themeColor(const char* attr) const
{
  Color color = node->getColorAttr(attr, Color::INVALID_COLOR);
  return color.color != Color::INVALID_COLOR ? color : Color(Color::BLACK);
}

void ImageCropWidget::draw(SvgPainter* svgp) const
{
  Painter* painter = svgp->p;
  Color backdrop = themeColor("fill");
  Color accent = themeColor("stroke");
  Color handleFill = themeColor("color");
  real scale = imageScale();
  Point origin = imageOrigin();
  Rect imageRect = Rect::ltwh(origin.x, origin.y, image->width*scale, image->height*scale);
  painter->save();
  painter->fillRect(mBounds, backdrop);
  // Paper, not UI: the image is transparent where its paper was (or outside a shape), and it is shown on
  //  white as it will look on light paper.
  painter->fillRect(imageRect, Color::WHITE);
  painter->drawImage(imageRect, *image);

  std::vector<Point> corners = handles();
  bool closed = currKind == CropShape::RECT || corners.size() >= 3;
  Color shade = Color(backdrop).setAlphaF(OUTSIDE_SHADE_ALPHA);
  painter->setStrokeBrush(Color::NONE);
  if(closed) {
    // shade what the shape leaves out: the image with the outline as a hole (even-odd)
    std::vector<Point> outline = roundedOutline(corners, radius, std::max(real(0.25), 1/scale));
    Path2D shadePath;
    shadePath.addRect(imageRect);
    Path2D outlinePath;
    for(size_t ii = 0; ii < outline.size(); ++ii) {
      Point pt = imageToWidget(outline[ii]);
      if(ii == 0) {
        shadePath.moveTo(pt);
        outlinePath.moveTo(pt);
      }
      else {
        shadePath.lineTo(pt);
        outlinePath.lineTo(pt);
      }
    }
    shadePath.closeSubpath();
    outlinePath.closeSubpath();
    shadePath.setFillRule(Path2D::EvenOddFill);
    painter->setFillBrush(shade);
    painter->drawPath(shadePath);
    painter->setFillBrush(Color::NONE);
    painter->setStrokeBrush(accent);
    painter->setStrokeWidth(2);
    painter->drawPath(outlinePath);
  }
  else {
    // still placing points: everything is cut away until they enclose something
    painter->fillRect(imageRect, shade);
    if(corners.size() == 2) {
      painter->setFillBrush(Color::NONE);
      painter->setStrokeBrush(accent);
      painter->setStrokeWidth(2);
      painter->drawPath(Path2D().addLine(imageToWidget(corners[0]), imageToWidget(corners[1])));
    }
  }

  painter->setStrokeBrush(accent);
  painter->setStrokeWidth(2);
  for(int ii = 0; ii < int(corners.size()); ++ii) {
    Point handlePos = imageToWidget(corners[ii]);
    Path2D handle;
    handle.addEllipse(handlePos.x, handlePos.y, HANDLE_RADIUS, HANDLE_RADIUS);
    painter->setFillBrush(ii == dragIndex ? handleFill : Color(handleFill).setAlphaF(HANDLE_REST_ALPHA));
    painter->drawPath(handle);
  }
  painter->restore();
}

ImageCropDialog::ImageCropDialog(RenderFn render, bool hasBackground, const Rect& initialCrop)
    : PopupDialog(createPopupDialogNode()), renderFn(render), source(render(SHOT_DEFAULT))
{
  CropShape initial;
  Rect whole = Rect::wh(source.width, source.height);
  initial.rect = initialCrop.isValid() ? Rect(initialCrop).rectIntersect(whole) : whole;
  if(!initial.rect.isValid() || initial.rect.width() < MIN_CROP_PX || initial.rect.height() < MIN_CROP_PX)
    initial.rect = whole;
  build(true, hasBackground, initial);
}

ImageCropDialog::ImageCropDialog(Image original, const CropShape& initial)
    : PopupDialog(createPopupDialogNode()), source(std::move(original))
{
  CropShape shape = initial;
  if(!shape.isValid())
    shape = CropShape{CropShape::RECT, Rect::wh(source.width, source.height)};
  build(false, false, shape);
}

void ImageCropDialog::build(bool screenshot, bool hasBackground, const CropShape& initial)
{
  node->addClass("imagecrop-dialog");
  cropWidget = new ImageCropWidget(&source, initial);
  cropWidget->node->setAttribute("box-anchor", "fill");

  // close and title on the left, the choices on the right, in place of the usual bottom button row
  selectFirst(".dialogpopup-title")->setVisible(false);
  // hidden, not just empty: PopupDialog's BUTTON_GAP margin would still leave a strip at the bottom
  selectFirst(".dialog-buttons")->setVisible(false);
  // the header sits right at the top edge, so trim the popup's vertical padding (sides stay as they are)
  selectFirst(".dialog-layout")->setMargins(HEADER_PADDING, PopupDialog::PADDING);
  Button* closeBtn = createToolbutton(SvgGui::useFile("icons/ic_menu_cancel.svg"), _("Cancel"));
  closeBtn->onClicked = [this](){ finish(CANCELLED); };
  closeBtn->setMargins(0, 4, 0, 0);  // the title's own margin is fixed by CSS
  Widget* title = new Widget(createTextNode(screenshot ? _("Screenshot") : _("Edit image")));
  title->node->addClass("window-title dialogpopup-title");
  Widget* header = createRow({closeBtn, title, createStretch()});
  if(screenshot) {
    Button* copyBtn = createPushbutton(_("Copy"));
    copyBtn->onClicked = [this](){ choice = CHOICE_COPY; finish(ACCEPTED); };
    Button* addBtn = createPushbutton(_("Add to page"));
    addBtn->onClicked = [this](){ choice = CHOICE_ADD; finish(ACCEPTED); };
    acceptBtn = addBtn;  // Enter adds; Esc cancels
    addBtn->setMargins(0, 0, 0, BUTTON_SPACING);
    acceptButtons = {copyBtn, addBtn};
  }
  else {
    Button* applyBtn = createPushbutton(_("Apply"));
    applyBtn->onClicked = [this](){ choice = CHOICE_APPLY; finish(ACCEPTED); };
    acceptBtn = applyBtn;
    acceptButtons = {applyBtn};
  }
  // #pushbutton is box-anchor="fill", which in a row makes the buttons share the free width with the
  //  stretch; vfill keeps them at their text's width
  for(Button* btn : acceptButtons) {
    btn->node->setAttribute("box-anchor", "vfill");
    header->addWidget(btn);
  }
  header->setMargins(0, 0, PREVIEW_GAP, 0);

  Widget* dialogBody = selectFirst(".body-container");
  dialogBody->node->setAttribute("box-anchor", "fill");
  dialogBody->setMargins(0);  // the TITLE_GAP is for the hidden stock title
  Widget* column = createColumn();
  column->node->setAttribute("box-anchor", "fill");
  column->addWidget(header);
  column->addWidget(cropWidget);
  Widget* barRow = createRow({createStretch(), createShapeBar(), createStretch()});
  barRow->setMargins(BAR_GAP, 0, 0, 0);
  column->addWidget(barRow);

  if(screenshot) {
    cbRuling = createCheckBox(_("Ruling"), false);
    cbBackground = createCheckBox(_("PDF / image background"), true);
    cbInk = createCheckBox(_("Annotations"), true);
    cbKeepWhite = createCheckBox(_("Keep white"), false);
    for(CheckBox* cb : {cbRuling, cbBackground, cbInk, cbKeepWhite}) {
      cb->onToggled = [this](bool){ rerender(); };
      cb->setMargins(0, 10);
    }
    Widget* optionsRow = createRow({cbRuling, cbBackground, cbInk});
    if(hasBackground)
      optionsRow->addWidget(cbKeepWhite);
    optionsRow->setMargins(6, 0);
    column->addWidget(optionsRow);
  }
  dialogBody->addWidget(column);

  cropWidget->onChanged = [this](){ shapeChanged(); };
  radiusSlider->setValue(std::min(real(1), cropWidget->getRadius()/cropWidget->maxRadius()));
  shapeChanged();

  Rect parentBounds = ScribbleApp::win->winBounds();
  setWinBounds(Rect::centerwh(parentBounds.center(),
      std::min(parentBounds.width() - 40, Dim(820)), std::min(parentBounds.height() - 40, Dim(720))));
}

// The shape bar, built as the reference floating bar (ui-floating-bar.md): a fixed-width hint that carries
//  the state, the two shapes, the corner radius, and Remove last point - disabled, not hidden, when there
//  is none, so the bar never changes width.
Widget* ImageCropDialog::createShapeBar()
{
  Widget* bar = new Widget(new SvgG());
  bar->node->addClass("imagecrop-bar");
  bar->node->setAttribute("layout", "box");
  SvgRect* barBg = new SvgRect(Rect::wh(20, 20), 14, 14);
  barBg->addClass("selectbar-bg");
  barBg->setAttribute("box-anchor", "fill");
  bar->containerNode()->addChild(barBg);
  Widget* barRow = new Widget(new SvgG());
  barRow->node->setAttribute("layout", "flex");
  barRow->node->setAttribute("flex-direction", "row");
  barRow->node->setAttribute("align-items", "center");
  barRow->setMargins(6);

  Widget* hintBox = new Widget(new SvgG());
  hintBox->node->setAttribute("layout", "box");
  SvgRect* hintSizer = new SvgRect(Rect::wh(HINT_WIDTH, 1));
  hintSizer->setAttribute("fill", "none");
  hintBox->containerNode()->addChild(hintSizer);
  hintText = createTextNode("");
  hintText->addClass("selectbar-count");
  hintText->setAttribute("box-anchor", "left");
  hintBox->containerNode()->addChild(hintText);
  hintBox->setMargins(0, 14, 0, 12);
  barRow->addWidget(hintBox);

  rectBtn = TagDocList::createSelectBarButton("icons/ic_menu_shape_box.svg", _("Rectangle"));
  rectBtn->onClicked = [this](){ cropWidget->setKind(CropShape::RECT); };
  polyBtn = TagDocList::createSelectBarButton("icons/ic_menu_shape_polyline.svg", _("Polygon: tap to place points"));
  polyBtn->onClicked = [this](){ cropWidget->setKind(CropShape::POLYGON); };
  barRow->addWidget(rectBtn);
  barRow->addWidget(polyBtn);
  auto addSeparator = [&](){
    Widget* separator = new Widget(new SvgRect(Rect::wh(1, 28)));
    separator->node->addClass("selectbar-sep");
    separator->setMargins(0, 6);
    barRow->addWidget(separator);
  };
  addSeparator();

  SvgUse* roundIcon = new SvgUse(Rect::wh(20, 20), "", SvgGui::useFile("icons/ic_menu_shape_rounded.svg"));
  roundIcon->addClass("icon");
  Widget* roundIconWidget = new Widget(roundIcon);
  roundIconWidget->setMargins(0, 6, 0, 6);
  barRow->addWidget(roundIconWidget);
  // the standard slider, on the floating bar's colors (imagecrop stylesheet below)
  radiusSlider = createSlider();
  radiusSlider->onValueChanged = [this](real pos){ cropWidget->setRadius(pos*cropWidget->maxRadius()); };
  setupTooltip(radiusSlider, _("Corner radius"));
  Widget* sliderBox = new Widget(new SvgG());
  sliderBox->node->setAttribute("layout", "box");
  SvgRect* sliderSizer = new SvgRect(Rect::wh(RADIUS_SLIDER_WIDTH, 1));
  sliderSizer->setAttribute("fill", "none");
  sliderBox->containerNode()->addChild(sliderSizer);
  sliderBox->addWidget(radiusSlider);
  sliderBox->setMargins(0, 6, 0, 0);
  barRow->addWidget(sliderBox);
  addSeparator();

  removePointBtn = TagDocList::createSelectBarButton("icons/ic_menu_undo.svg", _("Remove last point"));
  removePointBtn->onClicked = [this](){ cropWidget->removeLastPoint(); };
  barRow->addWidget(removePointBtn);
  bar->addWidget(barRow);
  return bar;
}

void ImageCropDialog::shapeChanged()
{
  bool poly = cropWidget->kind() == CropShape::POLYGON;
  int points = cropWidget->pointCount();
  rectBtn->setChecked(!poly);
  polyBtn->setChecked(poly);
  removePointBtn->setEnabled(poly && points > 0);
  hintText->setText(!poly ? _("Drag the corners") : points < 3 ? _("Tap to place points")
      : _("Drag a point, tap to add"));
  bool valid = cropWidget->shape().isValid();
  for(Button* btn : acceptButtons)
    btn->setEnabled(valid);
}

CropShape ImageCropDialog::cropShape() const
{
  CropShape shape = cropWidget->shape();
  if(shape.kind == CropShape::RECT) {
    // whole pixels, so the result is exactly the pixels under the crop
    const Rect& crop = shape.rect;
    shape.rect = Rect::ltrb(std::floor(crop.left), std::floor(crop.top), std::ceil(crop.right), std::ceil(crop.bottom));
  }
  return shape;
}

void ImageCropDialog::rerender()
{
  if(!renderFn)
    return;
  int layers = (cbRuling->isChecked() ? SHOT_RULING : 0) | (cbBackground->isChecked() ? SHOT_BACKGROUND : 0)
      | (cbInk->isChecked() ? SHOT_INK : 0) | (cbKeepWhite->isChecked() ? SHOT_KEEP_WHITE : 0);
  // same region and scale, so the same size: the crop, which is in pixels, stays where it was
  source = renderFn(layers);
  cropWidget->node->invalidate(true);
}

// Theme variables only (ui-floating-bar.md; feedback: never a literal surface color).  The bar's rules are
//  the select-mode bar's, scoped to .imagecrop-bar, plus the checked state of the two shape buttons.
const char* imageCropStylesheet = R"#(
.imagecrop-view { fill: var(--canvas); stroke: var(--checked); color: var(--on-checked); }
.imagecrop-bar { font-family: satoshi; }
.imagecrop-bar .selectbar-bg { fill: var(--floating-bg); stroke: var(--floating-outline); stroke-width: 1; }
.imagecrop-bar .selectbar-count { fill: var(--text); font-size: 15; }
.imagecrop-bar .selectbar-sep { fill: var(--floating-separator); }
.imagecrop-bar .icon { fill: var(--icon); color: var(--icon); }
.imagecrop-bar .selectbar-btn-bg { fill: none; }
.imagecrop-bar .selectbar-btn.hovered .selectbar-btn-bg { fill: var(--floating-hovered); }
.imagecrop-bar .selectbar-btn.pressed .selectbar-btn-bg { fill: var(--floating-pressed); }
.imagecrop-bar .selectbar-btn.checked .selectbar-btn-bg { fill: var(--floating-pressed); }
.imagecrop-bar .selectbar-btn.checked .icon { fill: var(--checked); color: var(--checked); }
.imagecrop-bar .selectbar-btn.disabled .icon { fill: var(--icon-disabled); color: var(--icon-disabled); }
.imagecrop-bar .slider-bg { fill: var(--floating-separator); stroke: none; }
.imagecrop-bar .slider-caret { fill: var(--text); stroke: none; }
.imagecrop-bar .slider-caret-gap { fill: var(--floating-bg); stroke: none; }
)#";
