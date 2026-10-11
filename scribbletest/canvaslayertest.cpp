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

int ScribbleTest::layerCompare(const char* name)
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
      if(d == 0)
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
  bool bad = maxdiff > LAYER_MAX_DIFF || ndiff > LAYER_MAX_FRAC*(vx1 - vx0)*(vy1 - vy0);
  fprintf(stderr, "BENCH layer_%s_diff_px: %d count\n", name, ndiff);
  fprintf(stderr, "BENCH layer_%s_max_diff: %d level\n", name, maxdiff);
  if(ndiff > 0 || noutside > 0)
    fprintf(stderr, "  %s%s: %d px differ, max %d, bbox %.0f %.0f %.0f %.0f; %d px outside the view\n", name,
        bad ? " BAD" : "", ndiff, maxdiff, bbox.left, bbox.top, bbox.right, bbox.bottom, noutside);
  if(bad || getenv("SUMI_LAYERTEST_SAVEALL")) {
    for(int ii = 0; ii < 2; ++ii) {
      Image img(w, h);
      memcpy(img.bytes(), ii ? direct.data() : layered.data(), size_t(w)*h*4);
      for(size_t px = 0; px < size_t(w)*h; ++px)
        img.bytes()[px*4 + 3] = 255;
      auto enc = img.encodePNG();
      std::string fname = layerTestDir + "/" + name + (ii ? "_direct.png" : "_layer.png");
      FileStream out(fname.c_str(), "wb");
      out.write((const char*)enc.data(), enc.size());
    }
  }
  return bad ? 1 : 0;
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

  scribbleMode->setMode(MODE_STROKE);
  scribbleDoc->newDocument();
  benchFrame();
  benchOut("layer_bad_steps", nbad, "count");
  fprintf(stderr, "BENCH_DONE\n");
}
