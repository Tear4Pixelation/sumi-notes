// canvaslayertest.cpp - included by scribbletest.cpp, like benchtest.cpp
// `Sumi --canvaslayertest` (or tools/perf-bench.sh --mode canvaslayertest --runs 1): the retained canvas
//  layer (ScribbleView::prepareLayer, docs/agent/canvas-layer.md) against direct drawing.  After every step -
//  scrolls of many sizes, diagonal pans, strokes, a stroke in the same frame as a scroll, selection,
//  move, undo, night mode, zoom, dotted paper, a PDF page - the frame on screen is read back and compared
//  with a full direct repaint of the same state (ScribbleView::testDirectPaint, which leaves the layer
//  alone, so stale content left by an earlier step is still caught later).  Prints
//  "BENCH layer_<step>_diff_px" and "_max_diff" per step, the steps over tolerance (layerCompare) as
//  layer_bad_steps, then BENCH_DONE; a bad step writes <step>_layer.png and <step>_direct.png to
//  $TMPDIR/sumi-layertest-<pid> (SUMI_LAYERTEST_SAVEALL=1: every step).

static std::string layerTestDir;

// The frame just painted vs a direct full repaint.  Only device pixels inside the view count: nanovg_sw
//  bleeds the canvas a row past the widget's clip depending on float rounding (direct drawing does it
//  erratically too).  Inside, shifted content is not bit-exact - rasterizing is not invariant under whole
//  pixel translation (GL: a few levels on antialiased edges; SW: up to ~10% on the rows where its
//  thread tiles meet) - so a step is bad when any pixel is off by more than LAYER_MAX_DIFF or more than
//  LAYER_MAX_FRAC of the view differs at all; a stale strip or a missing line fails both.
static constexpr int LAYER_MAX_DIFF = 32;
static constexpr double LAYER_MAX_FRAC = 0.002;

// two frames read back (layered vs reference): prints the per step BENCH lines, saves both as PNG when bad
//  (or SUMI_LAYERTEST_SAVEALL); ndiff gets the number of differing pixels in the view
static bool compareFrames(const std::string& name, const std::vector<unsigned char>& layered,
    const std::vector<unsigned char>& direct, int w, int h, Rect view, int* ndiffOut = NULL,
    const char* refName = "direct", bool quiet = false, int ignoreBelow = 1)
{
  int vx0 = std::max(0, int(std::ceil(view.left))), vy0 = std::max(0, int(std::ceil(view.top)));
  int vx1 = std::min(w, int(std::floor(view.right))), vy1 = std::min(h, int(std::floor(view.bottom)));
  int ndiff = 0, maxdiff = 0, noutside = 0;
  Rect bbox;
  for(int y = 0; y < h; ++y) {
    for(int x = 0; x < w; ++x) {
      const unsigned char* a = &layered[(size_t(y)*w + x)*4];
      const unsigned char* b = &direct[(size_t(y)*w + x)*4];
      int d = 0;
      for(int c = 0; c < 3; ++c)
        d = std::max(d, std::abs(int(a[c]) - int(b[c])));
      if(d < ignoreBelow)
        continue;
      if(x < vx0 || x >= vx1 || y < vy0 || y >= vy1) {
        ++noutside;
        continue;
      }
      ++ndiff;
      maxdiff = std::max(maxdiff, d);
      bbox.rectUnion(Rect::ltwh(x, y, 1, 1));
    }
  }
  if(ndiffOut)
    *ndiffOut = ndiff;
  bool bad = maxdiff > LAYER_MAX_DIFF || ndiff > LAYER_MAX_FRAC*(vx1 - vx0)*(vy1 - vy0);
  if(quiet)
    return bad;
  const char* cname = name.c_str();
  fprintf(stderr, "BENCH layer_%s_diff_px: %d count\n", cname, ndiff);
  fprintf(stderr, "BENCH layer_%s_max_diff: %d level\n", cname, maxdiff);
  if(ndiff > 0 || noutside > 0)
    fprintf(stderr, "  %s%s: %d px differ, max %d, bbox %.0f %.0f %.0f %.0f; %d px outside the view\n", cname,
        bad ? " BAD" : "", ndiff, maxdiff, bbox.left, bbox.top, bbox.right, bbox.bottom, noutside);
  if(bad || getenv("SUMI_LAYERTEST_SAVEALL")) {
    for(int ii = 0; ii < 2; ++ii) {
      Image img(w, h);
      memcpy(img.bytes(), ii ? direct.data() : layered.data(), size_t(w)*h*4);
      for(size_t px = 0; px < size_t(w)*h; ++px)
        img.bytes()[px*4 + 3] = 255;
      auto enc = img.encodePNG();
      std::string fname = layerTestDir + "/" + name + (ii ? std::string("_") + refName + ".png" : "_layer.png");
      FileStream out(fname.c_str(), "wb");
      out.write((const char*)enc.data(), enc.size());
    }
  }
  return bad;
}

int ScribbleTest::layerCompare(const char* name, int ignoreBelow)
{
  std::vector<unsigned char> layered, direct;
  int w = 0, h = 0, w2 = 0, h2 = 0;
  bool ok = Application::readFramebuffer(layered, w, h);
  ScribbleView::testDirectPaint = true;
  SvgPainter::cullSubpaths = false;  // the reference draws every dot of dotted paper (svgpainter.cpp)
  scribbleArea->repaintAll(true);
  benchFrame();
  ok = ok && Application::readFramebuffer(direct, w2, h2) && w == w2 && h == h2;
  ScribbleView::testDirectPaint = false;
  SvgPainter::cullSubpaths = true;
  if(!ok) {
    fprintf(stderr, "BENCH layer_%s_readback_failed: 1 count\n", name);
    return 1;
  }
  Rect view = scribbleArea->widget->node->bounds().scale(Application::gui->paintScale);
  return compareFrames(name, layered, direct, w, h, view, NULL, "direct", false, ignoreBelow) ? 1 : 0;
}

// Selection drag layer (ScribbleArea::selFloat): a big selection dragged by a move is drawn once into the
//  float layer and moved over the canvas layer, which is drawn without it.  Checked here: mid drag, the
//  frame matches a direct repaint (select all, so no other ink for the float to cover - z-order is not kept
//  while it floats); the canvas layer under it holds none of the selected ink (with the float left out of
//  the frame, the selection's areas have no ink pixels); after the drop the frame matches a direct
//  repaint; and the drop leaves the page exactly as the same drag without the layer does, undone by one
//  undo.  Also a fractional zoom, night mode, a partial selection among other ink, a drag partly off the
//  page and one onto the next page.
int ScribbleTest::selDragLayerTest(BenchRng& rng)
{
  int nbad = 0;
  auto pageSvg = [this](int pagenum) {
    MemStream strm;
    scribbleDoc->document->pages[pagenum]->saveSVG(strm);
    return std::string(strm.data(), strm.size());
  };
  auto report = [&nbad](const std::string& name, bool ok) {
    fprintf(stderr, "BENCH layer_%s: %d bool\n", name.c_str(), ok ? 1 : 0);
    if(!ok) {
      fprintf(stderr, "  %s BAD\n", name.c_str());
      ++nbad;
    }
  };
  // repaint the whole view without dirtying content: the layers composited as they are
  auto overlayFrame = [this]() {
    scribbleArea->overlayDirtyScreen = scribbleArea->screenRect;
    scribbleArea->ScribbleView::reqRepaint();
    benchFrame();
  };
  // a direct full repaint, read back; hideSel leaves the selection out, as the canvas layer under it should
  auto directFrame = [this](std::vector<unsigned char>& out, int& w, int& h, bool hideSel) {
    ScribbleView::testDirectPaint = true;
    ScribbleView::testHideFloat = hideSel;
    SvgPainter::cullSubpaths = false;
    scribbleArea->repaintAll(true);
    benchFrame();
    bool ok = Application::readFramebuffer(out, w, h);
    ScribbleView::testDirectPaint = false;
    ScribbleView::testHideFloat = false;
    SvgPainter::cullSubpaths = true;
    return ok;
  };
  // the canvas layer mid drag (the float left out of the frame) against a direct repaint without the
  //  selection, which must differ from one with it (else the check says nothing)
  auto canvasCheck = [&](const std::string& name) {
    std::vector<unsigned char> canvas, hidden, shown;
    int w = 0, h = 0, w2 = 0, h2 = 0, w3 = 0, h3 = 0;
    ScribbleView::testSkipFloat = true;
    overlayFrame();
    bool ok = Application::readFramebuffer(canvas, w, h);
    ScribbleView::testSkipFloat = false;
    ok = directFrame(hidden, w2, h2, true) && ok;
    ok = directFrame(shown, w3, h3, false) && ok && w == w2 && h == h2 && w == w3 && h == h3;
    overlayFrame();
    if(!ok) {
      report(name + "_readback", false);
      return;
    }
    Rect view = scribbleArea->widget->node->bounds().scale(Application::gui->paintScale);
    int selpx = 0;
    if(compareFrames(name + "_canvas", canvas, hidden, w, h, view, NULL, "nosel"))
      ++nbad;
    compareFrames(name + "_sel_visible", shown, hidden, w, h, view, &selpx, "nosel", true);
    fprintf(stderr, "BENCH layer_%s_sel_px: %d count\n", name.c_str(), selpx);
    report(name + "_selection_drawn_normally", selpx > 1000);
  };
  // press on the selection, move in 4 steps, check, release
  //  - exact: compare the frame mid drag with a direct repaint (nothing for the float to cover, and moves
  //  of whole device pixels, so no rounding); canvasChecks: canvasCheck() mid drag
  //  - midStep: run after the second move (a change from elsewhere during the drag)
  auto drag = [&](const std::string& name, Point from, Point to, bool exact, bool canvasChecks,
      std::function<void()> midStep = nullptr) {
    ie(from.x, from.y, 1, pen, press);
    benchFrame();
    bool floated = false;
    for(int ii = 1; ii <= 4; ++ii) {
      Point p = from + (to - from)*(ii/4.0);
      ie(p.x, p.y, 1, pen);
      benchFrame();
      floated = floated || scribbleArea->selFloatActive();
      if(ii == 2 && midStep) {
        midStep();
        benchFrame();
      }
    }
    report(name + "_floats", floated);
    if(floated && canvasChecks)
      canvasCheck(name);
    // the float layer stores partly covered pixels premultiplied in 8 bits: they come back off by a level
    //  or two (GL: ~2000 such pixels for a page of handwriting), so only larger differences count here
    if(floated && exact)
      nbad += layerCompare((name + "_mid").c_str(), 3);
    ie(to.x, to.y, 1, pen, release);
    benchFrame();
    report(name + "_float_ended", !scribbleArea->selFloatActive());
    nbad += layerCompare((name + "_drop").c_str());
  };
  Point mid = scribbleArea->screenRect.center();
  // exact drags move by whole device pixels: 4 steps of a multiple of 4 device px, in screen units
  const Dim ps = Application::gui->paintScale;
  auto devPx = [ps](Dim x, Dim y) { return Point(x/ps, y/ps); };
  // No popups over the view: on SW, the view drawn under a popup bleeds a row into it (nanovg_sw's
  //  inclusive bounds), with or without the layer - the one-time history tip, once there is something to
  //  undo, and the selection toolbar
  if(ScribbleApp::win->historyTip) {
    ScribbleApp::win->historyTip->setVisible(false);
    ScribbleApp::win->historyTipShown = true;
  }
  bool popupToolbar = ScribbleApp::cfg->Bool("popupToolbar");
  ScribbleApp::cfg->set("popupToolbar", false);

  scribbleMode->setMode(MODE_STROKE);
  scribbleDoc->newDocument();
  benchFrame();
  benchHandwriting(rng, 10, 6, 80);
  scribbleArea->gotoPos(0, Point(-10, -10));
  for(int ii = 0; ii < 4; ++ii)
    benchFrame();
  const std::string original = pageSvg(0);
  // two small scrolls after selecting, so the canvas layer is built with the selection in it and the drag
  //  starts from that, kept up to date incrementally (a fresh layer, or the selecting's own redraw still
  //  pending, would hide a missing invalidation)
  auto rebuildLayer = [this]() {
    scribbleArea->scrollBy(0, -4);
    benchFrame();
    scribbleArea->scrollBy(0, 4);
    benchFrame();
  };

  // the same drag with and without the layer: the same page, one undo step
  doCommand(ID_SELALL);
  benchFrame();
  rebuildLayer();
  drag("seldrag", mid, mid + devPx(48, 32), true, true);
  std::string withLayer = pageSvg(0);
  doCommand(ID_CANCEL);
  benchFrame();
  nbad += layerCompare("seldrag_deselect");
  doCommand(ID_UNDO);
  benchFrame();
  // (an undone move can leave rounding noise in the coordinates, with or without the layer)
  const std::string undoneLayer = pageSvg(0);
  nbad += layerCompare("seldrag_undo");
  int minStrokes = ScribbleArea::selLayerMinStrokes;
  ScribbleArea::selLayerMinStrokes = -1;
  doCommand(ID_SELALL);
  benchFrame();
  ie(mid.x, mid.y, 1, pen, press);
  benchFrame();
  for(int ii = 1; ii <= 4; ++ii) {
    Point p = mid + devPx(48, 32)*(ii/4.0);
    ie(p.x, p.y, 1, pen);
    benchFrame();
  }
  ie(mid.x + 48/ps, mid.y + 32/ps, 1, pen, release);
  benchFrame();
  std::string withoutLayer = pageSvg(0);
  doCommand(ID_CANCEL);
  doCommand(ID_UNDO);
  benchFrame();
  ScribbleArea::selLayerMinStrokes = minStrokes;
  report("seldrag_moved", withLayer != original);
  report("seldrag_same_as_direct", withLayer == withoutLayer);
  report("seldrag_undo_same_as_direct", pageSvg(0) == undoneLayer);

  // the page changed from elsewhere mid drag (as sync would): a stroke away from the selection shows at
  //  once; one where the selection is moving is left out of the canvas until the drop (so no exact mid
  //  drag check), then drawn - placed between where the drag starts and ends, so only the page revision
  //  check in endSelFloat() brings it back
  auto addStroke = [this](Point pagePos, Dim size) {
    Path2D path;
    path.moveTo(pagePos.x, pagePos.y);
    path.lineTo(pagePos.x + size, pagePos.y + size/2);
    path.lineTo(pagePos.x + size/3, pagePos.y + size);
    SvgPath* node = new SvgPath(path);
    node->setAttribute("fill", "none");
    node->setAttribute("stroke", "#0000ff");
    node->setAttribute("stroke-width", "3");
    Page* pg = scribbleDoc->document->pages[0];
    scribbleDoc->startAction(0);
    pg->addStroke(new Element(node));
    scribbleDoc->endAction();
    scribbleDoc->doRefresh();
  };
  doCommand(ID_SELALL);
  benchFrame();
  rebuildLayer();
  drag("seldrag_foreign_out", mid, mid + devPx(-32, 24), true, false, [&](){
    Rect sel = scribbleArea->currSelection->getBBox();
    addStroke(Point(sel.right + 60, sel.top + 40), 50);
  });
  doCommand(ID_CANCEL);
  doCommand(ID_UNDO);  // the move
  doCommand(ID_UNDO);  // the added stroke
  benchFrame();
  report("seldrag_foreign_out_undone", pageSvg(0) == undoneLayer);
  ScribbleArea::selLayerMinStrokes = 1;
  scribbleMode->setMode(MODE_SELECTRECT);
  Point wordTopLeft = benchPageToScreen(Point(60, 85));
  benchDrag(wordTopLeft, benchPageToScreen(Point(300, 135)), 6, pen, NULL);
  scribbleMode->setMode(MODE_STROKE);
  if(scribbleArea->currSelection && scribbleArea->currSelection->count() > 0) {
    Point grab = benchPageToScreen(scribbleArea->currSelection->getBBox().center());
    drag("seldrag_foreign_in", grab, grab + devPx(200, 240), false, false, [&](){
      addStroke(scribbleArea->currSelection->getBBox().center() - Point(8, 8), 16);
    });
  }
  else {
    fprintf(stderr, "  rect select from %.1f %.1f: %d selected\n", wordTopLeft.x, wordTopLeft.y,
        scribbleArea->currSelection ? scribbleArea->currSelection->count() : -1);
    report("seldrag_foreign_in_selected", false);
  }
  ScribbleArea::selLayerMinStrokes = minStrokes;
  doCommand(ID_CANCEL);
  doCommand(ID_UNDO);
  doCommand(ID_UNDO);
  benchFrame();
  report("seldrag_foreign_in_undone", pageSvg(0) == undoneLayer);
  nbad += layerCompare("seldrag_foreign_deselect");

  // a fractional zoom, so the offset is rounded to device pixels mid drag (zoomed out: the selection stays
  //  clear of the view's edges, where SW bleeds a column on any repaint)
  scribbleArea->zoomBy(0.83, mid.x, mid.y);
  benchFrame();
  doCommand(ID_SELALL);
  benchFrame();
  drag("seldrag_zoom", mid, mid + Point(29.3, -17.6), false, true);
  doCommand(ID_CANCEL);
  doCommand(ID_UNDO);
  benchFrame();
  doCommand(ID_RESETZOOM);
  benchFrame();

  // night mode
  ScribbleApp::win->toggleInvertColors();
  benchFrame();
  doCommand(ID_SELALL);
  benchFrame();
  drag("seldrag_night", mid, mid + devPx(-36, 24), true, true);
  doCommand(ID_CANCEL);
  doCommand(ID_UNDO);
  benchFrame();
  ScribbleApp::win->toggleInvertColors();
  benchFrame();
  nbad += layerCompare("seldrag_day");

  // part of the ink, among the rest: dropped where other ink is, then moved back over it
  ScribbleArea::selLayerMinStrokes = 8;
  scribbleMode->setMode(MODE_SELECTRECT);
  Point topleft = benchPageToScreen(Point(60, 90));
  benchDrag(topleft, topleft + Point(380, 260), 6, pen, NULL);
  scribbleMode->setMode(MODE_STROKE);
  if(scribbleArea->currSelection) {
    Point grab = benchPageToScreen(scribbleArea->currSelection->getBBox().center());
    drag("seldrag_partial", grab, grab + Point(150, 120), false, false);
    drag("seldrag_partial_back", grab + Point(150, 120), grab + Point(10, 5), false, false);
  }
  else
    report("seldrag_partial_selected", false);
  doCommand(ID_CANCEL);
  benchFrame();
  nbad += layerCompare("seldrag_partial_deselect");
  ScribbleArea::selLayerMinStrokes = minStrokes;

  // partly off the bottom of the page, then onto the next page (the overlay takes over off the page)
  scribbleArea->zoomBy(0.5, mid.x, mid.y);
  benchFrame();
  doCommand(ID_SELALL);
  benchFrame();
  if(scribbleArea->currSelection) {
    Rect pageScr = scribbleArea->dimToScreen(scribbleArea->pageDimToDim(scribbleDoc->document->pages[0]->rect()));
    Rect selScr = scribbleArea->dimToScreen(scribbleArea->pageDimToDim(scribbleArea->currSelection->getBBox()));
    Point grab = selScr.center();
    // not near the bottom of the view, where the drag would start scrolling it
    Dim down = 4*std::floor(ps*std::min(std::min(pageScr.bottom - selScr.bottom + 40, pageScr.bottom - 10 - grab.y),
        scribbleArea->screenRect.bottom - 100 - grab.y)/4);
    drag("seldrag_page_edge", grab, grab + devPx(0, down), true, false);
    if(scribbleArea->currSelection) {
      grab = benchPageToScreen(scribbleArea->currSelection->getBBox().center());
      drag("seldrag_next_page", grab, grab + devPx(0, 4*std::floor(ps*(pageScr.bottom + 120 - grab.y)/4)), true, false);
    }
  }
  doCommand(ID_CANCEL);
  benchFrame();
  nbad += layerCompare("seldrag_next_page_deselect");
  doCommand(ID_RESETZOOM);
  benchFrame();
  ScribbleApp::cfg->set("popupToolbar", popupToolbar);
  return nbad;
}

void ScribbleTest::canvasLayerTest()
{
  const char* tmpdir = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
  layerTestDir = fstring("%s/sumi-layertest-%d", tmpdir, int(getpid()));
  createPath(layerTestDir.c_str());
  benchFullFrame();
  if(Application::glRender)
    SDL_GL_SetSwapInterval(0);
  benchOut("gl_render", Application::glRender ? 1 : 0, "bool");
  ScribbleApp::cfg->set("liftScratchOut", false);
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 1, ScribblePen::TIP_FLAT | ScribblePen::WIDTH_PR, 1.0, 2.0));
  int nbad = 0;
  auto settle = [this](){ for(int ii = 0; ii < 4; ++ii) benchFrame(); };
  auto scroll = [this](int dx, int dy){ scribbleArea->scrollBy(dx, dy); benchFrame(); };
  Point mid = scribbleArea->screenRect.center();

  // ruled paper with handwriting
  scribbleMode->setMode(MODE_STROKE);
  scribbleDoc->newDocument();
  benchFrame();
  BenchRng rng(7);
  benchHandwriting(rng, 14, 6, 80);
  scribbleArea->gotoPos(0, Point(-10, -10));
  settle();
  nbad += layerCompare("initial");
  scribbleMode->setMode(MODE_PAN);
  int steps[] = {1, 7, 37, 333, -50, -1, 600, 2, 450, -900};
  for(int step : steps) {
    scroll(0, -step);
    nbad += layerCompare(fstring("scroll_%d", step).c_str());
  }
  // zoomed in, so the view also pans sideways
  scribbleArea->zoomBy(1.6, mid.x, mid.y);
  benchFrame();
  nbad += layerCompare("zoom_in");
  scroll(23, -17);
  nbad += layerCompare("diag_1");
  scroll(-140, 90);
  nbad += layerCompare("diag_2");
  // a stroke, then a scroll: the strip and the stroke both have to be right
  scribbleMode->setMode(MODE_STROKE);
  benchWord(rng, scribbleArea->dimToPageDim(scribbleArea->screenToDim(mid)), NULL);
  benchFrame();
  nbad += layerCompare("stroke");
  scribbleMode->setMode(MODE_PAN);
  scroll(0, -45);
  nbad += layerCompare("stroke_scroll");
  // a scroll and a stroke in the same frame: the stroke's dirty rect is recorded after the pan
  scribbleArea->scrollBy(0, -30);
  scribbleMode->setMode(MODE_STROKE);
  benchWord(rng, scribbleArea->dimToPageDim(scribbleArea->screenToDim(mid + Point(-100, 40))), NULL);
  benchFrame();
  nbad += layerCompare("scroll_stroke_one_frame");
  // ... and a stroke followed by a scroll in the same frame: dirty before the pan, kept in Dim
  benchWord(rng, scribbleArea->dimToPageDim(scribbleArea->screenToDim(mid + Point(-60, -60))), NULL);
  scribbleArea->scrollBy(0, -25);
  benchFrame();
  nbad += layerCompare("stroke_scroll_one_frame");
  // selection, scrolled, moved, cancelled, undone
  doCommand(ID_SELALL);
  benchFrame();
  nbad += layerCompare("select_all");
  scroll(0, -20);
  nbad += layerCompare("select_scroll");
  benchDrag(mid, mid + Point(35, 22), 6, pen, NULL);
  nbad += layerCompare("select_move");
  doCommand(ID_CANCEL);
  benchFrame();
  nbad += layerCompare("deselect");
  doCommand(ID_UNDO);
  benchFrame();
  nbad += layerCompare("undo");
  // night mode, on and off, scrolled in between
  ScribbleApp::win->toggleInvertColors();
  benchFrame();
  nbad += layerCompare("night");
  scribbleMode->setMode(MODE_PAN);
  scroll(0, -60);
  nbad += layerCompare("night_scroll");
  ScribbleApp::win->toggleInvertColors();
  benchFrame();
  nbad += layerCompare("day");
  doCommand(ID_RESETZOOM);
  benchFrame();
  nbad += layerCompare("reset_zoom");
  scroll(0, 1000);
  nbad += layerCompare("scroll_top");

  // dotted paper
  ScribbleApp::cfg->set("dotRadius", 1.5f);
  ScribbleApp::cfg->set("xRuling", 40.0f);
  scribbleMode->setMode(MODE_STROKE);
  scribbleDoc->newDocument();
  benchFrame();
  benchHandwriting(rng, 4, 5, 80);
  ScribbleApp::cfg->set("dotRadius", 0.0f);
  ScribbleApp::cfg->set("xRuling", 0.0f);
  scribbleArea->gotoPos(0, Point(-10, -10));
  settle();
  nbad += layerCompare("dotted");
  scribbleMode->setMode(MODE_PAN);
  for(int step : {3, 250, -41}) {
    scroll(0, -step);
    nbad += layerCompare(fstring("dotted_scroll_%d", step).c_str());
  }
  scribbleMode->setMode(MODE_STROKE);
  benchWord(rng, scribbleArea->dimToPageDim(scribbleArea->screenToDim(mid)), NULL);
  benchFrame();
  nbad += layerCompare("dotted_stroke");

  // a PDF page background
  if(PdfImport::isAvailable()) {
    std::string pdf = benchMakePdf(1);
    std::string pdffile = layerTestDir + "/layer.pdf";
    {
      FileStream out(pdffile.c_str(), "wb");
      out.write(pdf.data(), pdf.size());
    }
    scribbleDoc->newDocument();
    benchFrame();
    {
      Document scratch;
      PdfImport::Options opts;
      if(PdfImport::importPdf(&scratch, pdffile.c_str(), opts) > 0)
        scribbleDoc->insertPagesFrom(&scratch);
    }
    scribbleArea->gotoPos(0, Point(-10, -10));
    settle();
    nbad += layerCompare("pdf");
    scribbleMode->setMode(MODE_PAN);
    for(int step : {5, 300, -77}) {
      scroll(0, -step);
      nbad += layerCompare(fstring("pdf_scroll_%d", step).c_str());
    }
    ScribbleApp::win->toggleInvertColors();
    benchFrame();
    nbad += layerCompare("pdf_night");
    scroll(0, -33);
    nbad += layerCompare("pdf_night_scroll");
    ScribbleApp::win->toggleInvertColors();
    benchFrame();
  }

  nbad += selDragLayerTest(rng);

  scribbleMode->setMode(MODE_STROKE);
  scribbleDoc->newDocument();
  benchFrame();
  benchOut("layer_bad_steps", nbad, "count");
  fprintf(stderr, "BENCH_DONE\n");
}
