#include "scribbletest.h"
#include <fstream>
#include <set>

#include "usvg/svgparser.h"
#include "application.h"
#include "strokebuilder.h"
#include "scribblesync.h"
#include "scribbleapp.h"  // only for sync tests
#include "notefulimport.h"
#include "pdfimport.h"
#include "tagstore.h"
#include "miniz/miniz_zip.h"

// document scanning math; unlike everything else here it needs neither GL nor a document
#include "scantest.cpp"
#include "shapetest.cpp"
#include "colortest.cpp"
#include "layertest.cpp"
#include "regiontest.cpp"
#include "librarytest.cpp"
#include "notefultest.cpp"
#include "pagetagtest.cpp"

// Ideally, these tests should be run under valgrind to help check for memory leaks
// renaming out files to refs (Linux):  for i in {0..13}; do mv "test${i}_out.html" "test${i}_ref.html"; done;

// runAll run times (tests 0 - 10):
//  Debian-NDK VM (Qt 4.7 gcc): 440 ms (debug build), 12600 ms (valgrind, 2nd run)
//  Dev-XP VM (Qt 4.8.4, MSVC 2010): 1300 ms (debug), 453 ms (release, standalone)
//  X61T: TBD
// standalone means not started from Qt Creator (so no qDebug output)

// Known sync test issues: 9 (randomStr for bookmark id), 12 (numerical), 14 (numerical)
bool ScribbleTest::exitAfterTest = false;


#ifndef SCRIBBLE_TEST_PATH
#define SCRIBBLE_TEST_PATH "."
#endif

ScribbleTest::ScribbleTest(const std::string& path)
{
  scribbleConfig = new ScribbleConfig();
  scribbleConfig->set("singleFile", true);
  scribbleConfig->set("reduceFileSize", 0);
  // don't use panBorder
  scribbleConfig->set("panFromEdge", false);
  scribbleConfig->set("popupToolbar", false);
  // disable input smoothing
  scribbleConfig->set("inputSmoothing", 0);
  scribbleConfig->set("inputCurveFit", 0);  // reference output is a polyline through the input points
  // the hold timer runs off the wall clock; shapeSnapTest() turns it on and fires it by hand
  scribbleConfig->set("shapeSnapDelay", 0.0f);
  // include thumbnail in test output as a check of rendering
  //  but don't draw page num since fonts are different on different platforms
  scribbleConfig->set("saveThumbnail", 2);
  // don't rely on default page setup
  scribbleConfig->set("pageWidth", 768.0);
  scribbleConfig->set("pageHeight", 1024.0);
  scribbleConfig->set("xRuling", 0.0);
  scribbleConfig->set("yRuling", 40.0);
  scribbleConfig->set("marginLeft", 100.0);
  scribbleConfig->set("ruleColor", Color(Color::BLUE).argb());
  //scribbleConfig->migrateConfig();  // convert xRuling, yRuling, marginLeft to RuleLayer
  // disable DPI detection
  scribbleConfig->set("screenDPI", 150);  //int(ScribbleView::DEFAULT_DPI));
  // disable pen detection (so that mouse input doesn't get disabled)
  scribbleConfig->set("penType", ScribbleInput::DETECTED_PEN);
  //Painter::initPaintSystem();
  scribbleMode = new ScribbleMode(scribbleConfig);
  // NOTE: if you remove or change this, you must find another way to init erase, select, ins space modes!
  scribbleMode->setRuled(true);
  scribbleMode->setMode(MODE_STROKE);
  scribbleDoc = new ScribbleDoc(ScribbleApp::app, scribbleConfig, scribbleMode);
  scribbleArea = new ScribbleArea();
  scribbleDoc->addArea(scribbleArea);
  bookmarkArea = new BookmarkView(scribbleConfig, scribbleDoc);
  //bookmarkArea->setScribbleDoc(scribbleDoc);
  syncSlave = NULL;
  waitForSyncDone = false;
  outPath = path;
  // setup ScribbleArea
  scribbleDoc->newDocument();
  screenRect = Rect::ltwh(0,0,600,800);
  screenImg = new Image(screenRect.width(), screenRect.height());
  screenPaint = new Painter(Painter::PAINT_SW, screenImg);  //| Painter::SRGB_AWARE
  scribbleArea->screenRect = screenRect;
  screenPaint->beginFrame();
  scribbleArea->doPaintEvent(screenPaint);  // ensure that ScribbleView::imgPaint is inited
  screenPaint->endFrame();
  //scribbleArea->setGeometry(Painter::rectToQRect(screenRect));
  swbXML.append_child("swb");
  swbXML.first_child().append_attribute("token").set_value("SCRIBBLE_SYNC_TEST");
  swbXML.first_child().append_attribute("name");
}

ScribbleTest::ScribbleTest(ScribbleDoc* sd, BookmarkView* bv, ScribbleMode* sm)
{
  scribbleDoc = sd;
  scribbleArea = sd->activeArea;
  scribbleMode = sd->scribbleMode;
  bookmarkArea = bv;
  scribbleConfig = NULL;
  screenImg = NULL;
  screenPaint = NULL;
  syncSlave = NULL;
}

ScribbleTest::~ScribbleTest()
{
  if(syncSlave) {
    // ensure disconnect SDL event has been processed before deleting scribbleDoc!
    SDL_Delay(20);
    ScribbleApp::processEvents();
    delete syncSlave;
  }
  if(scribbleConfig) {
    delete screenPaint;
    delete screenImg;
    delete bookmarkArea;
    delete scribbleDoc;
    delete scribbleArea;
    delete scribbleMode;
    delete scribbleConfig;
  }
}

static void distributeTransform(Document* doc)
{
  for(Page* page : doc->pages) {
    for(SvgNode* node : page->contentNode->children()) {
      if(node->type() == SvgNode::PATH && node->hasTransform()) {
        SvgPath* path = static_cast<SvgPath*>(node);
        path->m_path.transform(path->getTransform());
        float w = path->getFloatAttr("stroke-width");
        if(!std::isnan(w))
          path->setAttr<float>("stroke-width", w*path->getTransform().avgScale());
        path->setTransform(Transform2D());
      }
    }
  }
}

void ScribbleTest::startSyncTest(int testnum)
{
  syncTestNum = testnum;
  scribbleDoc->newDocument();
  swbXML.first_child().attribute("name").set_value(fstring("testdoc%d", testnum).c_str());
  scribbleDoc->openSharedDoc(scribbleConfig->String("syncServer"), swbXML.first_child(), false);
  scribbleDoc->scribbleSync->userMessage = [this](std::string msg, int level){ syncSlaveMsg(msg, level); };
  waitForSyncDone = true;
}

void ScribbleTest::checkStrokeMap(ScribbleDoc* doc, const char* msg)
{
  int n = 0;
  for(unsigned int pp = 0; pp < doc->document->pages.size(); pp++)
    n += doc->document->pages[pp]->strokeCount();
  int m = doc->scribbleSync->strokemap.size();
  if(n != m)
    SCRIBBLE_LOG("%s - strokemap count incorrect: %d strokes in document, %d strokes in strokemap", msg, n, m);
}

// wait on disconnect message to save and check slave document and advance to next test
// note that the slave doesn't load any documents
void ScribbleTest::syncSlaveMsg(std::string msg, int level)
{
  if(!StringRef(msg).contains("testuser1 disconnected"))
    return;

  // check strokemap count
  checkStrokeMap(scribbleDoc, "slave (testuser2)");
  distributeTransform(scribbleDoc->document);
  // write output file
  std::string basefile = fstring("%s/test%d", outPath.c_str(), syncTestNum);
  std::string outfile = basefile + "_out.html";
  scribbleDoc->saveDocument(outfile.c_str());
  // compare output to reference; tests of one-file-per-page must handle the svg files themselves
  if(testCompareFiles(outfile.c_str(), (basefile + "_ref.html").c_str(), true))
    scribbleDoc->document->deleteFiles();  //remove(outfile.c_str());
  else
    ++nFailed;
  waitForSyncDone = false;
}

// if svgonly is true, comparison will start from first "<svg" instead of beginning of file
bool ScribbleTest::testCompareFiles(const char* f1, const char* f2, bool svgonly)
{
  std::vector<char> b1;
  std::vector<char> b2;
  readFile(&b1, f1);
  readFile(&b2, f2);
  // fail if either file is missing
  if(b1.empty() || b2.empty())
    return false;
  // convert to null-terminated strings (overwrites last char)
  b1.back() = '\0';
  b2.back() = '\0';
  if(svgonly) {
    char* s1 = strstr(b1.data(), "<svg");
    char* s2 = strstr(b2.data(), "<svg");
    return s1 && s2 && strcmp(s1, s2) == 0;
  }
  return b1.size() == b2.size() && strcmp(b1.data(), b2.data()) == 0;
}

// a bunch of integration tests ... any "*_out.html" files present after test indicate a failure
// TODO: add some tests to capture scribbleArea->imgPaint->image and compare to a ref

// SHAPES_SPEC.md step 1 gate: draw a box, save, reload, resize, change a parameter - still correct.
// Reported through the unit-check count rather than as a testN, so it needs no reference file.
// Interrupting a shape gesture must never leave an orphan behind.  ScribbleInput restarts the gesture
// when the pen button goes down mid-stroke, and the shape tool has to survive that: the live element is
// held on ScribbleArea rather than in the page, so abandoning it without deleting it leaves a shape that
// is painted every frame but belongs to no page - unselectable, unerasable, undeletable.
int ScribbleTest::shapeInterruptTest()
{
  int nbad = 0;
  auto check = [&](bool ok, const char* what) {
    if(!ok) { ++nbad; printf("FAIL: shape interrupt: %s\n", what); }
  };

  auto startShape = [&](int shapeid) {
    scribbleDoc->newDocument();
    scribbleMode->setMode(MODE_STROKE);
    scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 2, ScribblePen::TIP_ROUND));
    scribbleMode->shapeId = shapeid;
    scribbleMode->shapeFlags = 0;
    scribbleMode->setMode(MODE_DRAWSHAPE);
  };

  // drag gesture interrupted by the pen button going down part way through
  startShape(SHAPE_BOX);
  ie(120, 160, 0, pen, press);
  ie(180, 190, 0, pen);
  ie(200, 200, 0, pen, 0, MODEMOD_PENBTN);   // button down mid-drag: input layer cancels and restarts
  ie(0, 0, 0, pen, release, MODEMOD_PENBTN);
  check(scribbleArea->currStroke == NULL, "no live shape may be left over after an interrupted drag");
  check(scribbleArea->shapeInProgress == NULL, "no multi-point shape may be left over either");

  // same again, but with the button already down on the very first press
  startShape(SHAPE_BOX);
  ie(120, 160, 0, pen, press, MODEMOD_PENBTN);
  ie(180, 190, 0, pen, 0, MODEMOD_PENBTN);
  ie(0, 0, 0, pen, release, MODEMOD_PENBTN);
  check(scribbleArea->currStroke == NULL, "pen button on the initial press leaves no live shape");

  // multi-point gesture interrupted between taps
  startShape(SHAPE_POLYLINE);
  ie(100, 100, 0, pen, press);
  ie(0, 0, 0, pen, release);
  ie(200, 100, 0, pen, press);
  ie(0, 0, 0, pen, release);
  check(scribbleArea->shapeInProgress != NULL, "the polyline should still be open between taps");
  ie(250, 300, 0, pen, press, MODEMOD_PENBTN);  // right-click while the polyline is open
  ie(0, 0, 0, pen, release, MODEMOD_PENBTN);
  check(scribbleArea->shapeInProgress == NULL, "interrupting an open polyline must resolve it");
  check(scribbleArea->currStroke == NULL, "interrupting an open polyline leaves no live shape");

  // No element may point at a Selection that is not the current one.  doPressEvent's mode switch assigns
  // currSelection directly without deleting what was there, relying on its earlier clearSelection() pass;
  // anything that installs a selection after that point leaks it, and the elements it held keep rendering
  // as selected while being absent from currSelection - visibly selected, but untouchable.
  for(Element* s : scribbleArea->currPage->children()) {
    check(s->selection() == NULL || s->selection() == scribbleArea->currSelection,
        "no element may be left pointing at an orphaned selection");
  }

  // whatever ended up on the page must be a real, selectable element - the symptom of the orphan bug is
  // that the shape is visible but selectAll() cannot see it
  scribbleArea->selectAll();
  int onPage = scribbleArea->currPage->strokeCount();
  int selected = scribbleArea->currSelection ? scribbleArea->currSelection->count() : 0;
  check(selected == onPage, "everything drawn must be selectable");
  scribbleArea->clearSelection();

  // The other half of the same rule, and the half a user actually notices: a shape the user *finished*
  // must be left selected with its handles up (shapeEditAfterDraw).  The fix above made finishShape()
  // default to not selecting, so this is what stops that default from swallowing the deliberate
  // finishes too - the side-effect paths must stay silent and these must not.
  startShape(SHAPE_POLYLINE);
  scribbleDoc->cfg->set("shapeEditAfterDraw", true);
  ie(100, 500, 0, pen, press);
  ie(0, 0, 0, pen, release);
  ie(250, 500, 0, pen, press);
  ie(0, 0, 0, pen, release);
  ie(250, 620, 0, pen, press);
  ie(0, 0, 0, pen, release);
  ie(250, 620, 0, pen, press);   // tap the last point again: the deliberate finish
  ie(0, 0, 0, pen, release);
  check(scribbleArea->shapeInProgress == NULL, "tapping the last point finishes the polyline");
  check(scribbleArea->currPage->strokeCount() == 1, "and commits exactly one element");
  check(scribbleArea->currSelection && scribbleArea->currSelection->count() == 1,
        "a deliberately finished shape must be left selected for editing");
  if(scribbleArea->currSelection && scribbleArea->currSelection->count() == 1) {
    Element* shape = scribbleArea->currSelection->strokes.front();
    check(shape->isShape(), "and the selected element must still be a shape");
    // the handles are the whole point of selecting it
    std::vector<ShapeHandle> handles;
    if(shape->isShape())
      getShapeHandles(shape->shapeParams(), handles);
    check(!handles.empty(), "and it must offer editing handles");
  }
  scribbleArea->clearSelection();
  scribbleArea->clearSelection();

  scribbleDoc->newDocument();
  return nbad;
}

// Phase 2 of COLORS_SPEC.md: the recipe has to survive the file format, an unthemed document has to
//  inherit the global recipe, and applying a theme must not flatten the pages it touches.
int ScribbleTest::themeRoundTripTest()
{
  int nbad = 0;
  auto check = [&](bool ok, const char* what) {
    if(!ok) { ++nbad; printf("FAIL: theme round trip: %s\n", what); }
  };

  scribbleDoc->newDocument();

  // a document that has never been themed must still have a usable palette, inherited from the global
  //  config - this is what makes an untitled document need no special case
  check(!scribbleDoc->palette().families.empty(), "an unthemed document still has a palette");
  check(scribbleDoc->isThemeGenKnown(), "the default recipe names a generator this build has");

  // Give the two pages *different* rulings and widths, and check both afterwards.  Checking only the
  //  current page would be worthless: setPageProperties() pushes the current page's own properties onto
  //  the others, so the current page looks untouched however badly the rest are flattened - an
  //  assertion that passes against the broken code as well as the fixed one.
  PageProperties p0 = scribbleArea->currPage->getProperties();
  p0.yRuling = 40;
  p0.width = 800;
  scribbleArea->currPage->setProperties(&p0);
  Dim page0Ruling = scribbleArea->currPage->getProperties().yRuling;
  Dim page0Width = scribbleArea->currPage->getProperties().width;

  scribbleDoc->newPage();
  PageProperties p1 = scribbleArea->currPage->getProperties();
  p1.yRuling = 37;
  p1.width = 640;
  scribbleArea->currPage->setProperties(&p1);
  Dim page1Ruling = scribbleArea->currPage->getProperties().yRuling;
  Dim page1Width = scribbleArea->currPage->getProperties().width;
  check(page0Ruling != page1Ruling && page0Width != page1Width,
      "the two pages must actually differ for this check to mean anything");

  PaletteRecipe r;
  r.gen = "cusp-walk-1";
  r.seedHue = 47;
  r.vividness = 0.62f;
  r.depth = 0.17f;
  r.minContrast = 3.5f;
  r.jitter = 6;
  r.paperL = 0.97f;
  r.paperWarm = 0.8f;
  r.families = 12;
  scribbleDoc->setTheme(r, true, false);

  Color themedPaper = scribbleDoc->palette().paper;
  Color themedRule = scribbleDoc->palette().rule;
  check(scribbleDoc->document->pages.size() == 2, "the document should have two pages");
  for(size_t ii = 0; ii < scribbleDoc->document->pages.size(); ++ii) {
    PageProperties got = scribbleDoc->document->pages[ii]->getProperties();
    check(got.color == themedPaper, "applying a theme sets every page's paper color");
    check(got.ruleColor == themedRule, "applying a theme sets every page's rule color");
    // the reason setTheme() does not use setPageProperties(): that assigns the whole struct, so one
    //  shared PageProperties would push the current page's ruling and size onto every other page
    check(got.yRuling == (ii == 0 ? page0Ruling : page1Ruling),
        "applying a theme must not overwrite a page's own ruling");
    check(got.width == (ii == 0 ? page0Width : page1Width),
        "applying a theme must not overwrite a page's own width");
  }

  // Accents follow the theme too (phase 4).  These were global-only config values, so the failure
  //  mode is a bookmark or link in some unrelated blue - the one mark on the page ignoring the
  //  palette - which is easy to miss by eye and trivial to catch here.
  check(Color::fromArgb(scribbleDoc->cfg->Int("bookmarkColor")) == scribbleDoc->palette().bookmark,
      "the theme sets the document's bookmark color");
  check(Color::fromArgb(scribbleDoc->cfg->Int("linkColor")) == scribbleDoc->palette().link,
      "the theme sets the document's link color");
  // ScribbleApp caches the bookmark color in a member, so writing the config alone is not enough
  check(scribbleDoc->app->bookmarkColor == scribbleDoc->palette().bookmark,
      "the theme updates the cached bookmark color, not just the config value");

  // save and reload: the recipe has to survive the document config node
  std::string file = outPath + "/theme_roundtrip_out.html";
  check(scribbleDoc->saveDocument(file.c_str()), "saving the document should succeed");
  scribbleDoc->newDocument();
  check(scribbleDoc->openDocument(file.c_str()) == Document::LOAD_OK, "reloading should succeed");
  removeFile(file.c_str());

  PaletteRecipe back = scribbleDoc->cfg->themeRecipe();
  check(back.gen == "cusp-walk-1", "the generator id survives save/load");
  check(std::abs(back.seedHue - r.seedHue) < 0.01, "the seed hue survives save/load");
  check(std::abs(back.vividness - r.vividness) < 1e-4, "vividness survives save/load");
  check(std::abs(back.depth - r.depth) < 1e-4, "depth survives save/load");
  check(std::abs(back.minContrast - r.minContrast) < 1e-4, "contrast survives save/load");
  check(std::abs(back.paperWarm - r.paperWarm) < 1e-4, "paper warmth survives save/load");
  check(back.families == r.families, "the family count survives save/load");
  // and the reloaded recipe must regenerate the same colors it was saved with
  check(scribbleDoc->palette().paper == themedPaper,
      "the reloaded recipe regenerates the same palette");

  // Starting a fresh document must drop the cached palette.  Note the check above cannot catch a stale
  //  cache - a cache left over from setTheme() holds precisely the palette that check expects - so the
  //  invalidation is only observable somewhere the two answers differ, which is here.
  scribbleDoc->newDocument();
  check(!(scribbleDoc->palette().paper == themedPaper),
      "a new document must not keep the previous document's palette");

  // a document written by a newer build keeps its unknown generator id rather than having it rewritten
  PaletteRecipe future = r;
  future.gen = "cusp-walk-99";
  scribbleDoc->setTheme(future, false, false);
  check(!scribbleDoc->isThemeGenKnown(), "an unknown generator id is reported as such");
  check(!scribbleDoc->palette().families.empty(), "an unknown generator id still yields a palette");
  check(scribbleDoc->cfg->themeRecipe().gen == "cusp-walk-99",
      "an unknown generator id is preserved, not rewritten to this build's default");

  scribbleDoc->newDocument();
  return nbad;
}

// Outline (table of contents) entries must follow their page through document edits.  The whole point
//  of storing the entry inside the page's own SVG rather than in a {page number, title} list on
//  Document is that inserting or deleting a page above it cannot invalidate it - so that is what this
//  checks, along with the round trip, the level normalization and the undo step.
int ScribbleTest::notefulImportTest()
{
  int nbad = 0;
  auto check = [&](bool ok, const char* what) {
    if(!ok) { ++nbad; printf("FAIL: noteful import: %s\n", what); }
  };
  const char* tmpdir = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
  std::string notebookPath = std::string(tmpdir) + "/sumi-noteful-test.noteful";
  std::string savedPath = std::string(tmpdir) + "/sumi-noteful-test.svgz";

  // Converting a real notebook for inspection, with no UI for it yet:
  //  NOTEFUL_CONVERT=in.noteful NOTEFUL_OUT=out.svgz ./Debug/Sumi --test
  //  NOTEFUL_CONVERT=export.zip NOTEFUL_OUT=librarydir ./Debug/Sumi --test   (a zip or a directory)
  if(getenv("NOTEFUL_CONVERT") && getenv("NOTEFUL_OUT") && NotefulImport::isNotefulArchive(getenv("NOTEFUL_CONVERT"))) {
    NotefulImport::ArchiveResult result;
    std::string error;
    int docs = NotefulImport::importArchive(getenv("NOTEFUL_CONVERT"), getenv("NOTEFUL_OUT"),
        NotefulImport::ArchiveOptions(), &result, &error);
    printf("Noteful archive: %d documents, %d failed %s\n", docs, result.failed, error.c_str());
    for(const NotefulImport::ArchiveEntry& entry : result.entries) {
      printf("  %s -> %s %s\n", entry.archivePath.c_str(), entry.docPath.c_str(), entry.error.c_str());
      for(const std::string& tag : entry.tagPaths)
        printf("    tag %s\n", tag.c_str());
      for(const std::string& warning : entry.result.warnings)
        printf("    warning: %s\n", warning.c_str());
    }
  }
  else if(getenv("NOTEFUL_CONVERT") && getenv("NOTEFUL_OUT")) {
    Document converted;
    NotefulImport::Result result;
    std::string error;
    int pages = NotefulImport::importNoteful(&converted, getenv("NOTEFUL_CONVERT"), NotefulImport::Options(),
        &result, &error);
    float savedImageScale = SvgWriter::DEFAULT_SAVE_IMAGE_SCALED;
    SvgWriter::DEFAULT_SAVE_IMAGE_SCALED = 0;  // as for PDF import: keep the backgrounds' resolution
    bool saved = pages > 0 && converted.save(new FileStream(getenv("NOTEFUL_OUT"), "wb"), NULL, Document::SAVE_FORCE);
    SvgWriter::DEFAULT_SAVE_IMAGE_SCALED = savedImageScale;
    printf("Noteful convert: %d pages, %d text boxes skipped, saved: %d %s\n", pages, result.textBoxesSkipped,
        saved, error.c_str());
    for(const std::string& warning : result.warnings)
      printf("  warning: %s\n", warning.c_str());
  }

  // the synthetic notebook from notefultest.cpp: a grid page with ink on two layers, some of it below
  //  the page's bottom edge, and an imported page whose (fake) PDF cannot be rendered
  {
    std::string sample = sampleNotebook();
    FileStream out(notebookPath.c_str(), "wb");
    out.write(sample.data(), sample.size());
  }
  Document doc;
  NotefulImport::Result result;
  std::string error;
  int pages = NotefulImport::importNoteful(&doc, notebookPath.c_str(), NotefulImport::Options(), &result, &error);
  check(pages == 2 && doc.numPages() == 2, "both live pages imported");
  if(doc.numPages() != 2)
    return nbad;

  check(doc.layers.size() == 2 && doc.layers.byIndex(0)->name == "Layer 1" && doc.layers.byIndex(1)->name == "Layer 2",
      "layer table, bottom first");
  int topLayer = doc.layers.size() == 2 ? doc.layers.byIndex(1)->id : -1;
  Page* grid = doc.pages[0];
  int onBottom = 0, onTop = 0;
  for(Element* s : grid->children()) {
    if(s->layer() == LayerList::DEFAULT_LAYER) ++onBottom;
    else if(s->layer() == topLayer) ++onTop;
  }
  // bottom: the 3 point, flag-2 and four pressure strokes, the ellipse, the triangle and the highlighter;
  //  top: the 4 point stroke, the last box stroke and the line
  check(onBottom == 9 && onTop == 3, "each element on its layer");
  // Noteful stores a highlighter opaque: imported at the marker's alpha, under the ink of its layer
  //  (the bottom one, so first on the page) although it comes last among the elements
  Element* first = grid->children().begin() != grid->children().end() ? *grid->children().begin() : NULL;
  check(first && first->layer() == LayerList::DEFAULT_LAYER && fabs(first->node->getFloatAttr("stroke-opacity", 1) - 0.5) < 0.01,
      "highlighter translucent and under the ink");
  int pressureStrokes = 0;
  for(Element* s : grid->children())
    pressureStrokes += s->node->hasClass(Element::ROUND_PEN_CLASS);
  // a filled round-pen outline, as Sumi draws a pressure pen - not a constant-width stroked path
  check(pressureStrokes == 4, "pressure strokes become variable-width strokes");

  const Dim scale = 150.0/132;
  check(fabs(grid->props.yRuling - 20*150.0/72) < 1e-6 && fabs(grid->props.xRuling - grid->props.yRuling) < 1e-6,
      "grid paper becomes Sumi ruling at Noteful's spacing (points, not page units)");
  check(fabs(grid->width() - 1091.3385826771655*scale) < 1e-3, "page width scaled to Sumi units");
  // a stroke reaches y = 1800 on a 1543 unit page; Noteful keeps it, so the page grows
  check(grid->height() > 1800*scale, "page extended to hold ink below its edge");
  check(!grid->isCustomRuling, "paper page keeps a standard ruling");
  Page* imported = doc.pages[1];
  check(!result.warnings.empty(), "an unrenderable background is reported");
  check(fabs(imported->width() - 869*scale) < 1e-3 && fabs(imported->height() - 1315.8*scale) < 1e-3,
      "a cropped page keeps its own size");

  check(grid->outlineTitle == "Grundlagen" && grid->outlineLevel == 0, "outline entry on its page");
  check(imported->outlineTitle == "Anhang", "second outline entry on the second page");
  // Sumi has one entry per page; the child sharing Grundlagen's page is dropped and said so
  bool droppedReported = false;
  for(const std::string& warning : result.warnings)
    droppedReported = droppedReported || warning.find("Kettenregel") != std::string::npos;
  check(droppedReported, "a second entry on one page is dropped with a warning");

  check(result.title == "Mathe" && result.tags.size() == 2 && result.textBoxesSkipped == 1,
      "title, tags and skipped text boxes reported");

  // the layer table rides the document config; a Document saved directly must keep it
  check(doc.save(new FileStream(savedPath.c_str(), "wb"), NULL, Document::SAVE_FORCE), "saved");
  Document reloaded;
  check(reloaded.load(new FileStream(savedPath.c_str(), "rb")) == Document::LOAD_OK, "reloaded");
  // a default config, as a document load uses: loadConfig() only takes keys the config already knows
  ScribbleConfig reloadedCfg;
  reloadedCfg.loadConfig(reloaded.getConfigNode());
  LayerList reloadedLayers = LayerList::parse(reloadedCfg.String("layers", ""));
  check(reloadedLayers.size() == 2 && reloadedLayers.byIndex(1)->name == "Layer 2", "layer table survives save");
  check(reloaded.numPages() == 2 && reloaded.pages[0]->ensureLoaded(false)
      && std::distance(reloaded.pages[0]->children().begin(), reloaded.pages[0]->children().end()) == 12,
      "ink survives save");

  removeFile(notebookPath);
  removeFile(savedPath);
  return nbad;
}

int ScribbleTest::notefulArchiveTest()
{
  int nbad = 0;
  auto check = [&](bool ok, const char* what) {
    if(!ok) { ++nbad; printf("FAIL: noteful archive: %s\n", what); }
  };
  const char* tmpdir = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
  FSPath base(fstring("%s/sumi-noteful-archive-%lld", tmpdir, (long long)mSecSinceEpoch()));
  FSPath source = base.child("src"), library = base.child("lib"), zipLibrary = base.child("ziplib");
  std::string sample = sampleNotebook();
  auto writeBytes = [](const FSPath& path, const std::string& bytes) {
    createPath(path.parentPath());
    FileStream out(path.c_str(), "wb");
    out.write(bytes.data(), bytes.size());
  };
  // two notebooks, both titled "Mathe": one in KA, one in KA/Test
  writeBytes(source.child("KA").child("One.noteful"), sample);
  writeBytes(source.child("KA").child("Test").child("Two.noteful"), sample);
  writeBytes(source.child("KA").child("notes.txt"), "not a notebook");

  // tag path of a tag id, "/" separated, from the library's index
  auto tagPath = [](const TagStore& store, const std::string& id) {
    std::string path;
    for(const TagNode* node = store.tag(id); node; node = node->parentId.empty() ? NULL : store.tag(node->parentId))
      path = path.empty() ? node->name : node->name + "/" + path;
    return path;
  };
  auto docTagPaths = [&](const FSPath& lib, const std::string& doc) {
    TagStore store(lib.child(".write-tags").c_str());
    store.load();
    std::vector<std::string> paths;
    for(const std::string& id : ScribbleDoc::extractDocTags(doc.c_str()))
      paths.push_back(tagPath(store, id));
    std::sort(paths.begin(), paths.end());
    return paths;
  };

  NotefulImport::ArchiveResult result;
  std::string error;
  int imported = NotefulImport::importArchive(source.c_str(), library.c_str(), NotefulImport::ArchiveOptions(),
      &result, &error);
  check(imported == 2 && result.failed == 0, "directory: both notebooks imported, the text file ignored");
  check(library.child("Mathe.svgz").exists() && library.child("Mathe (2).svgz").exists(),
      "named after the notebook, a clash numbered");
  const NotefulImport::ArchiveEntry* inTest = NULL;
  for(const NotefulImport::ArchiveEntry& entry : result.entries)
    if(entry.folder == "KA/Test") inTest = &entry;
  check(inTest && !inTest->docPath.empty(), "the subfolder's notebook found");
  if(inTest) {
    std::vector<std::string> expected = {"Jahre/11", "KA/Test", "Schule/Mathe"};
    check(docTagPaths(library, inTest->docPath) == expected,
        "document carries its own tags and its folder, as subtags of their parents");
  }

  // importing again reuses the tags rather than creating a second "KA"
  NotefulImport::importArchive(source.c_str(), library.c_str());
  TagStore store(library.child(".write-tags").c_str());
  store.load();
  int kaTags = 0;
  for(const auto& pair : store.allTags())
    kaTags += pair.second.name == "KA";
  check(kaTags == 1, "tags are reused, not duplicated, by a second import");
  check(library.child("Mathe (4).svgz").exists(), "a second import adds documents rather than overwriting");

  // the same as a zip, as Noteful exports it (plus the junk macOS adds), with folder tags off
  {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    mz_zip_writer_init_heap(&zip, 0, 0);
    mz_zip_writer_add_mem(&zip, "archive.json", "{}", 2, MZ_DEFAULT_COMPRESSION);
    mz_zip_writer_add_mem(&zip, "KA/", NULL, 0, 0);
    mz_zip_writer_add_mem(&zip, "KA/One.noteful", sample.data(), sample.size(), MZ_DEFAULT_COMPRESSION);
    mz_zip_writer_add_mem(&zip, "KA/Test/Two.noteful", sample.data(), sample.size(), 0);  // stored
    mz_zip_writer_add_mem(&zip, "__MACOSX/KA/._One.noteful", "junk", 4, 0);
    void* bytes = NULL;
    size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&zip, &bytes, &size);
    writeBytes(base.child("export.zip"), std::string((const char*)bytes, size));
    mz_zip_writer_end(&zip);
  }
  NotefulImport::ArchiveOptions noFolders;
  noFolders.folderTags = false;
  NotefulImport::ArchiveResult zipResult;
  imported = NotefulImport::importArchive(base.child("export.zip").c_str(), zipLibrary.c_str(), noFolders,
      &zipResult, &error);
  check(imported == 2 && zipResult.failed == 0, "zip: both notebooks imported, macOS junk skipped");
  bool noFolderTag = zipResult.entries.size() == 2;
  for(const NotefulImport::ArchiveEntry& entry : zipResult.entries) {
    std::vector<std::string> expected = {"Jahre/11", "Schule/Mathe"};
    noFolderTag = noFolderTag && docTagPaths(zipLibrary, entry.docPath) == expected;
  }
  check(noFolderTag, "folder tags off: only the notebooks' own tags");

  // a broken notebook is reported and does not stop the others
  writeBytes(source.child("KA").child("Broken.noteful"), sample.substr(0, sample.size()/2));
  NotefulImport::ArchiveResult brokenResult;
  imported = NotefulImport::importArchive(source.c_str(), base.child("lib3").c_str(), NotefulImport::ArchiveOptions(),
      &brokenResult, &error);
  check(imported == 2 && brokenResult.failed == 1, "a broken notebook fails alone");

  removeDir(base);
  return nbad;
}

int ScribbleTest::pdfImportTest()
{
  int nbad = 0;
  auto check = [&](bool ok, const char* what) {
    if(!ok) { ++nbad; printf("FAIL: pdf import: %s\n", what); }
  };
  if(!PdfImport::isAvailable())
    return 0;
  // three A4 pages, each with a blue square at (100, 100)-(300, 300) pt, PDF's y up - so 542..742 pt from
  //  the top.  No xref: MuPDF repairs that, as it would any damaged file.
  std::string pdf = "%PDF-1.4\n"
      "1 0 obj <</Type/Catalog/Pages 2 0 R>> endobj\n"
      "2 0 obj <</Type/Pages/Kids[3 0 R 5 0 R 6 0 R]/Count 3>> endobj\n"
      "3 0 obj <</Type/Page/Parent 2 0 R/MediaBox[0 0 595 842]/Contents 4 0 R>> endobj\n"
      "5 0 obj <</Type/Page/Parent 2 0 R/MediaBox[0 0 595 842]/Contents 4 0 R>> endobj\n"
      "6 0 obj <</Type/Page/Parent 2 0 R/MediaBox[0 0 595 842]/Contents 4 0 R>> endobj\n"
      "4 0 obj <</Length 29>> stream\n0 0 1 rg 100 100 200 200 re f\nendstream endobj\n"
      "trailer <</Root 1 0 R>>\n%%EOF\n";
  // RGBA of a decoded pixel
  auto pixel = [](Image& image, int x, int y) {
    const unsigned char* bytes = image.bytes() + (size_t(y)*image.width + x)*4;
    return std::vector<int>{bytes[0], bytes[1], bytes[2]};
  };
  auto isBlue = [](const std::vector<int>& rgb) { return rgb[0] < 40 && rgb[1] < 40 && rgb[2] > 200; };
  auto isWhite = [](const std::vector<int>& rgb) { return rgb[0] > 240 && rgb[1] > 240 && rgb[2] > 240; };
  const Dim scale = 300.0/72;

  PdfImport::Renderer renderer;
  check(renderer.openMemory(pdf, NULL) && renderer.numPages() == 3, "opened from memory");
  Dim widthPt = 0, heightPt = 0;
  Image page = renderer.render(0, 300, Image::PNG, &widthPt, &heightPt);
  // kept decoded, a 300 DPI page is ~35 MB, and a notebook of them ended the app on an iPad
  check(!page.isNull() && page.data == NULL && !page.encData.empty(), "page kept as its encoded bytes only");
  check(widthPt == 595 && heightPt == 842 && std::abs(page.width - int(595*scale + 0.5)) <= 1, "page size and resolution");
  if(!page.isNull()) {
    // MuPDF's RGB samples go to the encoder as they are: a swapped channel would make the square red
    check(isBlue(pixel(page, int(200*scale), int(642*scale))) && isWhite(pixel(page, int(50*scale), int(50*scale))),
        "square blue where the PDF put it, page white around it");
  }

  // the bottom-left quarter only, rendered no further: the square moves up by the 421 pt cut off above
  Image quarter = renderer.render(0, 300, Image::JPEG, NULL, NULL, Rect::ltrb(0, 0.5, 0.5, 1));
  check(!quarter.isNull() && std::abs(quarter.width - int(297.5*scale + 0.5)) <= 1
      && std::abs(quarter.height - int(421*scale + 0.5)) <= 1, "crop renders just that part");
  if(!quarter.isNull())
    check(isBlue(pixel(quarter, int(200*scale), int(221*scale))), "crop keeps the content in place");

  // a budget that cannot hold a 300 DPI page (8.7 Mpx) gets one at a lower resolution, and says so
  {
    PdfImport::MemoryBudget budget(size_t(40) << 20);
    PdfImport::Renderer limited;
    Image small = limited.openMemory(pdf, &budget) ? limited.render(0, 300, Image::PNG) : Image(0, 0);
    check(!small.isNull() && small.width < page.width/2 && small.width >= 594,
        "a page that does not fit the budget comes out smaller, never under 72 DPI");
    check(budget.reducedPages == 1 && budget.lowestDpi < 300, "the reduced page is counted");
  }
  // with the budget spent (a whole notebook held, say), a page still comes out, at 72 DPI - capped at
  //  what was left, MuPDF used to fail it and the page was dropped
  {
    PdfImport::MemoryBudget budget(size_t(1) << 20);
    budget.setInput(size_t(4) << 20);
    PdfImport::Renderer spent;
    Image coarse = spent.openMemory(pdf, &budget) ? spent.render(1, 300, Image::PNG) : Image(0, 0);
    check(!coarse.isNull() && std::abs(coarse.width - 595) <= 1, "a spent budget still gets a page, at 72 DPI");
  }

  // ImportSaver: each page written as it is made and unloaded, the file whole at the end
  const char* tmpdir = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
  std::string pdfPath = fstring("%s/sumi-pdf-import-%lld.pdf", tmpdir, (long long)mSecSinceEpoch());
  std::string savedPath = pdfPath + ".svgz";
  {
    FileStream out(pdfPath.c_str(), "wb");
    out.write(pdf.data(), pdf.size());
  }
  int pages = -1;
  bool allUnloaded = true;
  bool finished = false;
  {
    Document doc;
    PdfImport::ImportSaver saver(&doc, savedPath);
    PdfImport::Options opts;
    opts.dpi = 72;  // the size of the pages does not matter here
    opts.saver = &saver;
    pages = PdfImport::importPdf(&doc, pdfPath.c_str(), opts);
    for(Page* imported : doc.pages)
      allUnloaded = allUnloaded && imported->loadStatus == Page::NOT_LOADED;
    finished = pages == 3 && saver.finish();
  }
  check(pages == 3, "every page imported");
  check(allUnloaded, "pages written are unloaded, so the import holds one at a time");
  check(finished, "saved");
  Document reloaded;
  check(reloaded.load(new FileStream(savedPath.c_str(), "rb")) == Document::LOAD_OK && reloaded.numPages() == 3,
      "the file written page by page loads with all its pages");
  bool backgrounds = reloaded.numPages() == 3;
  for(Page* loaded : reloaded.pages)
    backgrounds = backgrounds && loaded->ensureLoaded(false) && loaded->ruleNode && loaded->ruleNode->selectFirst("image");
  check(backgrounds, "every page has its background image");

  removeFile(pdfPath);
  removeFile(savedPath);
  return nbad;
}

int ScribbleTest::outlineTest()
{
  int nbad = 0;
  auto check = [&](bool ok, const char* what) {
    if(!ok) { ++nbad; printf("FAIL: outline: %s\n", what); }
  };
  // find the entry for a given title, or NULL
  auto entryFor = [&](std::vector<OutlineEntry>& es, const char* title) -> OutlineEntry* {
    for(OutlineEntry& e : es) { if(e.title == title) return &e; }
    return NULL;
  };

  scribbleDoc->newDocument();
  scribbleDoc->newPage();
  scribbleDoc->newPage();  // 3 pages: 0, 1, 2
  check(scribbleDoc->document->numPages() == 3, "three pages to work with");

  check(scribbleDoc->outline().empty(), "a fresh document has an empty outline");

  check(scribbleDoc->setPageOutline(0, "Intro", 0), "setting an entry succeeds");
  check(scribbleDoc->setPageOutline(2, "Results", 1), "setting a second entry succeeds");
  // a no-op must not push an empty undo step
  check(!scribbleDoc->setPageOutline(0, "Intro", 0), "setting the same entry again is a no-op");
  check(!scribbleDoc->setPageOutline(99, "Nowhere", 0), "an out-of-range page is refused");

  std::vector<OutlineEntry> es = scribbleDoc->outline();
  check(es.size() == 2, "two entries");
  check(es.size() == 2 && es[0].title == "Intro" && es[1].title == "Results",
      "entries come back in page order");
  check(entryFor(es, "Intro") && entryFor(es, "Intro")->pagenum == 0, "Intro is on page 0");
  check(entryFor(es, "Results") && entryFor(es, "Results")->pagenum == 2, "Results is on page 2");
  // page 2 asked for level 1 and follows a level 0, so it is a legal child and must stay at 1
  check(entryFor(es, "Results") && entryFor(es, "Results")->level == 1, "a legal nesting level is kept");

  // *** the requirement: insert a page above and the entries must still name the right pages ***
  scribbleDoc->newPage(0);  // insert a new page at the very front
  check(scribbleDoc->document->numPages() == 4, "the page was inserted");
  es = scribbleDoc->outline();
  check(es.size() == 2, "inserting a page adds no entries and loses none");
  check(entryFor(es, "Intro") && entryFor(es, "Intro")->pagenum == 1,
      "an entry follows its page when a page is inserted above it");
  check(entryFor(es, "Results") && entryFor(es, "Results")->pagenum == 3,
      "a later entry follows its page too");
  // and the title must be on the page it points at, not merely at a shifted index
  check(scribbleDoc->document->pages[1]->outlineTitle == "Intro",
      "the entry is stored on the page it names");
  check(!scribbleDoc->document->pages[0]->hasOutlineEntry(),
      "the newly inserted page has no entry of its own");

  // deleting a page above must shift them back
  scribbleDoc->deletePage(0);
  es = scribbleDoc->outline();
  check(entryFor(es, "Intro") && entryFor(es, "Intro")->pagenum == 0,
      "an entry follows its page when a page above it is deleted");

  // Level normalization: an entry may not sit more than one level below the one before it.  Here the
  //  first entry is level 0, so a level-3 request must come back as 1 - otherwise deleting the page
  //  that held a parent leaves children hanging under nothing.
  check(scribbleDoc->setPageOutline(1, "Orphan", 3), "setting a deep entry succeeds");
  es = scribbleDoc->outline();
  check(entryFor(es, "Orphan") && entryFor(es, "Orphan")->level == 1,
      "a level more than one deeper than the previous entry is normalized");
  // but the page keeps what was asked for, so restoring the parent restores the intent
  check(scribbleDoc->document->pages[1]->outlineLevel == 3,
      "the stored level is the user's request, not the normalized one");
  check(scribbleDoc->setPageOutline(1, NULL, 0), "clearing an entry succeeds");
  check(scribbleDoc->outline().size() == 2, "a cleared entry leaves the outline");

  // one undo step per change: undo the clear and the entry comes back
  scribbleDoc->doCommand(ID_UNDO);
  es = scribbleDoc->outline();
  check(entryFor(es, "Orphan") != NULL, "undo restores a cleared entry");
  check(entryFor(es, "Orphan") && scribbleDoc->document->pages[1]->outlineLevel == 3,
      "undo restores the entry's level too");
  scribbleDoc->doCommand(ID_REDO);
  check(scribbleDoc->outline().size() == 2, "redo clears it again");

  // save and reload: the entry rides the page SVG
  std::string file = outPath + "/outline_roundtrip_out.html";
  check(scribbleDoc->saveDocument(file.c_str()), "saving the document should succeed");
  scribbleDoc->newDocument();
  check(scribbleDoc->openDocument(file.c_str()) == Document::LOAD_OK, "reloading should succeed");
  removeFile(file.c_str());

  es = scribbleDoc->outline();
  check(es.size() == 2, "both entries survive save/reload");
  check(entryFor(es, "Intro") && entryFor(es, "Intro")->pagenum == 0, "Intro reloads on page 0");
  check(entryFor(es, "Results") && entryFor(es, "Results")->pagenum == 2, "Results reloads on page 2");
  check(entryFor(es, "Results") && entryFor(es, "Results")->level == 1,
      "the nesting level survives save/reload");

  // a title containing XML metacharacters must survive both the file and the sync wire format
  const char* nasty = "R&D <draft> \"one\" 'two'";
  check(scribbleDoc->setPageOutline(1, nasty, 1), "setting a title with markup characters succeeds");
  file = outPath + "/outline_escape_out.html";
  check(scribbleDoc->saveDocument(file.c_str()), "saving with a markup title should succeed");
  scribbleDoc->newDocument();
  check(scribbleDoc->openDocument(file.c_str()) == Document::LOAD_OK, "reloading should succeed");
  removeFile(file.c_str());
  check(scribbleDoc->document->pages[1]->outlineTitle == nasty,
      "a title containing & < > and quotes round-trips unmangled");

  // The check above only exercises the *file* format (SvgWriter escapes for us).  The sync wire
  //  format is hand-written, and a title is the first arbitrary user string to go on it, so serialize
  //  the undo item and parse it back the way ScribbleSync would - an unescaped & or ' produces a
  //  malformed item and desyncs the stream, which nothing else here would notice.
  {
    MemStream strm;
    PageOutlineItem item(scribbleDoc->document->pages[1]);
    item.serialize(strm);
    std::string wire(strm.data(), strm.size());
    pugi::xml_document wiredoc;
    bool parsed = wiredoc.load_buffer(wire.data(), wire.size());
    check(parsed, "the serialized outline item is well-formed XML");
    pugi::xml_node n = wiredoc.child("outlinechanged");
    check(!n.empty(), "the item serializes as <outlinechanged>");
    check(n.attribute("pagenum").as_int(-1) == 1, "the wire format carries the page number");
    check(n.attribute("level").as_int(-1) == 1, "the wire format carries the level");
    check(strcmp(n.attribute("title").as_string(), nasty) == 0,
        "a title with markup characters survives the sync wire format");
  }

  scribbleDoc->newDocument();
  return nbad;
}

// Dragging an outline entry onto another (Sidebar -> ScribbleDoc::nestOutlineEntry).  Nesting is
//  positional, so this moves pages; the checks are that each entry ends up at the right page *and*
//  level, that its whole section (untitled pages included) travels with it, and that it is one undo step.
int ScribbleTest::outlineNestTest()
{
  int nbad = 0;
  auto check = [&](bool ok, const char* what) {
    if(!ok) { ++nbad; printf("FAIL: outline nest: %s\n", what); }
  };
  // the outline as "title@page:level ..." - one string to compare, and printed on failure
  auto shape = [&](){
    std::string result;
    for(const OutlineEntry& entry : scribbleDoc->outline())
      result += fstring("%s@%d:%d ", entry.title.c_str(), entry.pagenum, entry.level);
    return result;
  };
  auto expect = [&](const char* want, const char* what){
    std::string got = shape();
    check(got == want, what);
    if(got != want)
      printf("  expected '%s'\n  got      '%s'\n", want, got.c_str());
  };

  scribbleDoc->newDocument();
  for(int ii = 0; ii < 5; ++ii)
    scribbleDoc->newPage();  // 6 pages
  // an untitled page inside a section: pages 2 and 4 carry no entry and must move with theirs.  Their
  //  width marks them, since a title cannot
  PageProperties marked = scribbleDoc->document->pages[2]->getProperties();
  scribbleDoc->startAction(2 | UndoHistory::MULTIPAGE);  // setProperties records an undo item
  marked.width += 100;
  scribbleDoc->document->pages[2]->setProperties(&marked);
  marked.width += 100;
  scribbleDoc->document->pages[4]->setProperties(&marked);
  scribbleDoc->endAction();
  Dim width2 = scribbleDoc->document->pages[2]->width(), width4 = scribbleDoc->document->pages[4]->width();
  scribbleDoc->setPageOutline(0, "A", 0);
  scribbleDoc->setPageOutline(1, "A1", 1);
  scribbleDoc->setPageOutline(3, "B", 0);
  scribbleDoc->setPageOutline(5, "C", 0);
  const char* start = "A@0:0 A1@1:1 B@3:0 C@5:0 ";
  expect(start, "the starting outline");

  // a later entry under an earlier one: C goes to the end of A1's section, i.e. before B
  check(scribbleDoc->nestOutlineEntry(5, 1), "nesting C under A1 succeeds");
  expect("A@0:0 A1@1:1 C@3:2 B@4:0 ", "C lands at the end of A1's section, one level below it");
  check(scribbleDoc->document->numPages() == 6, "a move neither adds nor loses pages");
  check(scribbleDoc->document->pages[2]->width() == width2, "A1's untitled page stays in A1's section");
  check(scribbleDoc->document->pages[5]->width() == width4, "B's untitled page stays with B");

  scribbleDoc->doCommand(ID_UNDO);
  expect(start, "one undo puts the pages and the levels back");
  check(scribbleDoc->document->pages[4]->width() == width4, "undo puts the untitled page back too");
  scribbleDoc->doCommand(ID_REDO);
  expect("A@0:0 A1@1:1 C@3:2 B@4:0 ", "redo moves it again");
  scribbleDoc->doCommand(ID_UNDO);

  // an earlier entry, with its subtree, under a later one: A's section is pages 0-2
  check(scribbleDoc->nestOutlineEntry(0, 3), "nesting A under B succeeds");
  expect("B@0:0 A@2:1 A1@3:2 C@5:0 ", "A and its child move under B and both go one level deeper");
  check(scribbleDoc->document->pages[1]->width() == width4, "B's own untitled page stays first in its section");
  check(scribbleDoc->document->pages[4]->width() == width2, "A's untitled page travels with A");

  check(!scribbleDoc->nestOutlineEntry(2, 3), "an entry cannot go under its own child");
  check(!scribbleDoc->nestOutlineEntry(2, 2), "an entry cannot go under itself");
  check(!scribbleDoc->nestOutlineEntry(0, -1), "a top-level entry cannot be moved to the top level");
  expect("B@0:0 A@2:1 A1@3:2 C@5:0 ", "a refused move changes nothing");

  // to the top level: after the top-level section it was in, so it does not adopt what follows it
  check(scribbleDoc->nestOutlineEntry(2, -1), "moving A to the top level succeeds");
  expect("B@0:0 A@2:0 A1@3:1 C@5:0 ", "A, already last in B's section, only changes level");
  scribbleDoc->doCommand(ID_UNDO);
  scribbleDoc->setPageOutline(1, "B1", 1);  // now A is not last in B's section
  expect("B@0:0 B1@1:1 A@2:1 A1@3:2 C@5:0 ", "B1 inserted ahead of A, as its sibling");
  check(scribbleDoc->nestOutlineEntry(1, -1), "moving B1 to the top level succeeds");
  expect("B@0:0 A@1:1 A1@2:2 B1@4:0 C@5:0 ", "B1 moves past the rest of B's section, keeping A under B");

  // out of its parent (dragging it onto that parent): one level up, straight after the parent's section
  check(scribbleDoc->nestOutlineEntry(5, 1), "nesting C under A succeeds");
  expect("B@0:0 A@1:1 A1@2:2 C@4:2 B1@5:0 ", "C is now A's last child, after A1's section");
  check(scribbleDoc->nestOutlineEntry(2, ScribbleDoc::OUTLINE_OUTDENT), "moving A1 out of A succeeds");
  expect("B@0:0 A@1:1 C@2:2 A1@3:1 B1@5:0 ", "A1 lands after A's section, as A's sibling, leaving C under A");
  check(scribbleDoc->document->pages[4]->width() == width2, "A1's untitled page moves out with it");
  check(!scribbleDoc->nestOutlineEntry(0, ScribbleDoc::OUTLINE_OUTDENT), "a top-level entry has no parent to leave");
  scribbleDoc->doCommand(ID_UNDO);
  expect("B@0:0 A@1:1 A1@2:2 C@4:2 B1@5:0 ", "moving out is one undo step");

  scribbleDoc->newDocument();
  return nbad;
}

// Phase 5: restyling must move the theme's own ink and nothing else, and must undo in one step.
// Layers (LAYERS_INVESTIGATION.md).  runLayerTests() in layertest.cpp covers the table's own logic;
//  everything here needs a document: the lock actually blocking the editing paths, the undo item, the
//  round trip through the document config, and the wire format.
int ScribbleTest::selectTouchingTest()
{
  int nbad = 0;
  auto check = [&](bool ok, const char* what) {
    if(!ok) { ++nbad; printf("FAIL: select touching: %s\n", what); }
  };
  auto nSelected = [&]() { return scribbleArea->currSelection ? scribbleArea->currSelection->count() : 0; };
  auto rectSelect = [&](Dim x0, Dim y0, Dim x1, Dim y1) {
    scribbleDoc->clearSelection();
    scribbleMode->setMode(MODE_SELECTRECT);
    ie(x0, y0, 0, pen, press);  ie(x1, y1, 0, pen);  ie(0, 0, 0, pen, release);
    return nSelected();
  };
  auto lassoSelect = [&](Dim x0, Dim y0, Dim x1, Dim y1) {
    scribbleDoc->clearSelection();
    scribbleMode->setMode(MODE_SELECTLASSO);
    ie(x0, y0, 0, pen, press);  ie(x1, y0, 0, pen);  ie(x1, y1, 0, pen);  ie(x0, y1, 0, pen);
    ie(x0, y0 + 1, 0, pen);  ie(0, 0, 0, pen, release);
    return nSelected();
  };

  const bool wasTouching = scribbleMode->selectTouching;
  scribbleDoc->newDocument();
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 2, ScribblePen::TIP_ROUND));
  // two straight strokes, so their outlines have no points in the middle
  scribbleMode->setMode(MODE_STROKE);
  ie(120, 160, 0, pen, press);  ie(300, 160, 0, pen);  ie(0, 0, 0, pen, release);
  ie(120, 260, 0, pen, press);  ie(300, 260, 0, pen);  ie(0, 0, 0, pen, release);
  check(scribbleArea->currPage->strokeCount() == 2, "two strokes drawn");

  scribbleMode->selectTouching = false;
  check(rectSelect(100, 140, 200, 180) == 0, "off: a rect over half a stroke selects nothing");
  check(rectSelect(100, 140, 320, 180) == 1, "off: a rect around a stroke selects it");
  check(lassoSelect(100, 140, 200, 180) == 0, "off: a lasso over half a stroke selects nothing");

  scribbleMode->selectTouching = true;
  check(rectSelect(100, 140, 200, 180) == 1, "on: a rect over half a stroke selects it");
  check(rectSelect(180, 140, 220, 280) == 2, "on: a rect crossing both strokes between their points selects both");
  check(rectSelect(100, 190, 320, 230) == 0, "on: a rect between the strokes selects nothing");
  check(lassoSelect(100, 140, 200, 180) == 1, "on: a lasso over half a stroke selects it");
  check(lassoSelect(180, 140, 220, 280) == 2, "on: a lasso crossing both strokes between their points selects both");
  check(lassoSelect(100, 190, 320, 230) == 0, "on: a lasso between the strokes selects nothing");

  scribbleDoc->clearSelection();
  scribbleMode->selectTouching = wasTouching;
  return nbad;
}

// two fingers tapped without moving undo exactly one step; two fingers that move only pan
int ScribbleTest::twoFingerTapTest()
{
  int nbad = 0;
  auto check = [&](bool ok, const char* what) {
    if(!ok) { ++nbad; printf("FAIL: two finger tap: %s\n", what); }
  };
  ScribbleInput* input = scribbleArea->scribbleInput.get();
  const inputmode_t wasSingle = input->singleTouchMode, wasMulti = input->multiTouchMode;
  input->multiTouchMode = INPUTMODE_PAN;
  for(inputmode_t singleMode : {INPUTMODE_DRAW, INPUTMODE_PAN}) {
    input->singleTouchMode = singleMode;
    const char* modeName = singleMode == INPUTMODE_DRAW ? "touch draws" : "touch pans";
    scribbleDoc->newDocument();
    scribbleMode->setMode(MODE_STROKE);
    ie(120, 160, 0, pen, press);  ie(300, 160, 0, pen);  ie(0, 0, 0, pen, release);
    ie(120, 260, 0, pen, press);  ie(300, 260, 0, pen);  ie(0, 0, 0, pen, release);
    // still tap: second finger lands, first lifts, then the second
    mtinput(INPUTEVENT_PRESS, 200, 200, INPUTEVENT_NONE, 0, 0);
    mtinput(INPUTEVENT_MOVE, 201, 200, INPUTEVENT_PRESS, 260, 200);
    mtinput(INPUTEVENT_RELEASE, 201, 201, INPUTEVENT_MOVE, 260, 201);
    mtinput(INPUTEVENT_NONE, 0, 0, INPUTEVENT_RELEASE, 260, 201);
    check(scribbleArea->currPage->strokeCount() == 1, modeName);
    // the same fingers dragged 60px pan instead
    mtinput(INPUTEVENT_PRESS, 200, 200, INPUTEVENT_NONE, 0, 0);
    mtinput(INPUTEVENT_MOVE, 200, 200, INPUTEVENT_PRESS, 260, 200);
    mtinput(INPUTEVENT_MOVE, 200, 230, INPUTEVENT_MOVE, 260, 230);
    mtinput(INPUTEVENT_MOVE, 200, 260, INPUTEVENT_MOVE, 260, 260);
    mtinput(INPUTEVENT_RELEASE, 200, 260, INPUTEVENT_MOVE, 260, 260);
    mtinput(INPUTEVENT_NONE, 0, 0, INPUTEVENT_RELEASE, 260, 260);
    check(scribbleArea->currPage->strokeCount() == 1, "a two finger drag does not undo");
  }
  input->singleTouchMode = wasSingle;
  input->multiTouchMode = wasMulti;
  return nbad;
}

int ScribbleTest::pageTagTest()
{
  int nbad = 0;
  auto check = [&](bool ok, const char* what) {
    if(!ok) { ++nbad; printf("FAIL: page tags: %s\n", what); }
  };
  ScribbleArea* area = scribbleArea;
  auto screenAt = [&](int pagenum, Point p) { return area->dimToScreen(area->getPageOrigin(pagenum) + p); };
  auto tagsOn = [&](int pagenum) { return area->page(pagenum)->pageTagIds.size(); };
  // every tag element must lie inside the page it is on, or no thumbnail shows it and the pulse misses it
  auto allTagsInside = [&]() {
    for(int pagenum = 0; pagenum < area->numPages(); ++pagenum) {
      Page* page = area->page(pagenum);
      for(Element* s : page->children()) {
        if(s->isPageTag() && !page->rect().contains(s->bbox()))
          return false;
      }
    }
    return true;
  };
  // pick up one tag and press/release with the pen at a point given in a page's coordinates
  auto placeAt = [&](int pagenum, Point p) {
    area->startTagPlacement({{"t1", "homework"}});
    Point pos = screenAt(pagenum, p);
    ie(pos.x, pos.y, 0, pen, press);  ie(0, 0, 0, pen, release);
  };

  scribbleDoc->newDocument();
  scribbleDoc->newPage();
  area->gotoPage(0);
  Page* first = area->page(0);
  Page* last = area->page(1);

  placeAt(0, Point(first->width()/2, first->height() + area->pageSpacing/2));
  check(tagsOn(0) + tagsOn(1) == 1, "a press in the gap between pages places the tag");
  check(allTagsInside(), "a press in the gap between pages keeps the tag on a page");
  check(!area->placingTags(), "the tag is no longer on the pointer once placed");
  placeAt(0, Point(first->width() + 150, first->height()/2));
  check(allTagsInside(), "a press beside the page keeps the tag on it");
  placeAt(1, Point(last->width()/2, last->height() + 400));
  check(tagsOn(1) >= 1 && allTagsInside(), "a press below the last page puts the tag on the last page");

  // a pen had no preview, so the tag comes up selected, ready to be dragged into place
  scribbleDoc->newDocument();
  scribbleDoc->newPage();
  const Dim prevZoom = area->getZoom();
  area->setZoom(0.35);  // both pages on screen, so the drag below can reach the second
  area->gotoPage(0);
  placeAt(0, Point(200, 200));
  check(area->currSelection && area->currSelection->count() == 1, "a pen placement leaves the tag selected");

  // dragging the selected tag onto the next page moves it there, and undo brings it back
  Point from = screenAt(0, Point(200, 200));
  Point to = screenAt(1, Point(200, 200));
  ie(from.x, from.y, 0, pen, press);  ie((from.x + to.x)/2, (from.y + to.y)/2, 0, pen);  ie(to.x, to.y, 0, pen);
  ie(0, 0, 0, pen, release);
  check(tagsOn(0) == 0 && tagsOn(1) == 1, "a tag dragged to the next page is counted there and not on the first");
  check(allTagsInside(), "a tag dragged to the next page lands inside it");
  scribbleDoc->clearSelection();
  scribbleDoc->doCommand(ID_UNDO);
  check(tagsOn(0) == 1 && tagsOn(1) == 0, "undoing the move puts the tag back on the first page");
  scribbleDoc->doCommand(ID_REDO);
  check(tagsOn(0) == 0 && tagsOn(1) == 1, "redoing the move puts it on the next page again");

  // a to-do tag: ticking its box keeps it on the page but takes the tag off the page, unticking puts it back
  scribbleDoc->newDocument();
  area->gotoPage(0);
  scribbleMode->setMode(MODE_STROKE);  // a tap on the box ticks it rather than drawing a dot
  area->startTagPlacement({{"t2", "chores"}}, true);
  Point placePos = screenAt(0, Point(300, 300));
  ie(placePos.x, placePos.y, 0, pen, press);  ie(0, 0, 0, pen, release);
  scribbleDoc->clearSelection();
  auto todoTag = [&]() -> Element* {
    for(Element* s : area->page(0)->children()) { if(s->isTodoTag()) return s; }
    return NULL;
  };
  auto elementCount = [&]() { size_t count = 0; for(Element* s : area->page(0)->children()) { (void)s; ++count; } return count; };
  auto tapTodoBox = [&]() {
    if(!todoTag())
      return;  // already reported
    Point boxPos = screenAt(0, todoTag()->todoBoxRect().center());
    ie(boxPos.x, boxPos.y, 0, pen, press);  ie(boxPos.x, boxPos.y, 0, pen, release);
  };
  check(todoTag() && !todoTag()->isTodoDone() && tagsOn(0) == 1, "an open to-do tag tags its page");
  tapTodoBox();
  check(todoTag() && todoTag()->isTodoDone(), "tapping the box ticks the to-do and leaves it on the page");
  check(tagsOn(0) == 0, "a ticked to-do no longer tags its page");
  check(elementCount() == 1, "tapping the box draws nothing");
  scribbleDoc->doCommand(ID_UNDO);
  check(todoTag() && !todoTag()->isTodoDone() && tagsOn(0) == 1, "undo unticks the to-do and tags the page again");
  scribbleDoc->doCommand(ID_REDO);
  check(todoTag() && todoTag()->isTodoDone() && tagsOn(0) == 0, "redo ticks it again");
  ScribbleDoc::renamePageTagElements(scribbleDoc->document, "t2", "errands");
  check(todoTag() && todoTag()->isTodoDone() && tagsOn(0) == 0, "renaming the tag keeps the to-do ticked");
  tapTodoBox();
  check(todoTag() && !todoTag()->isTodoDone() && tagsOn(0) == 1, "tapping a ticked box unticks it and tags the page");

  // tags still on the pointer belong to the notebook they were picked for
  area->startTagPlacement({{"t1", "homework"}});
  scribbleDoc->newDocument();
  check(!area->placingTags(), "opening another document drops tags still on the pointer");

  scribbleDoc->clearSelection();
  area->setZoom(prevZoom);
  return nbad;
}

int ScribbleTest::layerTest()
{
  int nbad = 0;
  auto check = [&](bool ok, const char* what) {
    if(!ok) { ++nbad; printf("FAIL: layer: %s\n", what); }
  };
  auto drawStroke = [&](Dim y) {
    scribbleMode->setMode(MODE_STROKE);
    ie(120, y, 0, pen, press);  ie(300, y, 0, pen);  ie(0, 0, 0, pen, release);
  };
  auto elementAt = [&](int idx) -> Element* {
    int ii = 0;
    for(Element* s : scribbleArea->currPage->children()) { if(ii++ == idx) return s; }
    return NULL;
  };

  scribbleDoc->newDocument();
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 2, ScribblePen::TIP_ROUND));
  const int base = scribbleDoc->currentLayer();
  check(scribbleDoc->layers().size() == 1, "a fresh document has exactly one layer");

  drawStroke(160);
  check(elementAt(0) && elementAt(0)->layer() == base, "a stroke lands on the current layer");

  int top = scribbleDoc->addLayer("Top");
  check(scribbleDoc->layers().size() == 2, "a layer was added");
  check(scribbleDoc->setCurrentLayer(top), "the new layer can be made current");
  drawStroke(360);
  check(elementAt(1) && elementAt(1)->layer() == top, "a stroke lands on the new current layer");
  // an image is added by the same route (Clipboard::paste -> Page::addStroke), which is what makes
  //  "new content goes on the current layer" one rule rather than one per creation site
  scribbleArea->insertImage(Image(8, 8));
  Element* img = NULL;
  for(Element* s : scribbleArea->currPage->children()) {
    if(s->node->type() == SvgNode::IMAGE) img = s;
  }
  check(img != NULL, "an image was inserted");
  check(img && img->layer() == top, "an inserted image lands on the current layer too");

  // Layers stack: a stroke drawn on the lower layer goes *under* what is already on the upper one,
  //  even though it was drawn later.  Without this a layer would be only a tag, and moving content
  //  between layers would not change what covers what.
  // insertImage() leaves the image selected, and a press inside a selection moves it rather than
  //  drawing - so the selection has to go before the next stroke
  scribbleDoc->clearSelection();
  check(scribbleDoc->setCurrentLayer(base), "back to the lower layer");
  drawStroke(260);
  check(elementAt(0) && elementAt(0)->layer() == base
      && elementAt(1) && elementAt(1)->layer() == base,
      "a stroke drawn later on the lower layer is placed under the upper layer's content");
  check(elementAt(2) && elementAt(2)->layer() == top, "...which is still above it");
  // undo has to take it back out again, or the rest of this test counts the wrong elements
  scribbleDoc->doUndoRedo(false);
  check(elementAt(1) && elementAt(1)->layer() == top, "undoing that stroke leaves the rest in order");

  // *** the requirement: a locked layer is immune to selection and to erasing from other layers ***
  // the pen has to be on another layer first - the current layer is editable even while locked
  check(scribbleDoc->setCurrentLayer(top), "the pen is on the upper layer");
  check(scribbleDoc->setLayerLocked(base, true), "locking the lower layer succeeds");
  scribbleDoc->doCommand(ID_SELALL);
  int nsel = scribbleArea->currSelection ? scribbleArea->currSelection->count() : 0;
  check(nsel == 2, "Select All skips the locked layer and takes only the two unlocked elements");
  scribbleDoc->clearSelection();

  // a rect selection dragged right over the locked stroke must not pick it up either
  scribbleMode->setMode(MODE_SELECTRECT);
  ie(100, 140, 0, pen, press);  ie(320, 180, 0, pen);  ie(0, 0, 0, pen, release);
  check(!scribbleArea->currSelection || scribbleArea->currSelection->count() == 0,
      "a rect selection over a locked stroke selects nothing");
  scribbleDoc->clearSelection();

  int nstrokes = scribbleArea->currPage->strokeCount();
  // the stroke eraser is a selection, so it is covered by the same gate...
  scribbleMode->setMode(MODE_ERASESTROKE);
  ie(120, 160, 0, pen, press);  ie(300, 160, 0, pen);  ie(0, 0, 0, pen, release);
  check(scribbleArea->currPage->strokeCount() == nstrokes,
      "the stroke eraser cannot erase a stroke on a locked layer");
  // ...the free eraser is not, and needs its own check, because it does not build a Selection
  scribbleMode->setMode(MODE_ERASEFREE);
  ie(120, 160, 0, pen, press);  ie(300, 160, 0, pen);  ie(0, 0, 0, pen, release);
  check(scribbleArea->currPage->strokeCount() == nstrokes,
      "the free eraser cannot erase a stroke on a locked layer");
  check(elementAt(0) && elementAt(0)->layer() == base,
      "...and the locked stroke is still there, on its own layer");

  // Picking the locked layer is how it is edited: once current, it is selectable like any other...
  check(scribbleDoc->setCurrentLayer(base), "a locked layer can be picked as the current layer");
  scribbleDoc->doCommand(ID_SELALL);
  nsel = scribbleArea->currSelection ? scribbleArea->currSelection->count() : 0;
  check(nsel == 3, "Select All on the locked current layer takes its stroke as well");
  // ...and leaving it puts its ink back out of reach, including anything that was selected
  check(scribbleDoc->setCurrentLayer(top), "picking another layer succeeds");
  check(!scribbleArea->currSelection || scribbleArea->currSelection->count() == 0,
      "leaving a locked layer lets go of the selection that held its ink");
  scribbleDoc->doCommand(ID_SELALL);
  nsel = scribbleArea->currSelection ? scribbleArea->currSelection->count() : 0;
  check(nsel == 2, "...and Select All skips it again");
  scribbleDoc->clearSelection();

  // locking the layer the pen is on leaves the pen there, still able to write
  check(scribbleDoc->setLayerLocked(top, true), "locking the current layer succeeds");
  check(scribbleDoc->currentLayer() == top, "locking the current layer does not move the pen");
  check(scribbleDoc->layers().isEditable(top), "...and the current layer stays editable");
  scribbleDoc->setLayerLocked(top, false);
  scribbleDoc->setLayerLocked(base, false);

  // Moving an element between layers is one undoable action, and it restacks.  A *second* element on
  //  the lower layer is what gives this check teeth: with only one, the element being restored is the
  //  last of its layer's run either way, so recomputing the insertion point from the layer table
  //  happens to land in the right place and an undo item that dropped the recorded sibling position
  //  would still pass.  Moving the *first* of two makes the two answers differ.
  scribbleDoc->setCurrentLayer(base);
  drawStroke(260);
  scribbleMode->setMode(MODE_SELECTRECT);
  ie(100, 140, 0, pen, press);  ie(320, 180, 0, pen);  ie(0, 0, 0, pen, release);
  check(scribbleArea->currSelection && scribbleArea->currSelection->count() == 1,
      "just the first lower-layer stroke is selected");
  size_t undoBefore = scribbleDoc->history->undoSteps();
  Element* lower = elementAt(0);
  check(elementAt(1) && elementAt(1)->layer() == base, "there is a second element on that layer");
  int nmoved = scribbleDoc->moveSelToLayer(top);
  check(nmoved == 1, "exactly the one element not already on the target layer moves");
  check(lower->layer() == top, "the moved element reports the new layer");
  check(scribbleDoc->history->undoSteps() == undoBefore + 1, "the move is a single undo step");
  scribbleDoc->clearSelection();
  scribbleDoc->doUndoRedo(false);
  check(lower->layer() == base, "undo restores the element's layer");
  // the z-order half: a layer change restacks, so undoing only the id would leave the element
  //  somewhere the user never put it
  check(elementAt(0) == lower, "undo restores the element's position in z-order too");
  // undo the extra stroke as well, so what follows counts the three elements it expects
  scribbleDoc->doUndoRedo(false);
  check(scribbleArea->currPage->strokeCount() == 3, "back to one element per layer plus the image");

  // a hidden layer is not drawn and not editable
  check(scribbleDoc->setLayerHidden(top, true), "hiding a layer succeeds");
  check(elementAt(1) && elementAt(1)->node->displayMode() == SvgNode::NoneMode,
      "an element on a hidden layer is not drawn");
  check(elementAt(0)->node->displayMode() != SvgNode::NoneMode,
      "...while one on a visible layer still is");
  scribbleDoc->doCommand(ID_SELALL);
  check(scribbleArea->currSelection && scribbleArea->currSelection->count() == 1,
      "Select All skips a hidden layer");
  scribbleDoc->clearSelection();
  scribbleDoc->setLayerHidden(top, false);
  check(elementAt(1) && elementAt(1)->node->displayMode() != SvgNode::NoneMode,
      "unhiding puts it back");

  // reordering the layers restacks the page without touching a single element's layer id
  int lowerLayer = elementAt(0)->layer();
  check(scribbleDoc->moveLayer(1, 0), "moving the top layer to the bottom succeeds");
  check(elementAt(0)->layer() != lowerLayer,
      "the element that was at the bottom is no longer first after a reorder");
  check(scribbleDoc->layers().zIndexOf(lowerLayer) == 1, "...because its layer moved, not its id");
  scribbleDoc->moveLayer(1, 0);  // put it back

  // save and reload: the table rides the document config, the ids ride the elements.  The pen goes to
  //  the other layer first, since the current layer is editable even while locked.
  scribbleDoc->setCurrentLayer(base);
  scribbleDoc->setLayerLocked(top, true);
  scribbleDoc->setLayerName(top, "Ink, notes; 100%");
  LayerList saved = scribbleDoc->layers();
  std::string file = outPath + "/layer_roundtrip_out.html";
  check(scribbleDoc->saveDocument(file.c_str()), "saving the document should succeed");
  scribbleDoc->newDocument();
  check(scribbleDoc->openDocument(file.c_str()) == Document::LOAD_OK, "reloading should succeed");
  removeFile(file.c_str());

  check(scribbleDoc->layers() == saved, "the whole layer table survives save/reload");
  check(scribbleDoc->layers().isLocked(top), "the locked flag survives save/reload");
  check(scribbleDoc->layers().find(top)
      && scribbleDoc->layers().find(top)->name == "Ink, notes; 100%",
      "a name with the serializer's separators in it survives save/reload");
  check(elementAt(0) && elementAt(0)->layer() == base, "an element keeps its layer across a reload");
  check(elementAt(1) && elementAt(1)->layer() == top, "...and so does one on the other layer");
  // and the lock still bites after a reload, which is the point of saving it at all
  scribbleDoc->doCommand(ID_SELALL);
  check(scribbleArea->currSelection && scribbleArea->currSelection->count() == 1,
      "a layer locked before saving is still locked after reloading");
  scribbleDoc->clearSelection();

  // A document written before layers existed has no table and no __layer attributes; it must read
  //  back as one unlocked layer holding everything, with nothing to migrate.
  {
    Page* page = scribbleArea->currPage;
    LayerList none;
    scribbleDoc->document->layers = none;
    check(page->isEditable(elementAt(0)), "an element on an unknown layer is still editable");
    check(page->isEditable(elementAt(1)), "...whichever unknown layer it is on");
  }

  // the sync wire format: undo items *are* the sync protocol, so the item has to parse back
  {
    scribbleDoc->newDocument();
    scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 2, ScribblePen::TIP_ROUND));
    drawStroke(160);
    Element* s = elementAt(0);
    s->uuid = 12345;
    MemStream strm;
    StrokeLayerItem item(s, scribbleArea->currPage, 7);
    item.serialize(strm);
    std::string wire(strm.data(), strm.size());
    pugi::xml_document wiredoc;
    check(wiredoc.load_buffer(wire.data(), wire.size()),
        "the serialized layer item is well-formed XML");
    pugi::xml_node n = wiredoc.child("layerchanged");
    check(!n.empty(), "the item serializes as <layerchanged>");
    check(n.attribute("strokeuuid").as_ullong() == 12345, "the wire format carries the stroke uuid");
    // serialize() reports the element's *current* layer, which is what a peer has to apply
    check(n.attribute("layer").as_int(-1) == s->layer(), "the wire format carries the layer");
  }
  return nbad;
}

// The layer table and the theme as undo steps and on the sync wire.  Undo items *are* the sync
//  protocol, so each edit is checked three ways: it is one undo step, undo and redo restore it, and
//  what it serializes reproduces the edit when fed back through ScribbleSync::processItem() - the
//  path a peer's client runs - after the edit has been undone locally.
int ScribbleTest::docStateSyncTest()
{
  int nbad = 0;
  auto check = [&](bool ok, const char* what) {
    if(!ok) { ++nbad; printf("FAIL: doc state sync: %s\n", what); }
  };
  auto drawStroke = [&](Dim y) {
    scribbleMode->setMode(MODE_STROKE);
    ie(120, y, 0, pen, press);  ie(300, y, 0, pen);  ie(0, 0, 0, pen, release);
  };
  auto elementAt = [&](int idx) -> Element* {
    int ii = 0;
    for(Element* s : scribbleArea->currPage->children()) { if(ii++ == idx) return s; }
    return NULL;
  };
  auto steps = [&]() { return scribbleDoc->history->undoSteps(); };
  auto undo = [&]() { scribbleDoc->doUndoRedo(false); };
  auto redo = [&]() { scribbleDoc->doUndoRedo(true); };

  scribbleDoc->newDocument();
  // after newDocument(), which replaces the Document this refers into
  const LayerList& layers = scribbleDoc->layers();
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 2, ScribblePen::TIP_ROUND));
  const int base = scribbleDoc->currentLayer();
  drawStroke(160);

  // --- every table edit is one undo step, and undo/redo restore it ---
  size_t before = steps();
  int top = scribbleDoc->addLayer("Top");
  check(steps() == before + 1, "adding a layer is one undo step");
  undo();
  check(!layers.find(top), "undo removes an added layer");
  redo();
  check(layers.find(top) && layers.find(top)->name == "Top", "redo restores it, with the same id");
  scribbleDoc->setCurrentLayer(top);
  drawStroke(360);
  Element* onTop = elementAt(1);
  check(onTop && onTop->layer() == top, "a stroke is on the new layer");
  scribbleDoc->setCurrentLayer(base);

  before = steps();
  check(scribbleDoc->setLayerName(top, "Renamed"), "renaming succeeds");
  check(steps() == before + 1, "a rename is one undo step");
  check(!scribbleDoc->setLayerName(top, "Renamed") && steps() == before + 1,
      "renaming to the same name is refused and adds no step");
  undo();
  check(layers.find(top)->name == "Top", "undo restores the old name");
  redo();
  check(layers.find(top)->name == "Renamed", "redo renames again");

  scribbleDoc->setLayerLocked(top, true);
  undo();
  check(!layers.isLocked(top), "undo unlocks");
  redo();
  check(layers.isLocked(top), "redo locks again");
  undo();

  scribbleDoc->setLayerHidden(top, true);
  check(onTop->node->displayMode() == SvgNode::NoneMode, "hiding hides the layer's stroke");
  undo();
  check(!layers.isHidden(top) && onTop->node->displayMode() != SvgNode::NoneMode,
      "undo unhides the layer, and its stroke is drawn again");

  scribbleDoc->moveLayer(1, 0);
  check(elementAt(0) == onTop, "moving the layer to the bottom restacks its stroke");
  undo();
  check(elementAt(1) == onTop && layers.zIndexOf(top) == 1, "undo puts the layer and its stroke back on top");

  // Removal moves the layer's content to another layer and removes the entry, as one step; undo has
  //  to bring back the layer *before* the elements are moved back onto it
  before = steps();
  check(scribbleDoc->removeLayer(top), "removing a layer with content succeeds");
  check(steps() == before + 1, "removing a layer, content and all, is one undo step");
  check(!layers.find(top) && onTop->layer() == base, "its content moved to the remaining layer");
  undo();
  check(layers.find(top) && onTop->layer() == top, "undo restores the layer and its content's layer");
  check(layers.zIndexOf(top) == 1 && elementAt(1) == onTop, "...in the same place in the stack");

  // --- the receive path: what an edit serializes, applied as a peer would apply it ---
  std::unique_ptr<ScribbleSync> sync(new ScribbleSync(scribbleDoc));
  auto receive = [&](const std::string& wire) {
    std::string xml = "<undo uuid='77' user='peer'>" + wire + "</undo>";
    pugi::xml_document doc;
    if(!doc.load_buffer(xml.data(), xml.size())) return false;
    pugi::xml_node item = doc.child("undo").first_child();
    sync->processItem(item);
    return true;
  };
  auto layerWire = [&](int id) {
    MemStream strm;
    LayerTableItem(scribbleDoc, id).serialize(strm);  // reports the live state, as sendHist relies on
    return std::string(strm.data(), strm.size());
  };

  // a name with every XML-significant character, since it is user text on the wire
  const char* odd = "Peer & 'quoted' <name>";
  scribbleDoc->setLayerName(top, odd);
  scribbleDoc->setLayerLocked(top, true);
  std::string wire = layerWire(top);
  undo();  undo();
  check(layers.find(top)->name == "Renamed" && !layers.isLocked(top), "the local edits are undone");
  before = steps();
  check(receive(wire), "a layer table item is well-formed XML");
  check(layers.find(top) && layers.find(top)->name == odd, "a received rename applies, escaping and all");
  check(layers.isLocked(top), "a received lock applies");
  check(steps() == before, "a received edit is not added to the local undo history");
  scribbleDoc->setLayerLocked(top, false);

  // a peer's new layer arrives at the same place in the stack; its removal takes it away again
  int added = scribbleDoc->addLayer("Peer layer", 0);  // directly above the bottom layer
  wire = layerWire(added);
  undo();
  check(!layers.find(added), "the local add is undone");
  receive(wire);
  check(layers.find(added) && layers.zIndexOf(added) == 1, "a received layer lands at the same z position");
  scribbleDoc->removeLayer(added);
  wire = layerWire(added);
  undo();
  receive(wire);
  check(!layers.find(added), "a received removal removes the layer");

  // A peer hiding the layer our pen is on moves the pen: new ink has to land somewhere visible.  Our
  //  current layer is otherwise not part of the wire format.
  scribbleDoc->setCurrentLayer(top);
  scribbleDoc->setLayerHidden(top, true);
  wire = layerWire(top);
  undo();
  scribbleDoc->setCurrentLayer(top);
  receive(wire);
  check(layers.isHidden(top) && scribbleDoc->currentLayer() != top,
      "a peer hiding our current layer moves the pen off it");

  // A stroke a peer draws on a layer we have hidden must arrive hidden.  Visibility is otherwise only
  //  applied when the table changes or a page loads, so it used to arrive drawn.
  scribbleDoc->setLayerHidden(top, false);
  scribbleDoc->setCurrentLayer(top);
  drawStroke(460);
  Element* peerStroke = elementAt(2);
  MemStream strokeStrm;
  StrokeAddedItem(peerStroke, scribbleArea->currPage, NULL).serialize(strokeStrm);
  std::string strokeWire(strokeStrm.data(), strokeStrm.size());
  undo();  // the stroke, which the peer now "draws"
  scribbleDoc->setCurrentLayer(base);
  scribbleDoc->setLayerHidden(top, true);
  int nbefore = scribbleArea->currPage->strokeCount();
  receive(strokeWire);
  Element* arrived = scribbleArea->currPage->strokeCount() == nbefore + 1 ? elementAt(2) : NULL;
  check(arrived && arrived->layer() == top, "the peer's stroke arrives on the peer's layer");
  check(arrived && arrived->node->displayMode() == SvgNode::NoneMode,
      "a peer's stroke on a layer we have hidden arrives hidden");
  undo();  // our hide

  // --- the joiner snapshot: the whole table, keeping the joiner's own current layer ---
  {
    LayerList table;
    table.setName(table.layers[0].id, "Snap base");
    int locked = table.addLayer("Snap locked");
    table.setLocked(locked, true);
    table.addLayer("Snap top");
    XmlStreamWriter xmlwriter;
    xmlwriter.writeStartElement("layersnapshot");
    xmlwriter.writeAttribute("table", table.serialize());
    xmlwriter.writeEndElement();
    MemStream strm;
    xmlwriter.save(strm);
    receive(std::string(strm.data(), strm.size()));
    check(layers.serialize() == table.serialize(), "a received snapshot replaces the whole table");
    check(layers.isEditable(scribbleDoc->currentLayer()), "...and leaves the pen on a layer that takes ink");
  }

  // --- the theme ---
  PaletteRecipe themeA;
  themeA.gen = "cusp-walk-1";
  themeA.seedHue = 200;
  PaletteRecipe themeB = themeA;
  themeB.seedHue = 40;
  scribbleDoc->setTheme(themeA, true, false);
  Color inkA = scribbleDoc->palette().families[2].base;
  before = steps();
  scribbleDoc->setTheme(themeB, true, false);
  Color inkB = scribbleDoc->palette().families[2].base;
  check(!(inkA == inkB), "the two themes differ");
  check(steps() == before + 1, "a theme change is one undo step, pages and all");
  MemStream themeStrm;
  ThemeChangedItem(scribbleDoc).serialize(themeStrm);
  undo();
  check(scribbleDoc->palette().families[2].base == inkA, "undo of a theme change restores the recipe");
  check(scribbleArea->currPage->props.color == scribbleDoc->palette().paper,
      "...and the paper with it, in the same step");
  receive(std::string(themeStrm.data(), themeStrm.size()));
  check(scribbleDoc->palette().families[2].base == inkB, "a received theme applies the peer's recipe exactly");
  check(steps() == before, "...without adding to the local undo history");

  sync.reset();
  scribbleDoc->newDocument();
  return nbad;
}

int ScribbleTest::restyleTest()
{
  int nbad = 0;
  auto check = [&](bool ok, const char* what) {
    if(!ok) { ++nbad; printf("FAIL: restyle: %s\n", what); }
  };

  scribbleDoc->newDocument();
  PaletteRecipe themeA;
  themeA.gen = "cusp-walk-1";
  themeA.seedHue = 200;
  scribbleDoc->setTheme(themeA, true, false);

  // one stroke in a palette color, one in a color the palette cannot produce
  Palette palA = scribbleDoc->palette();
  check(palA.families.size() >= 3, "the theme should have families to draw with");
  Color inkA = palA.families[2].base;
  const Color offPalette(0x12, 0x34, 0x56);

  // Four strokes, one per behaviour the mapping has to get right: a family base, a *dark variant*
  //  (which must not collapse to the base), the *neutral* (which must stay neutral rather than being
  //  mapped by ordinal like a family), and an off-palette color (which must not move at all).
  Color darkA = palA.families[2].dark;
  Color neutralA = palA.neutral;
  scribbleMode->setMode(MODE_STROKE);
  scribbleDoc->app->setPen(ScribblePen(inkA, 2, ScribblePen::TIP_ROUND));
  ie(120, 160, 0, pen, press);  ie(200, 200, 0, pen);  ie(0, 0, 0, pen, release);
  scribbleDoc->app->setPen(ScribblePen(offPalette, 2, ScribblePen::TIP_ROUND));
  ie(120, 260, 0, pen, press);  ie(200, 300, 0, pen);  ie(0, 0, 0, pen, release);
  scribbleDoc->app->setPen(ScribblePen(darkA, 2, ScribblePen::TIP_ROUND));
  ie(120, 360, 0, pen, press);  ie(200, 400, 0, pen);  ie(0, 0, 0, pen, release);
  scribbleDoc->app->setPen(ScribblePen(neutralA, 2, ScribblePen::TIP_ROUND));
  ie(120, 460, 0, pen, press);  ie(200, 500, 0, pen);  ie(0, 0, 0, pen, release);

  Page* page = scribbleArea->currPage;
  check(page->strokeCount() == 4, "four strokes should have been drawn");
  if(page->strokeCount() != 4)
    return nbad;
  auto colorAt = [&](int idx) {
    int ii = 0;
    for(Element* s : page->children()) { if(ii++ == idx) return s->getProperties().color; }
    return Color(Color::INVALID_COLOR);
  };
  check(colorAt(0) == inkA, "the first stroke should be the theme's ink");
  check(colorAt(1) == offPalette, "the second stroke should be the off-palette color");

  size_t undoBefore = scribbleDoc->history->undoSteps();

  PaletteRecipe themeB = themeA;
  themeB.seedHue = 40;
  int nchanged = scribbleDoc->restyleToTheme(themeB, true, false);

  const Palette& palB = scribbleDoc->palette();
  // base and dark move; the neutral is unchanged by definition, so it is not counted
  check(nchanged == 2, "exactly the two non-neutral palette strokes should have been restyled");

  Color expected;
  check(palB.mapFrom(palA, inkA, &expected), "the old ink should map into theme B");
  check(colorAt(0) == expected, "the themed stroke takes theme B's matching color");
  check(!(colorAt(0) == inkA), "...and is actually different from what it was");

  // The off-palette stroke is untouched.  This is the half that protects imported PDFs and deliberate
  //  custom colors, and the one a careless "recolor everything" would break.
  check(colorAt(1) == offPalette, "an off-palette stroke must not be touched by a restyle");

  // A dark variant must land on theme B's *dark*, not on its base - otherwise every emphasis stroke
  //  in the document silently flattens into ordinary ink.
  int famB = -2, varB = -2;
  check(palB.indexOf(colorAt(2), &famB, &varB), "the restyled dark stroke is still a palette color");
  check(varB == PALETTE_DARK, "a dark variant must restyle to a dark variant, not to the base");

  // The neutral is exempt from the walk in both palettes, so mapping it by ordinal like a family
  //  would turn black-on-white into a colored ink - the one thing a neutral exists to prevent.
  check(colorAt(3) == palB.neutral, "the neutral maps to the neutral");
  check(colorAt(3) == neutralA, "...which for light paper means black stays black");

  // one action, so one Ctrl+Z puts every stroke back
  check(scribbleDoc->history->undoSteps() == undoBefore + 1,
      "a restyle is a single undo step, however many strokes it moved");
  scribbleDoc->doUndoRedo(false);
  check(colorAt(0) == inkA, "undoing a restyle restores the original ink");
  check(colorAt(1) == offPalette, "undo leaves the untouched stroke untouched");
  check(colorAt(2) == darkA, "undo restores the dark variant too");
  // ...and the recipe with them (ThemeChangedItem).  Without it the document is left holding theme B's
  //  recipe and theme A's ink, and the next restyle finds nothing it recognises.  Compared through the
  //  generated palette rather than the recipe, since the recipe is read back through float.
  check(scribbleDoc->palette().families[2].base == inkA, "undoing a restyle restores the theme's recipe too");

  // COLORS_SPEC.md §10.1: inverting a themed document mirrors its paper rather than XOR-ing to a
  //  negative, so inverting twice must land exactly back where it started - ink, paper and all.
  //  A lossy mirror would be invisible until someone toggled it twice and found their colors drifted.
  {
    // no setTheme(themeA) here: the undo above has to have left the document on theme A by itself,
    //  and the restyles below find nothing to map if it did not
    PaletteRecipe lightA = scribbleDoc->cfg->themeRecipe();
    Color inkBefore = colorAt(0), offBefore = colorAt(1), paperBefore = scribbleDoc->palette().paper;

    PaletteRecipe dark = lightA;
    dark.paperL = 1 - dark.paperL;
    scribbleDoc->restyleToTheme(dark, true, false);
    check(scribbleDoc->palette().isDarkPaper(), "mirroring the paper gives a dark-paper theme");
    check(!(colorAt(0) == inkBefore), "the ink actually changes on the dark theme");

    PaletteRecipe back = scribbleDoc->cfg->themeRecipe();
    back.paperL = 1 - back.paperL;
    scribbleDoc->restyleToTheme(back, true, false);
    check(colorAt(0) == inkBefore, "inverting twice restores the ink exactly");
    check(colorAt(1) == offBefore, "...and still never touches the off-palette stroke");
    check(scribbleDoc->palette().paper == paperBefore, "...and restores the paper exactly");
  }

  scribbleDoc->newDocument();
  return nbad;
}

// Dashed and dotted lines, as set from the selection's width popup.  A style is sized by each element's
//  own width; a filled pen stroke - the default pen, which stroke-dasharray alone cannot reach - is
//  redrawn as a stroked centreline to take one; the pattern scales with the width; and undo puts back
//  exactly what was there, geometry included.
int ScribbleTest::dashStyleTest()
{
  int nbad = 0;
  auto check = [&](bool ok, const char* what) {
    if(!ok) { ++nbad; printf("FAIL: dash style: %s\n", what); }
  };
  // the first two numbers of an element's stroke-dasharray, 0 for none
  auto dashesOf = [](Element* s, Dim* dash, Dim* gap) {
    std::string str = s->getProperties().dashArray;
    char* end = NULL;
    *dash = str == "none" ? 0 : std::strtod(str.c_str(), &end);
    *gap = str == "none" ? 0 : std::strtod(end, NULL);
  };
  auto near = [](Dim a, Dim b) { return std::abs(a - b) < 1E-3*std::max(Dim(1), std::abs(b)); };

  scribbleDoc->newDocument();
  scribbleMode->setMode(MODE_STROKE);
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 4, ScribblePen::TIP_FLAT | ScribblePen::WIDTH_PR));
  ie(120, 160, 1, pen, press);  ie(160, 180, 1, pen);  ie(200, 160, 1, pen);  ie(0, 0, 0, pen, release);
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 3));
  ie(120, 260, 0, pen, press);  ie(200, 300, 0, pen);  ie(0, 0, 0, pen, release);

  Page* page = scribbleArea->currPage;
  check(page->strokeCount() == 2, "two strokes should have been drawn");
  if(page->strokeCount() != 2)
    return nbad;
  auto elementAt = [&](int idx) {
    int ii = 0;
    for(Element* s : page->children()) { if(ii++ == idx) return s; }
    return (Element*)NULL;
  };
  check(elementAt(0)->isFilledPenStroke(), "a pressure pen draws a filled outline");
  check(!elementAt(1)->isFilledPenStroke(), "a plain pen draws a stroked path");
  std::string filledPath = elementAt(0)->getProperties().dashArray;  // "none"
  Rect filledBox = elementAt(0)->bbox();
  size_t undoBefore = scribbleDoc->history->undoSteps();

  scribbleArea->selectAll();
  StrokeProperties dotted(Color::INVALID_COLOR, -1);
  dotted.dashStyle = ScribblePen::DASH_DOTTED;
  scribbleArea->setStrokeProperties(dotted);
  check(scribbleDoc->history->undoSteps() == undoBefore + 1, "a line style is a single undo step");

  Element* conv = elementAt(0);
  Element* plain = elementAt(1);
  check(conv && !conv->isFilledPenStroke(), "a dotted style redraws a filled stroke as a stroked one");
  check(conv && conv->node->getColorAttr("stroke", Color::NONE) != Color::NONE, "...which has a stroke");
  check(conv && conv->getProperties().dashStyle == ScribblePen::DASH_DOTTED, "...and is dotted");
  check(conv && conv->bbox().intersects(filledBox), "...along the path it had");
  Dim dash, gap, width = plain->node->getFloatAttr("stroke-width", 0);
  dashesOf(plain, &dash, &gap);
  check(plain->getProperties().dashStyle == ScribblePen::DASH_DOTTED, "a stroked path takes the style directly");
  // each element's pattern follows its own width (ScribblePen::dashFor)
  check(near(dash, 0.1*width) && near(gap, 2*width), "a dotted pattern is sized by the element's width");
  check(plain->node->getIntAttr("stroke-linecap", -1) == Painter::RoundCap, "dots need a round cap");
  int selDash = -2;
  scribbleArea->getPenForSelection(&selDash);
  check(selDash == ScribblePen::DASH_DOTTED, "the selection reports its line style");

  // the pattern is part of how thick the line looks, so doubling the width doubles it
  StrokeProperties wider(Color::INVALID_COLOR, 2*width);
  scribbleArea->setStrokeProperties(wider);
  Dim dash2, gap2;
  dashesOf(plain, &dash2, &gap2);
  check(near(dash2, 2*dash) && near(gap2, 2*gap), "the pattern scales with the width");
  check(plain->getProperties().dashStyle == ScribblePen::DASH_DOTTED, "...so the style is unchanged");

  scribbleDoc->doUndoRedo(false);
  dashesOf(plain, &dash2, &gap2);
  check(near(dash2, dash) && near(gap2, gap), "undoing the width restores the pattern exactly");
  scribbleDoc->doUndoRedo(false);
  check(elementAt(0)->isFilledPenStroke(), "undoing the style restores the filled stroke");
  check(elementAt(0)->getProperties().dashArray == filledPath, "...unchanged");
  check(elementAt(1)->getProperties().dashArray == "none", "...and the stroked path's solid line");
  check(elementAt(1)->getProperties().dashStyle == ScribblePen::DASH_SOLID, "...which reads as solid");

  // one dashed, one solid: the selection has no one style, so the popup must show none checked
  scribbleDoc->doUndoRedo(true);
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 3));
  scribbleMode->setMode(MODE_STROKE);
  ie(120, 360, 0, pen, press);  ie(200, 400, 0, pen);  ie(0, 0, 0, pen, release);
  scribbleArea->selectAll();
  scribbleArea->getPenForSelection(&selDash);
  check(selDash == ScribblePen::DASH_MIXED, "a selection of several styles reports none");

  scribbleDoc->newDocument();
  return nbad;
}

// The curve fit exists to compensate for input that arrives on a whole-pixel lattice (CLAUDE.md,
// "Curve fitting the input").  This drives the real input path with samples taken off a known circular
// arc and *rounded to whole units* - which is exactly what the platform does to a real pen - and then
// measures the committed stroke two ways:
//   - the median turn angle between consecutive points.  A lattice staircase shows up here as tens of
//     degrees, because at 1-2 unit spacing an integer grid only permits a few segment directions.
//   - the worst deviation from the arc the samples were taken from, which is what says the fit is
//     following the input rather than merely flattening it.
// Both are measured at two sample spacings: the ~1.5 units a dense digitiser gives, and the ~10 units a
// 125 Hz mouse gives at writing speed.  The sparse case is not a smaller version of the dense one - the
// quantisation is the same half unit either way, so it is a far smaller *angle* when the samples are
// further apart - and only measuring the dense case would leave the common one untested.
int ScribbleTest::curveFitTest()
{
  int nbad = 0;
  auto check = [&](bool ok, const char* what) {
    if(!ok) { ++nbad; printf("FAIL: curve fit: %s\n", what); }
  };

  const Point center(400, 420);
  const Dim radius = 320;
  const Dim a0 = 3.49, a1 = 4.36;  // a shallow arc, in radians

  // draws the arc at `step` units between samples, with every sample snapped to the integer lattice,
  // and returns the committed stroke's points
  auto drawArc = [&](Dim step, int fit) {
    scribbleDoc->cfg->set("inputCurveFit", fit);
    scribbleMode->setMode(MODE_STROKE);
    // a pen with no width flags and no chisel tip goes to StrokedStrokeBuilder, so the path *is* the
    // centreline - no outline to unpick before measuring it
    scribbleDoc->app->setPen(ScribblePen(Color::BLUE, 2, ScribblePen::TIP_ROUND));
    Dim dtheta = step/radius;
    bool first = true;
    for(Dim th = a0; th <= a1; th += dtheta) {
      Dim x = std::floor(center.x + radius*std::cos(th) + 0.5);
      Dim y = std::floor(center.y + radius*std::sin(th) + 0.5);
      ie(x, y, 0, pen, first ? press : 0);
      first = false;
    }
    ie(0, 0, 0, pen, release);
    std::vector<Point> pts;
    Element* elem = NULL;
    for(Element* s : scribbleArea->currPage->children())
      elem = s;  // the stroke just drawn is the last child
    if(elem && elem->isPathElement()) {
      Path2D* path = static_cast<SvgPath*>(elem->node)->path();
      for(int ii = 0; ii < path->size(); ++ii)
        pts.push_back(path->point(ii));
    }
    return pts;
  };

  // median turn angle in degrees, and the RMS residual against a circle fitted to the points themselves.
  // The residual is measured against a *fitted* circle rather than the arc the samples were taken from,
  // because ie() takes screen coordinates while the committed path is in document coordinates - the view
  // transform sits between them, so the original centre and radius do not describe the output at all.
  // Fitting instead asks the question that actually matters: is the result still the arc it was, or has
  // the fit cut the corner off it?
  auto measure = [&](const std::vector<Point>& pts, Dim& medTurn, Dim& rmsDev) {
    std::vector<Dim> turns;
    for(size_t ii = 1; ii + 1 < pts.size(); ++ii) {
      Point d0 = pts[ii] - pts[ii-1], d1 = pts[ii+1] - pts[ii];
      if(d0.dist() < 1E-9 || d1.dist() < 1E-9) continue;
      Dim c = std::min(Dim(1), std::max(Dim(-1), dot(d0.normalize(), d1.normalize())));
      turns.push_back(std::acos(c)*180/M_PI);
    }
    std::sort(turns.begin(), turns.end());
    medTurn = turns.empty() ? 0 : turns[turns.size()/2];
    // least-squares circle (Kasa): the linear system in (a, b, c) for x^2 + y^2 + a x + b y + c = 0
    rmsDev = 0;
    int n = int(pts.size());
    if(n < 4) return;
    Dim sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0, sz = 0, sxz = 0, syz = 0;
    for(const Point& p : pts) {
      Dim z = p.x*p.x + p.y*p.y;
      sx += p.x; sy += p.y; sxx += p.x*p.x; syy += p.y*p.y; sxy += p.x*p.y;
      sz += z; sxz += p.x*z; syz += p.y*z;
    }
    Dim m[3][3] = {{sxx, sxy, sx}, {sxy, syy, sy}, {sx, sy, Dim(n)}};
    Dim rhs[3] = {-sxz, -syz, -sz};
    // Gaussian elimination with partial pivoting - three unknowns, so this is short enough to inline
    for(int col = 0; col < 3; ++col) {
      int piv = col;
      for(int row = col + 1; row < 3; ++row)
        if(std::abs(m[row][col]) > std::abs(m[piv][col])) piv = row;
      if(std::abs(m[piv][col]) < 1E-12) return;
      std::swap(m[col], m[piv]); std::swap(rhs[col], rhs[piv]);
      for(int row = 0; row < 3; ++row) {
        if(row == col) continue;
        Dim f = m[row][col]/m[col][col];
        for(int k = col; k < 3; ++k) m[row][k] -= f*m[col][k];
        rhs[row] -= f*rhs[col];
      }
    }
    Point fitCenter(-rhs[0]/m[0][0]/2, -rhs[1]/m[1][1]/2);
    Dim fitR2 = fitCenter.x*fitCenter.x + fitCenter.y*fitCenter.y - rhs[2]/m[2][2];
    if(fitR2 <= 0) return;
    Dim fitR = std::sqrt(fitR2);
    Dim sum = 0;
    for(const Point& p : pts) { Dim d = p.dist(fitCenter) - fitR; sum += d*d; }
    rmsDev = std::sqrt(sum/n);
  };

  char buf[256];
  // every strength at the dense spacing, to show what each extra pass is actually worth
  for(int lvl = 0; lvl <= 4; ++lvl) {
    Dim t = 0, d = 0;
    scribbleDoc->newDocument();
    measure(drawArc(1.5, lvl), t, d);
    printf("curve fit level %d: median turn %.1f deg, circle residual %.3f\n", lvl, double(t), double(d));
  }
  for(Dim step : {Dim(1.5), Dim(10)}) {
    Dim offTurn = 0, offDev = 0, fitTurn = 0, fitDev = 0;
    scribbleDoc->newDocument();
    measure(drawArc(step, 0), offTurn, offDev);
    scribbleDoc->newDocument();
    measure(drawArc(step, 4), fitTurn, fitDev);

    snprintf(buf, sizeof(buf), "curve fit @ %.1f unit spacing: median turn %.1f -> %.1f deg, "
        "circle residual %.3f -> %.3f units\n", double(step),
        double(offTurn), double(fitTurn), double(offDev), double(fitDev));
    printf("%s", buf);

    // The two spacings are different problems and are checked differently, which is the point of
    // measuring both.  Quantisation is half a unit either way, so it is a large *angle* between samples
    // 1.5 units apart and a small one between samples 10 units apart: the staircase is a slow-writing
    // artifact.  Asserting a big improvement at both spacings would be asserting something untrue of
    // the sparse case, where there is almost nothing there to remove.
    if(step < 5) {
      check(offTurn > 10, "samples this close together must show the lattice staircase without the fit");
      check(fitTurn < 5, "and the fit must take them down to something that reads as a curve");
    }
    else
      check(fitTurn <= offTurn, "the fit must never make a sparsely sampled stroke less smooth");
    // Either way it must still be the arc it was drawn as: a fit that cut corners rather than removing
    // noise would leave a *larger* residual against the circle its own points best fit.
    check(fitDev <= offDev + 0.05, "the fit must not pull the stroke off the arc it was drawn as");
  }
  scribbleDoc->newDocument();
  return nbad;
}

int ScribbleTest::shapeRoundTripTest()
{
  int nbad = 0;
  auto check = [&](bool ok, const char* what) {
    if(!ok) { ++nbad; printf("FAIL: shape round trip: %s\n", what); }
  };

  scribbleDoc->newDocument();
  scribbleMode->setMode(MODE_STROKE);
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 2, ScribblePen::TIP_ROUND));
  scribbleMode->shapeId = SHAPE_BOX;
  scribbleMode->setMode(MODE_DRAWSHAPE);
  // drag out a box twice as wide as it is tall
  ie(120, 160, 0, pen, press);
  ie(180, 190, 0, pen);
  ie(200, 200, 0, pen);
  ie(0, 0, 0, pen, release);
  scribbleMode->setMode(MODE_STROKE);

  Page* page = scribbleArea->currPage;
  check(page->strokeCount() == 1, "exactly one element should have been added");
  if(page->strokeCount() != 1)
    return nbad;
  Element* shape = *page->children().begin();
  check(shape->isShape(), "the new element should carry a shape descriptor");
  check(shape->shapeParams().id == SHAPE_BOX, "the new element should be a box");
  if(!shape->isShape())
    return nbad;
  Rect descriptorRect = shape->shapeParams().rect();
  // compare against the path, not Element::bbox(), which is padded by the stroke width
  Rect pathRect = static_cast<SvgPath*>(shape->node)->path()->getBBox();
  check(approxEq(pathRect, descriptorRect, 1e-6),
      "the rendered path must match the descriptor it was generated from");
  check(descriptorRect.width() > 1.5*descriptorRect.height()
      && descriptorRect.width() < 2.5*descriptorRect.height(),
      "the box should have the aspect ratio that was dragged");

  // save and reload: the descriptor has to survive the file format
  std::string file = outPath + "/shape_roundtrip_out.html";
  check(scribbleDoc->saveDocument(file.c_str()), "saving the document should succeed");
  scribbleDoc->newDocument();
  check(scribbleDoc->openDocument(file.c_str()) == Document::LOAD_OK, "reloading should succeed");
  removeFile(file.c_str());
  page = scribbleArea->currPage;
  check(page->strokeCount() == 1, "the reloaded page should hold one element");
  if(page->strokeCount() != 1)
    return nbad;
  shape = *page->children().begin();
  check(shape->isShape() && shape->shapeParams().id == SHAPE_BOX,
      "the reloaded element should still be a box");
  check(approxEq(shape->shapeParams().rect(), descriptorRect, 1e-3),
      "the reloaded descriptor should match the saved one");
  check(approxEq(static_cast<SvgPath*>(shape->node)->path()->getBBox(), descriptorRect, 1e-3),
      "the reloaded path should match the descriptor");

  // spec 7.8: a non-rotating scale must update the descriptor, not just the path - otherwise the shape
  //  silently snaps back to its old size the next time a parameter changes
  shape->applyTransform(ScribbleTransform(Transform2D::scaling(2, 1), 2, 1));
  shape->commitTransform();
  Rect scaled = static_cast<SvgPath*>(shape->node)->path()->getBBox();
  check(approxEq(scaled.width(), 2*descriptorRect.width(), 1e-3), "scaling should widen the shape");
  check(approxEq(shape->shapeParams().rect(), scaled, 1e-6),
      "scaling must update the descriptor, not only the path");
  // now change a parameter; the shape must keep its resized dimensions
  ShapeParams params = shape->shapeParams();
  params.rx = 4;
  params.ry = 4;
  shape->setShapeParams(params);
  check(approxEq(static_cast<SvgPath*>(shape->node)->path()->getBBox().width(), scaled.width(), 1e-3),
      "changing a parameter after a resize must not snap the shape back to its old size");

  scribbleDoc->newDocument();
  return nbad;
}

int ScribbleTest::shapeSnapTest()
{
  int nbad = 0;
  auto check = [&](bool ok, const char* what) {
    if(!ok) { ++nbad; printf("FAIL: shape snap: %s\n", what); }
  };
  Page* page = NULL;
  auto lastElement = [&]() {
    Element* elem = NULL;
    for(Element* s : page->children())
      elem = s;
    return elem;
  };
  auto begin = [&](float delay) {
    scribbleDoc->newDocument();
    scribbleDoc->cfg->set("shapeSnapDelay", delay);
    scribbleMode->setMode(MODE_STROKE);
    scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 2, ScribblePen::TIP_ROUND));
    page = scribbleArea->currPage;
  };
  // the pen resting at the end of a stroke: a few samples jittering within a pixel
  auto hold = [&](Dim x, Dim y) {
    for(int i = 0; i < 8; ++i)
      ie(x + 0.4*(i % 2), y + 0.3*((i/2) % 2), 0, pen);
  };
  // stands in for the timer: the hold has lasted as long as it needs to
  auto fire = [&]() { return scribbleArea->checkShapeSnap(scribbleArea->snapHoldStart + 1600); };
  const Point center(400, 400);
  const Dim radius = 110;
  auto drawCircle = [&]() {
    for(int i = 0; i <= 72; ++i) {
      Dim theta = 2*M_PI*i/72;
      ie(center.x + radius*std::cos(theta), center.y + radius*std::sin(theta), 0, pen, i == 0 ? press : 0);
    }
    hold(center.x + radius, center.y);
  };

  // a held circle snaps, and dragging away from its centre scales it
  begin(0.8f);
  drawCircle();
  scribbleArea->checkShapeSnap(scribbleArea->snapHoldStart + 100);
  check(!scribbleArea->snapActive, "nothing may snap before the hold has lasted the delay");
  fire();
  check(scribbleArea->snapActive && scribbleArea->currStroke && scribbleArea->currStroke->isShape()
      && scribbleArea->currStroke->shapeParams().id == SHAPE_ELLIPSE, "a held circle should snap to an ellipse");
  Rect snapped = scribbleArea->snapParams.rect();
  check(std::abs(snapped.width() - snapped.height()) < 1E-6*snapped.width(), "a circle should come out a true circle");
  ie(center.x + 1.5*radius, center.y, 0, pen);
  ie(0, 0, 0, pen, release);
  Element* shape = lastElement();
  check(page->strokeCount() == 1 && shape && shape->isShape(), "release should leave the shape and no ink");
  if(!shape || !shape->isShape())
    return nbad;
  check(approxEq(shape->shapeParams().rect().width(), 1.5*snapped.width(), 0.02*snapped.width()),
      "moving the pen 1.5x as far from the centre should scale the shape 1.5x");
  undo();
  check(page->strokeCount() == 1 && approxEq(lastElement()->shapeParams().rect(), snapped, 1E-3),
      "the first undo should go back to the shape as it snapped");
  undo();
  check(page->strokeCount() == 0, "the second undo should remove the shape");
  redo();
  redo();
  check(page->strokeCount() == 1 && approxEq(lastElement()->shapeParams().rect().width(),
      1.5*snapped.width(), 0.02*snapped.width()), "redo should bring back the scaled shape");

  // a shape the gesture did not change is one undo step, not two
  begin(0.8f);
  drawCircle();
  fire();
  ie(0, 0, 0, pen, release);
  check(page->strokeCount() == 1 && lastElement()->isShape(), "an unchanged snap should still commit the shape");
  undo();
  check(page->strokeCount() == 0, "an unchanged snap should undo in one step");

  // a held line snaps to a line whose end then follows the pen
  begin(0.8f);
  for(int i = 0; i <= 40; ++i)
    ie(200 + 6*i, 300 + 0.3*std::sin(Dim(i)), 0, pen, i == 0 ? press : 0);
  hold(440, 300);
  fire();
  check(scribbleArea->snapActive && scribbleArea->currStroke->shapeParams().id == SHAPE_LINE,
      "a held straight stroke should snap to a line");
  Point lineEnd = scribbleArea->snapParams.points.back();
  ie(440, 360, 0, pen);
  ie(0, 0, 0, pen, release);
  shape = lastElement();
  check(shape && shape->isShape() && shape->shapeParams().points.back().y > lineEnd.y + 1,
      "the line's end should follow the pen after the snap");

  // a held rectangle snaps to a box
  begin(0.8f);
  {
    const Point corners[] = {{250, 250}, {500, 250}, {500, 400}, {250, 400}, {250, 250}};
    bool first = true;
    for(int side = 0; side < 4; ++side) {
      for(int i = 0; i < 25; ++i) {
        Point pt = corners[side] + (corners[side+1] - corners[side])*(i/Dim(25));
        ie(pt.x, pt.y, 0, pen, first ? press : 0);
        first = false;
      }
    }
    hold(250, 250);
  }
  fire();
  check(scribbleArea->snapActive && scribbleArea->currStroke->shapeParams().id == SHAPE_BOX,
      "a held rectangle should snap to a box");
  ie(0, 0, 0, pen, release);

  // without the hold, or with snapping turned off, a stroke stays ink
  begin(0.8f);
  drawCircle();
  ie(0, 0, 0, pen, release);
  check(page->strokeCount() == 1 && !lastElement()->isShape(), "a stroke that was never held must stay ink");
  begin(0.0f);
  drawCircle();
  check(!fire() && !scribbleArea->snapActive, "with the delay at 0 nothing may snap");
  ie(0, 0, 0, pen, release);
  check(page->strokeCount() == 1 && !lastElement()->isShape(), "with snapping off a held circle stays ink");

  // a held scratch-out erases what it was drawn over, and nothing it was not
  begin(0.8f);
  ie(260, 300, 0, pen, press); ie(290, 310, 0, pen); ie(320, 305, 0, pen); ie(0, 0, 0, pen, release);
  ie(600, 600, 0, pen, press); ie(640, 610, 0, pen); ie(0, 0, 0, pen, release);
  check(page->strokeCount() == 2, "two strokes to scratch over");
  for(int pass = 0; pass < 6; ++pass) {
    for(int i = 0; i <= 20; ++i) {
      Dim frac = i/Dim(20);
      Dim x = pass % 2 ? 380 - 180*frac : 200 + 180*frac;
      ie(x, 285 + 8*pass + 8*frac, 0, pen, pass == 0 && i == 0 ? press : 0);
    }
  }
  hold(200, 333);
  fire();
  check(!scribbleArea->snapActive, "a scratch-out must not become a shape");
  ie(0, 0, 0, pen, release);
  check(page->strokeCount() == 1, "the scratch-out should erase the stroke under it and leave itself out");
  undo();
  check(page->strokeCount() == 2, "undo should bring the erased stroke back");

  scribbleDoc->newDocument();
  return nbad;
}

// Ruling regions (rulingregion.h).  runRegionTests() in regiontest.cpp covers the geometry; this is
//  everything that needs a document.  The region used is tilted on purpose: an unrotated region would let
//  every check pass against code that only ever looked at page y.
// Reflow on a page with no margin (a dot grid): words wrapped onto an empty line start where the text does,
//  not at the page edge, and words wrapped onto a line with text are kept a word gap apart - the writer's gap,
//  not one sized from the pitch.  Against the old code the first came out at x = 9 against text at 19, and
//  the second at 11.25 (1.25 x 0.3 x pitch) against a written gap of 17.
int ScribbleTest::reflowIndentTest()
{
  int nbad = 0;
  auto check = [&](bool ok, const char* what) {
    if(!ok) { ++nbad; printf("FAIL: reflow: %s\n", what); }
  };
  auto at = [&](Point pagept, int ev) {
    Point scr = scribbleArea->dimToScreen(scribbleArea->pageDimToDim(pagept));
    ie(scr.x, scr.y, 0, pen, ev);
  };
  const Dim pitch = 30, row = 4*pitch, textLeft = 20;
  // a word of n letters, each a zigzag in the lower part of the row starting at rowTop; returns its right end
  auto word = [&](Dim x, Dim rowTop, int n) {
    scribbleMode->setMode(MODE_STROKE);
    for(int ii = 0; ii < n; ++ii) {
      Dim lx = x + ii*14;
      at(Point(lx, rowTop + 0.35*pitch), press);
      at(Point(lx + 4, rowTop + 0.85*pitch), INPUTEVENT_MOVE);
      at(Point(lx + 8, rowTop + 0.35*pitch), INPUTEVENT_MOVE);
      at(Point(lx + 11, rowTop + 0.85*pitch), INPUTEVENT_MOVE);
      at(Point(lx + 11, rowTop + 0.85*pitch), release);
    }
    return x + n*14;
  };
  auto onRow = [&](const Element* s, Dim rowTop) { return s->com().y >= rowTop && s->com().y < rowTop + pitch; };

  for(int nextLineText = 0; nextLineText < 2; ++nextLineText) {
    scribbleDoc->newDocument();
    doCommand(ID_RESETZOOM);
    scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 2, ScribblePen::TIP_ROUND));
    PageProperties props(768, 1024, pitch, pitch, 0, Color::WHITE, Color::BLUE, 1.5);
    scribbleDoc->setPageProperties(&props, true, false, false, false);
    // a full line of 4-letter words 16 apart, and optionally a short word starting the next line
    Dim x = textLeft;
    while(x + 60 < 740) x = word(x, row, 4) + 16;
    std::set<Element*> existing;
    if(nextLineText) {
      word(textLeft, row + pitch, 3);
      for(Element* s : scribbleArea->currPage->children())
        if(onRow(s, row + pitch)) existing.insert(s);
    }
    Dim inkLeft = MAX_DIM, writtenGap = 0;
    for(Element* s : scribbleArea->currPage->children())
      inkLeft = std::min(inkLeft, s->bbox().left);
    {
      // the gap between the first two words' ink: first letter of word 2 minus last letter of word 1
      std::vector<Element*> line;
      for(Element* s : scribbleArea->currPage->children()) if(onRow(s, row)) line.push_back(s);
      writtenGap = line[4]->bbox().left - line[3]->bbox().right;
    }
    // ruled insert space from the gap after the first word, 160 to the right: the line overflows
    Dim px = textLeft + 4*14 + 8;
    scribbleMode->setMode(MODE_INSSPACERULED);
    at(Point(px, row + 0.5*pitch), press);
    for(int ii = 1; ii <= 16; ++ii) at(Point(px + 10*ii, row + 0.5*pitch), INPUTEVENT_MOVE);
    at(Point(px + 160, row + 0.5*pitch), release);

    Dim wrappedLeft = MAX_DIM, wrappedRight = MIN_DIM, existingLeft = MAX_DIM;
    for(Element* s : scribbleArea->currPage->children()) {
      if(!onRow(s, row + pitch)) continue;
      if(existing.count(s))
        existingLeft = std::min(existingLeft, s->bbox().left);
      else {
        wrappedLeft = std::min(wrappedLeft, s->bbox().left);
        wrappedRight = std::max(wrappedRight, s->bbox().right);
      }
    }
    check(wrappedLeft < MAX_DIM, "the overflowing words wrap onto the next line");
    if(!nextLineText)
      check(std::abs(wrappedLeft - inkLeft) < 0.5, "words wrapped onto an empty line start where the text does");
    else {
      check(std::abs(wrappedLeft - inkLeft) < 0.5, "words wrapped onto a line with text start where it did");
      check(std::abs((existingLeft - wrappedRight) - writtenGap) < 0.5,
          "wrapped words are the writer's word gap from the text already on the line");
    }
  }
  scribbleDoc->clearSelection();
  return nbad;
}

// The Skip Lines toggle (skippedLineFrame): with it on, reflow wraps to the next *text* line, an underline in
//  a blank line does not pull anything onto it, and vertical steps are two lines; with it off, nothing changes.
//  Default test page: lined, 40 pitch, margin 100.
int ScribbleTest::skippedLinesTest()
{
  int nbad = 0;
  auto check = [&](bool ok, const char* what) {
    if(!ok) { ++nbad; printf("FAIL: skipped lines: %s\n", what); }
  };
  auto at = [&](Point pagept, int ev) {
    Point scr = scribbleArea->dimToScreen(scribbleArea->pageDimToDim(pagept));
    ie(scr.x, scr.y, 0, pen, ev);
  };
  const Dim pitch = 40, textLeft = 110;
  auto lineOf = [&](const Element* s) { return int(std::floor(s->com().y/pitch)); };
  // a word of 4 letters on `line`, starting at x; returns its strokes
  auto word = [&](Dim x, int line) {
    std::vector<Element*> letters;
    scribbleMode->setMode(MODE_STROKE);
    for(int ii = 0; ii < 4; ++ii) {
      Dim lx = x + ii*14, top = line*pitch;
      at(Point(lx, top + 0.35*pitch), press);
      at(Point(lx + 4, top + 0.85*pitch), INPUTEVENT_MOVE);
      at(Point(lx + 8, top + 0.35*pitch), INPUTEVENT_MOVE);
      at(Point(lx + 11, top + 0.85*pitch), INPUTEVENT_MOVE);
      at(Point(lx + 11, top + 0.85*pitch), release);
      Element* last = NULL;
      for(Element* s : scribbleArea->currPage->children()) last = s;
      letters.push_back(last);
    }
    return letters;
  };
  // a descender: from the bottom of `line` well down into the line below
  auto descender = [&](Dim x, int line) {
    scribbleMode->setMode(MODE_STROKE);
    at(Point(x, line*pitch + 0.8*pitch), press);
    at(Point(x - 2, line*pitch + 1.3*pitch), INPUTEVENT_MOVE);
    at(Point(x - 6, line*pitch + 1.6*pitch), INPUTEVENT_MOVE);
    at(Point(x - 6, line*pitch + 1.6*pitch), release);
  };
  // an underline beneath the first word of `line`, in the upper part of the line below
  auto underline = [&](Dim x, int line) {
    scribbleMode->setMode(MODE_STROKE);
    at(Point(x, (line + 1.15)*pitch), press);
    at(Point(x + 30, (line + 1.2)*pitch), INPUTEVENT_MOVE);
    at(Point(x + 56, (line + 1.15)*pitch), INPUTEVENT_MOVE);
    at(Point(x + 56, (line + 1.15)*pitch), release);
  };
  // line 3 full of words (returns the last one, which must wrap), then two words on each of `others`
  auto setup = [&](const std::vector<int>& others) {
    scribbleDoc->newDocument();
    doCommand(ID_RESETZOOM);
    scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 2, ScribblePen::TIP_ROUND));
    std::vector<Element*> lastWord;
    for(Dim x = textLeft; x + 60 < 740; x += 4*14 + 16)
      lastWord = word(x, 3);
    for(int line : others) {
      word(textLeft, line);
      word(textLeft + 4*14 + 16, line);
    }
    return lastWord;
  };
  // ruled insert space on line 3 from the gap after the first word, far enough right to overflow
  auto pushRight = [&]() {
    Dim px = textLeft + 4*14 + 8;
    scribbleMode->setMode(MODE_INSSPACERULED);
    at(Point(px, 3*pitch + 0.5*pitch), press);
    for(int ii = 1; ii <= 16; ++ii) at(Point(px + 10*ii, 3*pitch + 0.5*pitch), INPUTEVENT_MOVE);
    at(Point(px + 160, 3*pitch + 0.5*pitch), release);
  };
  auto allOn = [&](const std::vector<Element*>& strokes, int line) {
    for(Element* s : strokes) if(lineOf(s) != line) return false;
    return true;
  };

  const bool wasSkipping = scribbleMode->insSpaceSkipLines;
  scribbleMode->insSpaceSkipLines = true;
  std::vector<Element*> wrapped = setup({5, 7, 9});
  pushRight();
  check(allOn(wrapped, 5), "double spaced: words wrap to the next text line, not the blank one");
  {
    bool blankStaysBlank = true;
    for(Element* s : scribbleArea->currPage->children()) if(lineOf(s) == 4) blankStaysBlank = false;
    check(blankStaysBlank, "double spaced: the blank line stays blank");
  }

  // descenders stay on their line (calcCom leans on a stroke's first point and top), but an underline lies in
  //  the blank line below its word - that is the stray ink the detection has to see past
  wrapped = setup({5, 7, 9});
  descender(300, 3);
  descender(160, 5);
  for(int line : {3, 5, 7}) underline(textLeft, line);
  pushRight();
  check(allOn(wrapped, 5), "double spaced with underlines in the blank lines: still wraps to the next text line");


  // insert space from the margin on double spaced text moves it a text line, i.e. two lines
  setup({5, 7, 9});
  std::vector<Element*> line5;
  for(Element* s : scribbleArea->currPage->children()) if(lineOf(s) == 5) line5.push_back(s);
  scribbleMode->setMode(MODE_INSSPACERULED);
  at(Point(50, 5*pitch + 0.5*pitch), press);
  //  (a step happens when the pen reaches the next text line, as one does on a line without Skip Lines)
  at(Point(50, 5*pitch + 1.0*pitch), INPUTEVENT_MOVE);
  at(Point(50, 5*pitch + 2.5*pitch), INPUTEVENT_MOVE);
  at(Point(50, 5*pitch + 2.5*pitch), release);
  check(!line5.empty() && allOn(line5, 7), "double spaced: vertical insert space steps two lines");

  // pressed on the blank line between two text lines (where you would put the pen to push the text below
  //  down), nothing of the text line above the press moves: the frame used to start half a line above the
  //  pressed line, which on a blank line runs through the middle of the text above and took its letters
  //  (x-height letters sitting on the rule, as handwriting does, so their centre is in the lower half)
  setup({5, 7, 9});
  scribbleMode->setMode(MODE_STROKE);
  for(int ii = 0; ii < 4; ++ii) {
    Dim lx = 600 + ii*14, top = 3*pitch;
    at(Point(lx, top + 0.6*pitch), press);
    at(Point(lx + 4, top + 0.95*pitch), INPUTEVENT_MOVE);
    at(Point(lx + 8, top + 0.6*pitch), INPUTEVENT_MOVE);
    at(Point(lx + 11, top + 0.95*pitch), INPUTEVENT_MOVE);
    at(Point(lx + 11, top + 0.95*pitch), release);
  }
  std::vector<Element*> line3, line5b;
  for(Element* s : scribbleArea->currPage->children()) {
    if(lineOf(s) == 3) line3.push_back(s);
    if(lineOf(s) == 5) line5b.push_back(s);
  }
  scribbleMode->setMode(MODE_INSSPACERULED);
  at(Point(50, 4*pitch + 0.5*pitch), press);
  at(Point(50, 4*pitch + 1.0*pitch), INPUTEVENT_MOVE);
  at(Point(50, 4*pitch + 2.6*pitch), INPUTEVENT_MOVE);
  at(Point(50, 4*pitch + 2.6*pitch), release);
  check(!line3.empty() && allOn(line3, 3), "double spaced, pressed on a blank line: the text line above stays put");
  check(!line5b.empty() && allOn(line5b, 7), "double spaced, pressed on a blank line: the text below moves two lines");

  // off: the same double spaced text wraps onto the blank line, as ruled insert space always has
  scribbleMode->insSpaceSkipLines = false;
  wrapped = setup({5, 7, 9});
  pushRight();
  check(allOn(wrapped, 4), "toggle off: words wrap to the very next line");

  scribbleMode->insSpaceSkipLines = wasSkipping;
  scribbleDoc->clearSelection();
  return nbad;
}

int ScribbleTest::rulingRegionTest()
{
  int nbad = 0;
  auto check = [&](bool ok, const char* what) {
    if(!ok) { ++nbad; printf("FAIL: ruling region: %s\n", what); }
  };
  // input is in screen coordinates; everything checked is in page coordinates
  auto at = [&](Point pagept, int ev) {
    Point scr = scribbleArea->dimToScreen(scribbleArea->pageDimToDim(pagept));
    ie(scr.x, scr.y, 0, pen, ev);
  };
  auto elementAt = [&](int idx) -> Element* {
    int ii = 0;
    for(Element* s : scribbleArea->currPage->children()) { if(ii++ == idx) return s; }
    return NULL;
  };
  auto stroke = [&](const std::vector<Point>& pts) -> Element* {
    scribbleMode->setMode(MODE_STROKE);
    at(pts.front(), press);
    for(size_t ii = 1; ii < pts.size(); ++ii) at(pts[ii], INPUTEVENT_MOVE);
    at(pts.back(), release);
    Element* last = NULL;
    for(Element* s : scribbleArea->currPage->children()) last = s;
    return last;
  };
  auto pathOf = [](Element* s) { return *static_cast<SvgPath*>(s->node)->path(); };

  scribbleDoc->newDocument();
  Page* page = scribbleArea->currPage;
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 2, ScribblePen::TIP_ROUND));
  // ink written before the region: the region must still go *under* it
  Element* before = stroke({Point(150, 300), Point(250, 305)});

  Element* region = scribbleArea->addRulingRegion(Rect::ltrb(100, 200, 400, 440));
  check(region && region->isRulingRegion(), "the region tool makes a ruling region");
  if(!region) return nbad;
  // tilt it by 0.3 rad about its centre, with a 30 unit pitch; the ink written before it stays where it is
  RulingRegionParams params = region->regionParams();
  params.yRuling = 30;
  params.transform(Transform2D().rotate(0.3, Point(250, 320)));
  scribbleArea->setSelRegionParams(params);
  scribbleDoc->clearSelection();
  check(elementAt(0) == region && elementAt(1) == before, "a region goes below ink written before it");
  check(region->layer() == LayerList::REGION_LAYER, "a region is on no layer");

  RulingFrame frame = region->regionParams().frame(region);
  Point inside = frame.toPage(frame.toLocal(Point(250, 320)));
  check(page->rulingAt(inside).region == region, "rulingAt() inside the region is the region's ruling");
  check(page->rulingAt(Point(700, 800)).region == NULL, "rulingAt() outside it is the page's");

  // *** the requirement: a centre-on-line stroke in a tilted region runs along the region's line ***
  int line = frame.line(inside, Page::BLANK_Y_RULING);
  Dim centre = (line + 0.5)*30;
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 2, ScribblePen::TIP_ROUND | ScribblePen::CENTER_ON_LINE));
  Element* centred = stroke({frame.toPage(Point(60, centre - 9)), frame.toPage(Point(160, centre + 11)),
      frame.toPage(Point(220, centre + 4))});
  {
    Path2D path = pathOf(centred);
    bool ok = path.size() >= 2;
    for(int ii = 0; ii < path.size(); ++ii)
      ok = ok && std::abs(frame.toLocal(path.point(ii)).y - centre) < 1E-3;
    check(ok, "a centre-on-line stroke in a tilted region lies on the middle of the region's line");
    check(ok && std::abs(frame.toLocal(path.point(path.size()-1)).x - 220) < 1E-3,
        "...and still follows the pen along the line");
  }

  // snap to grid: onto the region's grid, not the page's
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 2, ScribblePen::TIP_ROUND | ScribblePen::SNAP_TO_GRID));
  Element* snapped = stroke({frame.toPage(Point(47, 71)), frame.toPage(Point(107, 131))});
  {
    Path2D path = pathOf(snapped);
    Point l0 = frame.toLocal(path.point(0));
    // the region has no x ruling, so the grid is square at the y pitch
    check(std::abs(l0.x - 30*std::round(l0.x/30)) < 1E-3 && std::abs(l0.y - 30*std::round(l0.y/30)) < 1E-3,
        "a snap-to-grid stroke in a region snaps to the region's (tilted) grid");
  }
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 2, ScribblePen::TIP_ROUND));

  // a second stroke two lines down, for ruled select to leave alone
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 2, ScribblePen::TIP_ROUND | ScribblePen::CENTER_ON_LINE));
  Element* lower = stroke({frame.toPage(Point(60, centre + 60)), frame.toPage(Point(200, centre + 60))});
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 2, ScribblePen::TIP_ROUND));

  // ruled select along one of the region's (tilted) lines takes that line's ink and nothing else
  scribbleMode->setMode(MODE_SELECTRULED);
  at(frame.toPage(Point(40, centre)), press);
  at(frame.toPage(Point(150, centre)), INPUTEVENT_MOVE);
  at(frame.toPage(Point(260, centre)), INPUTEVENT_MOVE);
  at(frame.toPage(Point(260, centre)), release);
  {
    Selection* sel = scribbleArea->currSelection;
    bool hasCentred = sel && centred->isSelected(sel);
    check(hasCentred, "ruled select along a tilted region line selects the ink on that line");
    check(sel && !lower->isSelected(sel), "...but not the ink two lines further down the region");
    check(sel && !region->isSelected(sel), "...and never the region itself");
  }
  scribbleDoc->clearSelection();

  // the region is out of reach of ink selection and erasers
  scribbleDoc->doCommand(ID_SELALL);
  check(scribbleArea->currSelection && !region->isSelected(scribbleArea->currSelection),
      "Select All takes the ink but not the region");
  scribbleDoc->clearSelection();
  int ncount = page->strokeCount();
  scribbleMode->setMode(MODE_ERASESTROKE);
  // across a stretch of the region's paper with no ink on it
  at(frame.toPage(Point(250, 200)), press);  at(frame.toPage(Point(290, 205)), INPUTEVENT_MOVE);
  at(frame.toPage(Point(290, 205)), release);
  check(page->strokeCount() == ncount && region->node->parent(), "the stroke eraser does not erase a region");

  // moving the region carries its ink, as one undo step; undo takes both back
  Point centredStart = pathOf(centred).point(0);
  Point cornerStart = region->regionParams().corners[0];
  Point beforeStart = pathOf(before).point(0);
  scribbleArea->selectRegion(region);
  size_t steps = scribbleDoc->history->undoSteps();
  scribbleMode->setMode(MODE_STROKE);
  // press away from every handle and from the button: the middle of the region
  Point grab = frame.toPage(Point(150, 90));
  at(grab, press);
  at(grab + Point(20, 30), INPUTEVENT_MOVE);
  at(grab + Point(40, 60), INPUTEVENT_MOVE);
  at(grab + Point(40, 60), release);
  auto offsetOf = [&](Element* s, Point start) { return pathOf(s).point(0) + Point(s->node->getTransform().xoffset(),
      s->node->getTransform().yoffset()) - start; };
  check(approxEq(region->regionParams().corners[0] - cornerStart, Point(40, 60), 1E-3), "the region moved");
  check(approxEq(offsetOf(centred, centredStart), Point(40, 60), 1E-3), "...and carried the ink inside it");
  check(approxEq(offsetOf(before, beforeStart), Point(40, 60), 1E-3),
      "...including ink written before the region was made");
  check(scribbleDoc->history->undoSteps() == steps + 1, "moving a region and its ink is one undo step");
  scribbleDoc->clearSelection();
  scribbleDoc->doUndoRedo(false);
  check(approxEq(region->regionParams().corners[0], cornerStart, 1E-3)
      && approxEq(offsetOf(centred, centredStart), Point(0, 0), 1E-3), "undo puts the region and its ink back");

  // the size handle makes more paper, not bigger lines: the outline grows - by different amounts each
  //  way - while the ruling and the ink inside stay exactly where they were
  {
    scribbleArea->selectRegion(region);
    RulingRegionParams start = region->regionParams();
    auto localSize = [](const RulingRegionParams& rp) {
      RulingFrame f = rp.frame();
      Rect r;
      for(Point p : rp.corners) r.rectUnion(f.toLocal(p));
      return Point(r.width(), r.height());
    };
    Point size0 = localSize(start);
    Point handle = scribbleArea->regionSelector ? scribbleArea->regionSelector->scaleHandlePos() : Point(NaN, NaN);
    Point drag = frame.toPageDir(Point(60, 20));
    steps = scribbleDoc->history->undoSteps();
    scribbleMode->setMode(MODE_STROKE);
    at(handle, press);
    at(handle + drag*0.5, INPUTEVENT_MOVE);
    at(handle + drag, INPUTEVENT_MOVE);
    at(handle + drag, release);
    const RulingRegionParams& now = region->regionParams();
    check(approxEq(localSize(now) - size0, Point(60, 20), 1E-2), "the size handle stretches the outline, freely");
    check(std::abs(now.yRuling - start.yRuling) < 1E-6 && approxEq(now.origin, start.origin, 1E-6)
        && std::abs(now.angle - start.angle) < 1E-9, "...without scaling or moving the lines");
    check(approxEq(offsetOf(centred, centredStart), Point(0, 0), 1E-3), "...or the ink on them");
    check(scribbleDoc->history->undoSteps() == steps + 1, "resizing a region is one undo step");
    scribbleDoc->clearSelection();
    scribbleDoc->doUndoRedo(false);
    check(approxEq(localSize(region->regionParams()), size0, 1E-3), "undo puts the old size back");
  }

  // deleting a region keeps its ink
  ncount = page->strokeCount();
  scribbleArea->selectRegion(region);
  scribbleArea->deleteSelection();
  check(!region->node->parent() && page->strokeCount() == ncount - 1, "deleting a region removes the region only");
  check(page->rulingAt(inside).region == NULL, "...and the page's ruling applies there again");
  scribbleDoc->doUndoRedo(false);
  check(elementAt(0) == region, "undoing the delete puts the region back below the ink");

  // Replaying a region's add with no recorded sibling - what a sync rebase does when the region was added
  //  to an empty page - must still put it below ink that arrived since, not on top of it.
  {
    scribbleDoc->newDocument();
    Page* pg = scribbleArea->currPage;
    Element* rg = scribbleArea->addRulingRegion(Rect::ltrb(100, 200, 400, 440));
    scribbleDoc->clearSelection();
    Element* ink = stroke({Point(150, 300), Point(250, 305)});
    // the item a region added to an empty page records: no sibling to go before
    StrokeAddedItem added(rg, pg, NULL);
    added.undo();
    added.redo();
    int ii = 0;
    for(Element* s : pg->children()) { if(s == rg) break; ++ii; }
    check(ii == 0 && ink->node->parent(), "a region re-added with no recorded sibling lands below ink, not on top");
  }

  // save and reload
  {
    scribbleDoc->newDocument();
    Element* rg = scribbleArea->addRulingRegion(Rect::ltrb(100, 200, 400, 440));
    RulingRegionParams p = rg->regionParams();
    p.transform(Transform2D().rotate(-0.2, Point(250, 320)));
    p.xRuling = 25;
    p.dotRadius = 1.5;
    p.opaque = false;
    p.outline = true;
    scribbleArea->setSelRegionParams(p);
    scribbleDoc->clearSelection();
    stroke({Point(150, 300), Point(250, 305)});
    std::string file = outPath + "/region_roundtrip_out.html";
    check(scribbleDoc->saveDocument(file.c_str()), "saving a document with a region succeeds");
    scribbleDoc->newDocument();
    check(scribbleDoc->openDocument(file.c_str()) == Document::LOAD_OK, "reloading it succeeds");
    removeFile(file.c_str());
    Element* back = elementAt(0);
    check(back && back->isRulingRegion(), "the region is still a region, and still first, after a reload");
    if(back && back->isRulingRegion()) {
      const RulingRegionParams& q = back->regionParams();
      bool same = q.corners.size() == p.corners.size() && std::abs(q.angle - p.angle) < 1E-6
          && std::abs(q.xRuling - 25) < 1E-4 && std::abs(q.yRuling - p.yRuling) < 1E-4
          && std::abs(q.dotRadius - 1.5) < 1E-4 && !q.opaque && q.outline && approxEq(q.origin, p.origin, 1E-3);
      for(size_t ii = 0; same && ii < q.corners.size(); ++ii)
        same = approxEq(q.corners[ii], p.corners[ii], 1E-3);
      check(same, "outline, ruling, dots, paper and border all survive save/reload");
      scribbleDoc->doCommand(ID_SELALL);
      check(scribbleArea->currSelection && !back->isSelected(scribbleArea->currSelection),
          "a reloaded region is still out of reach of Select All");
      scribbleDoc->clearSelection();
    }
  }

  // the sync wire: what a region edit serializes reproduces it on the receive path
  {
    Element* rg = elementAt(0);
    if(rg && rg->isRulingRegion()) {
      rg->uuid = 424242;
      std::unique_ptr<ScribbleSync> sync(new ScribbleSync(scribbleDoc));
      sync->strokemap[rg->uuid] = rg;
      RulingRegionParams p = rg->regionParams();
      p.yRuling = 44;
      p.corners[2] = p.corners[2] + Point(30, 20);
      scribbleArea->selectRegion(rg);
      scribbleArea->setSelRegionParams(p);
      scribbleDoc->clearSelection();
      MemStream strm;
      RegionChangedItem(rg, scribbleArea->currPage).serialize(strm);  // the live (edited) state
      std::string wire(strm.data(), strm.size());
      scribbleDoc->doUndoRedo(false);
      check(std::abs(rg->regionParams().yRuling - 44) > 1, "the local edit is undone");
      std::string xml = "<undo uuid='78' user='peer'>" + wire + "</undo>";
      pugi::xml_document doc;
      check(doc.load_buffer(xml.data(), xml.size()), "the region item is well-formed XML");
      pugi::xml_node itemnode = doc.child("undo").first_child();
      sync->processItem(itemnode);
      check(std::abs(rg->regionParams().yRuling - 44) < 1E-6
          && approxEq(rg->regionParams().corners[2], p.corners[2], 1E-3), "a peer's region edit applies");
    }
    else
      check(false, "no region to test sync with");
  }

  scribbleDoc->newDocument();
  return nbad;
}

void ScribbleTest::runAll(bool runsynctest)
{
  nFailed = 0;
  int nThumbsFailed = 0;
  int nUnitFailed = runScanTests() + runShapeTests() + runColorTests() + runLayerTests() + runLibraryTests()
      + runRegionTests() + runNotefulTests() + runPageTagTests();
  std::vector<std::string> slFailed;
  void (ScribbleTest::*tests[])() = {
    &ScribbleTest::test0,
    &ScribbleTest::test1,
    &ScribbleTest::test2,
    &ScribbleTest::test3,
    &ScribbleTest::test4,
    &ScribbleTest::test5,
    &ScribbleTest::test6,
    &ScribbleTest::test7,
    &ScribbleTest::test8,
    &ScribbleTest::test9,
    &ScribbleTest::test10,
    &ScribbleTest::test11,
    &ScribbleTest::test12,
    &ScribbleTest::test13,
    &ScribbleTest::test14,
    &ScribbleTest::test15,
    &ScribbleTest::synctest01
  };
  unsigned int totaltests = sizeof(tests)/sizeof(tests[0]);
  unsigned int nonsynctests = totaltests - 1;  // last test is for sync only

  // create sync slave for sync test
  if(runsynctest) {
    // we are testuser1; slave is testuser2
    scribbleConfig->set("syncUser", "testuser1");
    syncSlave = new ScribbleTest(outPath);
    syncSlave->scribbleConfig->set("syncUser", "testuser2");
    syncSlave->scribbleConfig->set("syncServer", scribbleConfig->String("syncServer"));
    syncSlave->nFailed = 0;
  }

  Dim unitsPerPx = ScribbleView::unitsPerPx;
  ScribbleView::unitsPerPx = 1;
  // these are set in ScribbleApp::loadConfig() ... would be better to pass our scribbleConfig to it
  //RectSelector::HANDLE_SIZE = 4;
  SvgWriter::DEFAULT_SAVE_IMAGE_SCALED = 1;
  Page::BLANK_Y_RULING = scribbleConfig->Float("blankYRuling");  // don't serialize timestamp

  Element::SVG_NO_TIMESTAMP = true;
  SvgWriter::DEFAULT_PATH_DATA_REL = false;
  srandpp(1);  // randomStr now used for bookmark ids
  Timestamp runAllTime = mSecSinceEpoch();
  for(unsigned int ii = 0; true; ii++) {
    std::string basefile = fstring("%s/test%d", outPath.c_str(), ii);
    doCommand(ID_RESETZOOM);
    // enable this to update ref files (saving as *_new.html)
#ifdef REFRESH_REFS
    if(scribbleDoc->openDocument((basefile + "_ref.html").c_str()) == Document::LOAD_OK)
      scribbleDoc->saveDocument((basefile + "_new.html").c_str());
#endif

    scribbleDoc->newDocument();
    // attempt to open input file; fails quietly if not present
    Document::loadresult_t res = scribbleDoc->openDocument((basefile + "_in.html").c_str());
    if(res == Document::LOAD_NONFATAL) {
      scribbleDoc->checkAndClearErrors();
      scribbleDoc->cfg->set("test_loadErrors", true);
    }
    // reset mode
    scribbleMode->setRuled(true);
    scribbleMode->setMode(MODE_STROKE);
    scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 1, ScribblePen::TIP_ROUND));
    scribbleDoc->app->bookmarkColor = Color::BLUE;
    // prevent unintended updates while inspecting:
    scribbleDoc->cfg->set("savePrompt", true);

    if(syncSlave) {
      // wait for slave to finish previous test
      while(syncSlave->waitForSyncDone)
        ScribbleApp::processEvents();
      if(ii >= totaltests) {
        syncSlave->scribbleDoc->newDocument();  // disconnect sync
        break;
      }
      // create whiteboard
      //SCRIBBLE_LOG("\nRUNNING TEST %d\n\n", ii);
      swbXML.first_child().attribute("name").set_value(fstring("testdoc%d", ii).c_str());
      scribbleDoc->openSharedDoc(scribbleConfig->String("syncServer"), swbXML.first_child(), true);
      // wait until we connect
      while(!scribbleDoc->scribbleSync->isSyncActive())
        ScribbleApp::processEvents();
      // connect the slave
      syncSlave->startSyncTest(ii);
      // wait until slave connects
      while(!syncSlave->scribbleDoc->scribbleSync->isSyncActive())
        ScribbleApp::processEvents();
    }
    else if(ii >= nonsynctests)
      break;

    // array of pointers to member functions, wow...
    (this->*tests[ii])();

    // when running sync test, slave writes output
    if(syncSlave) {
      // check strokemap count
      checkStrokeMap(scribbleDoc, "master (testuser1)");
      continue;
    }

    // distribute transform to stroke points to match old behavior ... remove this later(?)
    distributeTransform(scribbleDoc->document);
    //screenPaint->beginFrame();
    //bookmarkArea->doPaintEvent(screenPaint);
    //screenPaint->endFrame();
    screenPaint->beginFrame();
    scribbleArea->doPaintEvent(screenPaint);
    screenPaint->endFrame();
    // We haven't looked the png output in ages, so stop generating for now
    //screenImg->save(basefile + "_out.png", "png");
    // include dirtyCount in output file as a check on undo system
    int dirtycount = scribbleDoc->document->dirtyCount;
    for(unsigned int pp = 0; pp < scribbleDoc->document->pages.size(); pp++)
      dirtycount += scribbleDoc->document->pages[pp]->dirtyCount;
    scribbleDoc->cfg->set("test_dirtyCount", dirtycount);
    // write output file
    std::string outfile = basefile + "_out.html";
    if(!scribbleDoc->saveDocument(outfile.c_str()))
      SCRIBBLE_LOG("ScribbleTest: error saving %s", outfile.c_str());
    // compare output to reference; tests of one-file-per-page must handle the svg files themselves
    // we've had so many problems with thumbnails that we will count mismatches of those separately
    if(testCompareFiles(outfile.c_str(), (basefile + "_ref.html").c_str()))
      scribbleDoc->document->deleteFiles();  // remove(outfile.c_str());
    else if(testCompareFiles(outfile.c_str(), (basefile + "_ref.html").c_str(), true)) {
      Image refthumb = ScribbleDoc::extractThumbnail((basefile + "_ref.html").c_str());
      Image outthumb = ScribbleDoc::extractThumbnail(outfile.c_str());
      if(outthumb != refthumb) {  //Application::painter->sRGB() &&  && Application::glRender
        nThumbsFailed++;
        std::ofstream refstrm((basefile + "_ref.png").c_str(), std::ios::binary);
        auto refenc = refthumb.encodePNG();
        refstrm.write((char*)refenc.data(), refenc.size());
        std::ofstream outstrm((basefile + "_out.png").c_str(), std::ios::binary);
        auto outenc = outthumb.encodePNG();
        outstrm.write((char*)outenc.data(), outenc.size());
        std::ofstream diffstrm((basefile + "_diff.png").c_str(), std::ios::binary);
        auto diffenc = outthumb.subtract(refthumb, 10, 0).encodePNG();
        diffstrm.write((char*)diffenc.data(), diffenc.size());
      }
    }
    else {
      slFailed.push_back(std::to_string(ii));
      nFailed++;
    }
  }
  // run after the testN loop so it cannot perturb their document/undo state
  nUnitFailed += shapeRoundTripTest();
  nUnitFailed += shapeInterruptTest();
  nUnitFailed += themeRoundTripTest();
  nUnitFailed += restyleTest();
  nUnitFailed += dashStyleTest();
  nUnitFailed += outlineTest();
  nUnitFailed += outlineNestTest();
  nUnitFailed += notefulImportTest();
  nUnitFailed += notefulArchiveTest();
  nUnitFailed += pdfImportTest();
  nUnitFailed += layerTest();
  nUnitFailed += selectTouchingTest();
  nUnitFailed += twoFingerTapTest();
  nUnitFailed += pageTagTest();
  nUnitFailed += docStateSyncTest();
  nUnitFailed += curveFitTest();
  nUnitFailed += shapeSnapTest();
  nUnitFailed += rulingRegionTest();
  nUnitFailed += reflowIndentTest();
  nUnitFailed += skippedLinesTest();
  runAllTime = mSecSinceEpoch() - runAllTime;
  // restore global config
  srandpp(mSecSinceEpoch());
  SvgWriter::DEFAULT_PATH_DATA_REL = true;
  Element::SVG_NO_TIMESTAMP = false;
  ScribbleView::unitsPerPx = unitsPerPx;
  ScribbleApp::app->loadConfig();
  if(syncSlave)
    nFailed = syncSlave->nFailed;
  nFailed += nUnitFailed;
  resultStr = fstring(
      "Tests completed in %d ms with %d failed tests (%s), %d failed thumbnails and %d failed unit checks.",
      int(runAllTime), nFailed, joinStr(slFailed, ", ").c_str(), nThumbsFailed, nUnitFailed);
  //if(!Application::painter->sRGB() || !Application::glRender)
  //  resultStr += "\nWARNING: ScribbleTest requires GL render and sRGB=1 to get correct thumbnails!";
  if(exitAfterTest) {
    SCRIBBLE_LOG(resultStr.c_str());
    exit(nFailed * 0x100 + nThumbsFailed);
  }
}

void ScribbleTest::performanceTest()
{
  scribbleArea->gotoPos(0, Point(0,0));
  scribbleMode->setMode(MODE_STROKE);
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 1, ScribblePen::TIP_FLAT | ScribblePen::WIDTH_PR, 1.0, 2.0));
  scribbleArea->frameCount = 0;
  Timestamp t0 = mSecSinceEpoch();

  // calibration - allocate 64 MB to ensure buffer doesn't fit in cache
  const int bufferSize = 8192*8192;
  Dim* calBuffer = new Dim[8192*8192];
  for(int ii = 0; ii < bufferSize; ++ii)
    calBuffer[ii] = Dim(ii*4 + 5);
  for(int ii = 0; ii < bufferSize; ++ii)
    calBuffer[ii] = sqrt(calBuffer[bufferSize-ii-1]*2.3 + 3.4);
  delete[] calBuffer;

  Timestamp t1 = mSecSinceEpoch();
  int calibrationTime = t1 - t0;
  t0 = t1;

  // draw many strokes
  for(Dim y = 20; y < 800; y += 20) {
    for(Dim x = 20; x < 800; x += 20) {
      ScribbleApp::processEvents();
      s3(x, y);
      scribbleArea->doRefresh();
    }
  }
  t1 = mSecSinceEpoch();
  int drawTime = t1 - t0;
  t0 = t1;
  int drawFrames = scribbleArea->frameCount;
  scribbleArea->frameCount = 0;
  // select them all
  doCommand(ID_SELALL);
  // move them around
  for(int ii = 0; ii < 10; ii++) {
    ScribbleApp::processEvents();
    s3(400, 400);
    scribbleArea->doRefresh();
  }
  t1 = mSecSinceEpoch();
  int moveTime = t1 - t0;
  t0 = t1;
  int moveFrames = scribbleArea->frameCount;
  scribbleArea->frameCount = 0;
  // scroll up and down a bunch
  scribbleMode->setMode(MODE_PAN);
  ie(400, 800, 0, pen, press);
  for(int ii = 0; ii < 2000; ii++) {
    // cleverly generate a triangle wave
    ScribbleApp::processEvents();
    ie(400, 16*ABS((ii % 100) - 50), 0, pen);
    scribbleArea->doRefresh();
  }
  ScribbleApp::processEvents();
  ie(0, 0, 1, pen, release);
  scribbleArea->doRefresh();
  ScribbleApp::processEvents();
  t1 = mSecSinceEpoch();
  int panTime = t1 - t0;
  t0 = t1;
  int panFrames = scribbleArea->frameCount;

  // save, then load file
  std::string outfile = std::string(SCRIBBLE_TEST_PATH) + "/perftest.html";
  scribbleDoc->cfg->set("singleFile", true);
  scribbleDoc->saveDocument(outfile.c_str());
  t1 = mSecSinceEpoch();
  int saveTime = t1 - t0;
  t0 = t1;
  scribbleDoc->openDocument(outfile.c_str());
  t1 = mSecSinceEpoch();
  int loadTime = t1 - t0;
  t0 = t1;
  int fileSize = getFileSize(outfile);
  scribbleDoc->document->deleteFiles();   //remove(outfile.c_str());

  resultStr = fstring(
      "Calibration: %d ms\nDraw: %d frames in %d ms (%f FPS)\nMove: %d frames in %d ms (%f FPS)\nPan: %d frames in %d ms (%f FPS)"
      "\nSave: %d bytes in %d ms\nLoad: %d ms", calibrationTime,
      drawFrames, drawTime, (drawFrames*1000.0)/drawTime, moveFrames, moveTime, (moveFrames*1000.0)/moveTime,
      panFrames, panTime, (panFrames*1000.0)/panTime, fileSize, saveTime, loadTime);
}

// input test; have to use touch since Windows only provides InjectTouchInput (not pen input)
void ScribbleTest::inputTest()
{
  scribbleArea->scribbleInput->singleTouchMode = INPUTMODE_DRAW;
  bool win8ok = false;
  bool injok = true;
#ifdef Q_OS_WIN_XXX
  if(QSysInfo::windowsVersion() > QSysInfo::WV_WINDOWS7) {
    QPoint origin = scribbleArea->mapToGlobal(QPoint(0, 0));
    int x = origin.x();
    int y = origin.y();
    injok = ScribbleInput::injectTouch(x + 150, y + 150, 0.25, press) && injok;
    injok = ScribbleInput::injectTouch(x + 200, y + 200, 0.75) && injok;
    injok = ScribbleInput::injectTouch(x + 250, y + 150, 0.5) && injok;
    injok = ScribbleInput::injectTouch(x + 250, y + 150, 0, release) && injok;
    win8ok = true;
  }
#endif
  if(!win8ok) {
    ie(150, 150, 0.25, INPUTSOURCE_TOUCH, press);
    ie(200, 200, 0.75, INPUTSOURCE_TOUCH);
    ie(250, 150, 0.5, INPUTSOURCE_TOUCH);
    ie(250, 150, 0, INPUTSOURCE_TOUCH, release);
  }
  ie(150, 150, 0.25, INPUTSOURCE_TOUCH, press);
  ie(200, 100, 0.75, INPUTSOURCE_TOUCH);
  ie(250, 150, 0.5, INPUTSOURCE_TOUCH);
  ie(250, 150, 0, INPUTSOURCE_TOUCH, release);
  scribbleArea->scribbleInput->singleTouchMode = INPUTMODE_NONE;
  // this attempted stroke should NOT appear
  ie(150, 200, 0.25, INPUTSOURCE_TOUCH, press);
  ie(200, 150, 0.75, INPUTSOURCE_TOUCH);
  ie(250, 200, 0.5, INPUTSOURCE_TOUCH);
  ie(250, 200, 0, INPUTSOURCE_TOUCH, release);

  resultStr = fstring("A diamond should have been drawn. Windows 8 touch injection OK: %s", (win8ok && injok) ? "true" : "false");
}

// long stroke
void ScribbleTest::s3(Dim xoffset, Dim yoffset)
{
  static const Dim xang[] = {1, 0, -1, 0};
  static const Dim yang[] = {0, -1, 0, 1};
  ie(xoffset, yoffset, 1, pen, press);
  for(int ii = 1; ii <= 100; ii++)
    ie(xoffset + ii*xang[ii % 4]/10.0, yoffset + ii*yang[ii % 4]/10.0, 1, pen);
  ie(0, 0, 1, pen, release);
}

void ScribbleTest::ie(Dim x, Dim y, Dim p, int src, int ev, int mm)
{
  scribbleArea->scribbleInput->doInputEvent(x, y, p, (inputsource_t)src, (inputevent_t)ev, mm, 0);
}

void ScribbleTest::mtinput(inputevent_t ev1, Dim x1, Dim y1, inputevent_t ev2, Dim x2, Dim y2)
{
  InputEvent ievent(INPUTSOURCE_TOUCH, MODEMOD_NONE);
  if(ev1 != INPUTEVENT_NONE)
    ievent.points.push_back(InputPoint(ev1, x1, y1, 1));  // pressure = 1
  if(ev2 != INPUTEVENT_NONE)
    ievent.points.push_back(InputPoint(ev2, x2, y2, 1));
  scribbleArea->scribbleInput->doInputEvent(ievent);
}

// draw a simple stroke
void ScribbleTest::ss(Dim offset)
{
  ie(104.4 + offset, 184.4 + offset, 0, pen, press);
  ie(124.5 + offset, 162.2 + offset, 0, pen);
  ie(131.8 + offset, 167.3 + offset, 0, pen);
  ie(0, 0, 0, pen, release);
}

void ScribbleTest::s1(Dim xoffset, Dim yoffset)
{
  ie(104.4 + xoffset, 184.4 + yoffset, 0, pen, press);
  ie(124.5 + xoffset, 162.2 + yoffset, 0, pen);
  ie(131.8 + xoffset, 167.3 + yoffset, 0, pen);
  ie(0, 0, 0, pen, release);
}

// stroke centered on (0,0)
void ScribbleTest::s2(Dim xoffset, Dim yoffset)
{
  ie(10 + xoffset, -14.8 + yoffset, 0, pen, press);
  ie(-9.9 + xoffset, 0.1 + yoffset, 0, pen);
  ie(9.8 + xoffset, 15.1 + yoffset, 0, pen);
  ie(0, 0, 0, pen, release);
}

// Filled stroke similar to s2
void ScribbleTest::f2(Dim xoffset, Dim yoffset)
{
  ScribblePen oldpen = *scribbleDoc->app->getPen();
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 1.2, ScribblePen::TIP_FLAT | ScribblePen::WIDTH_PR, 1.0, 2.0));
  ie(10 + xoffset, -14.8 + yoffset, 0.3, pen, press);
  ie(-9.9 + xoffset, 0.1 + yoffset, 0.7, pen);
  ie(9.8 + xoffset, 15.1 + yoffset, 0.5, pen);
  ie(0, 0, 0, pen, release);
  scribbleDoc->app->setPen(oldpen);
}

// MultiStroke (HyperRef) consisting of two strokes
void ScribbleTest::hr(Dim xoffset, Dim yoffset)
{
  ie(-10.1 + xoffset, -14.8 + yoffset, 0, pen, press);
  ie(9.8 + xoffset, 0.1 + yoffset, 0, pen);
  ie(-9.7 + xoffset, 15.1 + yoffset, 0, pen);
  ie(0, 0, 0, pen, release);

  ie(-10 + 10 + xoffset, -13.8 + yoffset, 0, pen, press);
  ie(9.9 + 10 + xoffset, 0.1 + yoffset, 0, pen);
  ie(-9.8 + 10 + xoffset, 12.4 + yoffset, 0, pen);
  ie(0, 0, 0, pen, release);

  scribbleMode->setMode(MODE_SELECTRECT);
  ie(-12 + xoffset, -17 + yoffset, 0, pen, press);
  ie(xoffset, yoffset, 0, pen);
  ie(22 + xoffset, 17 + yoffset, 0, pen);
  ie(0, 0, 0, pen, release);
  scribbleArea->createHyperRef("http://www.styluslabs.com");
  // have to clear the selection ourselves now
  scribbleArea->clearSelection();
}

void ScribbleTest::s3()
{
  ie(213.000, 51.000, 0.200, 1, 1, 0);
  ie(211.000, 51.000, 0.250, 1, 0, 0);
  ie(210.000, 51.000, 0.320, 1, 0, 0);
  ie(206.000, 52.000, 0.390, 1, 0, 0);
  ie(201.000, 54.000, 0.420, 1, 0, 0);
  ie(194.000, 57.000, 0.450, 1, 0, 0);
  ie(188.000, 60.000, 0.440, 1, 0, 0);
  ie(182.000, 64.000, 0.370, 1, 0, 0);
  ie(179.000, 66.000, 0.390, 1, 0, 0);
  ie(175.000, 68.000, 0.310, 1, 0, 0);
  ie(174.000, 69.000, 0.280, 1, 0, 0);
  ie(173.000, 70.000, 0.220, 1, 0, 0);
  ie(172.000, 71.000, 0.210, 1, 0, 0);
  ie(172.000, 72.000, 0.170, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
  ie(176.000, 46.000, 0.190, 1, 1, 0);
  ie(178.000, 47.000, 0.260, 1, 0, 0);
  ie(181.000, 51.000, 0.330, 1, 0, 0);
  ie(185.000, 56.000, 0.370, 1, 0, 0);
  ie(190.000, 63.000, 0.420, 1, 0, 0);
  ie(194.000, 68.000, 0.390, 1, 0, 0);
  ie(197.000, 72.000, 0.340, 1, 0, 0);
  ie(202.000, 78.000, 0.250, 1, 0, 0);
  ie(203.000, 80.000, 0.280, 1, 0, 0);
  ie(205.000, 82.000, 0.230, 1, 0, 0);
  ie(205.000, 83.000, 0.190, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
}

void ScribbleTest::s4()
{
  ie(217.000, 46.000, 0.200, 1, 1, 0);
  ie(215.000, 46.000, 0.250, 1, 0, 0);
  ie(212.000, 48.000, 0.320, 1, 0, 0);
  ie(209.000, 51.000, 0.390, 1, 0, 0);
  ie(203.000, 55.000, 0.420, 1, 0, 0);
  ie(198.000, 58.000, 0.450, 1, 0, 0);
  ie(192.000, 62.000, 0.440, 1, 0, 0);
  ie(190.000, 63.000, 0.370, 1, 0, 0);
  ie(187.000, 66.000, 0.390, 1, 0, 0);
  ie(184.000, 68.000, 0.310, 1, 0, 0);
  ie(181.000, 71.000, 0.280, 1, 0, 0);
  ie(178.000, 72.000, 0.220, 1, 0, 0);
  ie(174.000, 75.000, 0.210, 1, 0, 0);
  ie(172.000, 76.000, 0.170, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
  ie(179.000, 54.000, 0.200, 1, 1, 0);
  ie(180.000, 54.000, 0.250, 1, 0, 0);
  ie(181.000, 54.000, 0.320, 1, 0, 0);
  ie(182.000, 55.000, 0.390, 1, 0, 0);
  ie(184.000, 58.000, 0.420, 1, 0, 0);
  ie(187.000, 62.000, 0.450, 1, 0, 0);
  ie(192.000, 68.000, 0.440, 1, 0, 0);
  ie(194.000, 71.000, 0.370, 1, 0, 0);
  ie(198.000, 75.000, 0.390, 1, 0, 0);
  ie(201.000, 80.000, 0.310, 1, 0, 0);
  ie(205.000, 82.000, 0.280, 1, 0, 0);
  ie(208.000, 84.000, 0.220, 1, 0, 0);
  ie(210.000, 85.000, 0.210, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
}

void ScribbleTest::test0()
{
  //scribbleArea->gotoPos(0, Point(0,0));
  ie(104.4, 184.4, 0, pen, press);
  ie(124.5, 162.2, 0, pen);
  ie(131.8, 167.3, 0, pen);
  ie(0, 0, 0, pen, release);

  scribbleMode->setMode(MODE_INSSPACERULED);
  ie(14.4, 56.4, 0, pen, press);
  ie(16.5, 85.2, 0, pen);
  ie(16.5, 99.9, 0, pen);
  ie(15.8, 112.3, 0, pen);
  ie(0, 0, 0, pen, release);

  // pinch zoom (will also test rounding to nearest zoom level)
  mtinput(INPUTEVENT_PRESS, 120, 140, INPUTEVENT_NONE, 0, 0);
  mtinput(INPUTEVENT_MOVE, 135, 128, INPUTEVENT_NONE, 0, 0);
  mtinput(INPUTEVENT_MOVE, 132, 125, INPUTEVENT_PRESS, 345, 327);
  mtinput(INPUTEVENT_MOVE, 144, 137, INPUTEVENT_MOVE, 340, 317);
  mtinput(INPUTEVENT_MOVE, 200, 197, INPUTEVENT_MOVE, 290, 295);
  mtinput(INPUTEVENT_MOVE, 208, 204, INPUTEVENT_MOVE, 278, 275);  // zoom step >2 or <0.5 now cancels zoom
  mtinput(INPUTEVENT_MOVE, 216, 210, INPUTEVENT_MOVE, 264, 256);
  mtinput(INPUTEVENT_RELEASE, 216, 210, INPUTEVENT_MOVE, 262, 256);
  mtinput(INPUTEVENT_NONE, 0, 0, INPUTEVENT_MOVE, 260, 254);
  mtinput(INPUTEVENT_NONE, 0, 0, INPUTEVENT_MOVE, 260, 246);
  mtinput(INPUTEVENT_NONE, 0, 0, INPUTEVENT_RELEASE, 260, 246);

  // test cancellation of touch
  mtinput(INPUTEVENT_PRESS, 120, 140, INPUTEVENT_NONE, 0, 0);
  mtinput(INPUTEVENT_MOVE, 135, 128, INPUTEVENT_NONE, 0, 0);
  mtinput(INPUTEVENT_MOVE, 132, 125, INPUTEVENT_PRESS, 345, 327);
  mtinput(INPUTEVENT_MOVE, 144, 137, INPUTEVENT_MOVE, 340, 317);
  // this doesn't do anything - rather the pen input from s2(...) call cancels and starts drawing
  //mtinput(INPUTEVENT_CANCEL, 200, 197, INPUTEVENT_CANCEL, 290, 295);

  // draw something else
  s2(301, 402);
}

// similar to test0, but loads file test1_in.html - a damaged (truncated) file to test error robustness
void ScribbleTest::test1()
{
  ie(104.4, 184.4, 0, pen, press);
  ie(114.5, 172.2, 0, pen);
  ie(121.8, 177.3, 0, pen);
  ie(0, 0, 0, pen, release);

  scribbleMode->setMode(MODE_INSSPACERULED);
  ie(14.4, 56.4, 0, pen, press);
  ie(16.5, 86.2, 0, pen);
  ie(15.8, 113.3, 0, pen);
  ie(0, 0, 0, pen, release);
}

// draw an "X", select and move it, draw another "X"
void ScribbleTest::test2()
{
  ie(104.4, 184.4, 0, 1, 1, 0);
  ie(114.5, 172.2, 0, 1, 0, 0);
  ie(121.8, 177.3, 0, 1, 0, 0);
  ie(0, 0, 0, 1, -1, 0);
  undo();
  s3();
  ie(154.000, 29.000, 0.000, 1, 1, 2);
  ie(154.000, 30.000, 0.000, 1, 0, 0);
  ie(155.000, 30.000, 0.000, 1, 0, 0);
  ie(157.000, 33.000, 0.000, 1, 0, 0);
  ie(160.000, 36.000, 0.000, 1, 0, 0);
  ie(167.000, 46.000, 0.000, 1, 0, 0);
  ie(174.000, 55.000, 0.000, 1, 0, 0);
  ie(188.000, 67.000, 0.000, 1, 0, 0);
  ie(195.000, 76.000, 0.000, 1, 0, 0);
  ie(203.000, 85.000, 0.000, 1, 0, 0);
  ie(212.000, 96.000, 0.000, 1, 0, 0);
  ie(216.000, 102.000, 0.000, 1, 0, 0);
  ie(221.000, 108.000, 0.000, 1, 0, 0);
  ie(224.000, 112.000, 0.000, 1, 0, 0);
  ie(227.000, 115.000, 0.000, 1, 0, 0);
  ie(228.000, 117.000, 0.000, 1, 0, 0);
  ie(229.000, 117.000, 0.000, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
  ie(193.000, 65.000, 0.000, 1, 1, 0);
  ie(194.000, 65.000, 0.000, 1, 0, 0);
  ie(197.000, 68.000, 0.000, 1, 0, 0);
  ie(204.000, 73.000, 0.000, 1, 0, 0);
  ie(214.000, 81.000, 0.000, 1, 0, 0);
  ie(228.000, 90.000, 0.000, 1, 0, 0);
  ie(261.000, 116.000, 0.000, 1, 0, 0);
  ie(294.000, 142.000, 0.000, 1, 0, 0);
  ie(329.000, 166.000, 0.000, 1, 0, 0);
  ie(387.000, 218.000, 0.000, 1, 0, 0);
  ie(425.000, 257.000, 0.000, 1, 0, 0);
  ie(483.000, 315.000, 0.000, 1, 0, 0);
  ie(509.000, 363.000, 0.000, 1, 0, 0);
  ie(544.000, 438.000, 0.000, 1, 0, 0);
  ie(559.000, 476.000, 0.000, 1, 0, 0);
  ie(569.000, 535.000, 0.000, 1, 0, 0);
  ie(569.000, 562.000, 0.000, 1, 0, 0);
  ie(559.000, 602.000, 0.000, 1, 0, 0);
  ie(549.000, 625.000, 0.000, 1, 0, 0);
  ie(534.000, 649.000, 0.000, 1, 0, 0);
  ie(524.000, 663.000, 0.000, 1, 0, 0);
  ie(509.000, 683.000, 0.000, 1, 0, 0);
  ie(502.000, 693.000, 0.000, 1, 0, 0);
  ie(491.000, 707.000, 0.000, 1, 0, 0);
  ie(486.000, 714.000, 0.000, 1, 0, 0);
  ie(483.000, 719.000, 0.000, 1, 0, 0);
  ie(480.000, 723.000, 0.000, 1, 0, 0);
  ie(479.000, 726.000, 0.000, 1, 0, 0);
  ie(478.000, 728.000, 0.000, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
  ie(176.000, 70.000, 0.000, 1, 1, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
  s4();
}

// this test is mainly intended for finding memory leaks (so run it under valgrind)
// also, this test will now fail if undo does not preserve z-order
void ScribbleTest::test3()
{
  ss(0);
  hr(300, 300);
  ss(5);
  doCommand(ID_SELALL);
  doCommand(ID_DELSEL);
  undo();
  undo();
  ss(10);
  ss(15);
  undo();
  ss(20);
  doCommand(ID_SELALL);
  doCommand(ID_COPYSEL);
  doCommand(ID_DELSEL);
  ss(30);
  ss(40);
  doCommand(ID_SELALL);
  doCommand(ID_COPYSEL);
  doCommand(ID_PASTE);
  ss(50);
  undo();
  undo();
  doCommand(ID_PASTE);
  doCommand(ID_DELSEL);
  ss(60);
  undo();
  undo();
  ss(70);
  doCommand(ID_PASTE);
  doCommand(ID_DELSEL);
  undo();
  undo();
  ss(80);
  // none of the tests actually covered pasted strokes - there was a SWB bug
  doCommand(ID_PASTE);
}

// test deletion of page with active selection
void ScribbleTest::test4()
{
  ss(0);
  doCommand(ID_NEXTPAGENEW);
  ss(10);
  ss(20);
  doCommand(ID_SELALL);
  doCommand(ID_COPYSEL);
  doCommand(ID_DELPAGE);
  undo();
  // delete strokes from page, then undo that - added in response to SWB bug
  doCommand(ID_SELALL);
  doCommand(ID_DELSEL);
  undo();

  doCommand(ID_NEXTPAGE);  // make sure we're on second page
  //doCommand(ID_PASTE);
  scribbleMode->setMode(MODE_PAGESEL);
  // tap on page
  ie(300, 400, 0, pen, press);
  ie(301, 399, 0, pen);
  ie(0, 0, 0, pen, release);
  doCommand(ID_CUTSEL);

  doCommand(ID_NEXTSCREEN);  // want to paste after first page
  doCommand(ID_PASTE);
  doCommand(ID_PASTE);
  scribbleArea->gotoPage(0);  // restore view
}

// test setting page properties and growing page
void ScribbleTest::test5()
{
  PageProperties props(600, 800, 30, 30, 30, Color::YELLOW, Color(0, 0, 0xFF, 0x7F));
  ss(0);
  ss(10);
  scribbleDoc->setPageProperties(&props, false, true, false);
  ss(20);
  // add new page
  doCommand(ID_NEXTPAGENEW);
  ss(0);
  // grow new page ... off page strokes can't grow page any more, so delete afterwards!
  ie(104.4, 900, 0, pen, press);
  ie(124.5, 803, 0, pen);
  ie(131.8, 900, 0, pen);
  ie(0, 0, 0, pen, release);
  scribbleArea->recentStrokeSelect();
  doCommand(ID_DELSEL);
  ss(20);
}

// test opening file (test6_in.html) to saved position and lasso selection
void ScribbleTest::test6()
{
  // make a lasso selection
  scribbleMode->setMode(MODE_SELECTLASSO);
  ie(475.000, 199.000, 0.000, 1, 1, 0);
  ie(450.000, 175.000, 0.000, 1, 0, 0);
  ie(329.000, 269.000, 0.000, 1, 0, 0);
  ie(435.000, 386.000, 0.000, 1, 0, 0);
  ie(553.000, 268.000, 0.000, 1, 0, 0);
  ie(516.000, 240.000, 0.000, 1, 0, 0);
  ie(436.000, 317.000, 0.000, 1, 0, 0);
  ie(387.000, 269.000, 0.000, 1, 0, 0);
  ie(452.000, 215.000, 0.000, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);
  // tap selection to switch to ruled mode
  ie(442, 262, 0, pen, press);
  ie(441, 262, 0, pen);
  ie(0, 0, 0, pen, release);
  // now move the selection
  ie(441.000, 263.000, 0.000, 1, 1, 0);
  ie(432.000, 319.000, 0.000, 1, 0, 0);
  ie(431.000, 326.000, 0.000, 1, 0, 0);
  ie(431.000, 342.000, 0.000, 1, 0, 0);
  ie(429.000, 399.000, 0.000, 1, 0, 0);
  ie(429.000, 407.000, 0.000, 1, 0, 0);
  ie(424.000, 454.000, 0.000, 1, 0, 0);
  ie(425.000, 473.000, 0.000, 1, 0, 0);
  ie(429.000, 476.000, 0.000, 1, 0, 0);
  ie(0.000, 0.000, 0.000, 1, -1, 0);

  // tests added for dup sel (and for resaving page containing jpeg image)
  doCommand(ID_NEXTPAGE);
  // ruled select in left margin
  scribbleMode->setMode(MODE_SELECTRULED);
  ie(24, 61, 0, pen, press);
  ie(23, 139, 0, pen);
  ie(0, 0, 0, pen, release);
  doCommand(ID_DUPSEL);
  doCommand(ID_PREVPAGE);
}

// test insert space reflow
void ScribbleTest::test7()
{
  // easier to figure out what's going on if screen space and Dim space are the same
  scribbleArea->gotoPos(0, Point(0,0));
  PageProperties props(700, 800, 0, 40, 100);
  scribbleDoc->setPageProperties(&props, false, true, false);
  // create 3 lines of "text";  note: width of ss is 27.4
  for(int offset = 15; offset < 500; offset += 139) {
    s1(offset, 0);
    s1(offset + 30, 0);
    s1(offset + 60, 0);
    s1(offset + 90, 0);
  }
  for(int offset = 5; offset < 500; offset += 141) {
    s1(offset, 40);
    s1(offset + 30, 40);
    s1(offset + 60, 40);
    s1(offset + 90, 40);
  }
  for(int offset = 10; offset < 500; offset += 140) {
    s1(offset, 80);
    s1(offset + 30, 80);
    s1(offset + 60, 80);
    s1(offset + 90, 80);
  }
  // insert space on first line
  scribbleMode->setMode(MODE_INSSPACERULED);
  ie(102, 180, 0, pen, press);
  ie(200, 180, 0, pen);
  ie(310, 180, 0, pen);
  ie(0, 0, 0, pen, release);

  // insert some more space, then undo
  scribbleMode->setMode(MODE_INSSPACERULED);
  ie(102, 180, 0, pen, press);
  ie(200, 180, 0, pen);
  ie(310, 180, 0, pen);
  ie(0, 0, 0, pen, release);
  undo();

  // test non-ruled insert space
  s1(0, 280);
  s1(30, 280);
  s1(60, 280);
  s1(90, 280);
  scribbleMode->setMode(MODE_INSSPACEVERT);
  ie(102, 420, 0, pen, press);
  ie(103, 440, 0, pen);
  ie(105, 470, 0, pen);
  ie(0, 0, 0, pen, release);
}

// test ruled select and moving strokes between pages
void ScribbleTest::test8()
{
  PageProperties props(700, 800, 0, 40, 100);
  scribbleDoc->setPageProperties(&props, false, true, false);
  // add new page
  s2(400,60);
  doCommand(ID_NEXTPAGENEW);
  scribbleArea->gotoPos(0, Point(0,0));
  doCommand(ID_SELALL);
  doCommand(ID_DELSEL);
  s2(200, 380);
  s2(150, 420);
  s2(200, 420);
  f2(250, 420);
  s2(600, 420);
  s2(200, 460);
  scribbleMode->setMode(MODE_SELECTRULED);
  ie(110, 420, 0, pen, press);
  ie(275, 420, 0, pen);
  ie(350, 420, 0, pen);
  ie(0, 0, 0, pen, release);
  // hack to workaround new behavior that dropping selection outside screen area will not move
  scribbleArea->screenRect = Rect::ltwh(0, 0, 600, 1400);
  // drag selection to second page
  ie(200, 420, 0, pen, press);
  ie(210, 750, 0, pen);
  ie(215, 1300 - 40, 0, pen);  // -40 accounts for change of interpage gap from 60 to 20
  ie(0, 0, 0, pen, release);
  scribbleArea->screenRect = screenRect;
  // put pen down to clear selection
  s2(500, 1300);

  // return to first page and test erasers
  scribbleArea->gotoPos(0, Point(0,0));
  s2(200, 60);
  s2(150, 100);
  s2(200, 100);
  s2(250, 100);
  s2(600, 100);
  s2(200, 140);
  scribbleMode->setMode(MODE_ERASERULED);
  ie(110, 100, 0, pen, press);
  ie(150, 100, 0, pen);
  ie(200, 100, 0, pen);
  ie(250, 130, 0, pen);
  ie(150, 130, 0, pen);
  ie(0, 0, 0, pen, release);
  scribbleMode->setMode(MODE_ERASESTROKE);
  ie(599, 62, 0, pen, press);
  ie(600, 100, 0, pen);
  ie(602, 139, 0, pen);
  ie(0, 0, 0, pen, release);
}

// test ruled mode tools on unruled page
void ScribbleTest::test9()
{
  scribbleArea->gotoPos(0, Point(0,0));
  PageProperties props(700, 800, 0, 0, 0);
  scribbleDoc->setPageProperties(&props, false, true, false);
  // create 4 lines of "text"
  for(int offset = 125; offset < 600; offset += 125) {
    s2(offset, 41);
    s2(offset + 25, 37);
    s2(offset + 50, 42);
    s2(offset + 75, 40);
  }
  for(int offset = 125; offset < 600; offset += 118) {
    s2(offset, 82);
    s2(offset + 25, 79);
    s2(offset + 50, 77);
    s2(offset + 75, 81);
  }
  for(int offset = 115; offset < 600; offset += 121) {
    s2(offset, 118);
    s2(offset + 25, 117);
    s2(offset + 50, 123);
    s2(offset + 75, 120);
  }
  for(int offset = 120; offset < 600; offset += 120) {
    s2(offset, 160);
    s2(offset + 26, 161);
    s2(offset + 53, 157);
    s2(offset + 76, 159);
  }
  // add a bookmark
  scribbleMode->setMode(MODE_BOOKMARK);
  ie(135, 110, 0, pen, press);
  ie(106, 140, 0, pen);
  ie(60, 160, 0, pen);
  ie(0, 0, 0, pen, release);
  // insert space
  scribbleMode->setMode(MODE_INSSPACERULED);
  ie(235, 110, 0, pen, press);
  ie(206, 140, 0, pen);
  ie(184, 195, 0, pen);
  ie(0, 0, 0, pen, release);
  // ruled erase
  scribbleMode->setMode(MODE_ERASERULED);
  ie(60, 90, 0, pen, press);
  ie(140, 92, 0, pen);
  ie(200, 87, 0, pen);
  ie(290, 86, 0, pen);
  ie(385, 89, 0, pen);
  ie(0, 0, 0, pen, release);

  // drop a bookmark off page, then undo - added because this used to cause crash on undo
  scribbleMode->setMode(MODE_BOOKMARK);
  ie(50, 50, 0, pen, press);
  ie(20, 20, 0, pen);
  ie(-100, -100, 0, pen);
  ie(0, 0, 0, pen, release);
  undo();
  redo();

  // must refresh bookmarks so that findBookmark will work
  //bookmarkArea->repaintBookmarks();
  screenPaint->beginFrame();  bookmarkArea->doPaintEvent(screenPaint);  screenPaint->endFrame();

  doCommand(ID_NEXTPAGENEW);
  scribbleArea->gotoPos(1, Point(-10,-10));  // account for previous behavior of NEXTPAGE
  // create hyperref, then convert (along with another stroke) to point to bookmark on first page
  hr(150, 100);
  s2(200, 100);
  doCommand(ID_SELALL);
  scribbleArea->createHyperRef(bookmarkArea->findBookmark(scribbleDoc->document, 20));

  // add bookmark to 2nd page ... id should not be serialized for this one
  scribbleMode->setMode(MODE_BOOKMARK);
  ie(135, 150, 0, pen, press);
  ie(106, 180, 0, pen);
  ie(65, 200, 0, pen);
  ie(0, 0, 0, pen, release);

  // return to first page and copy the bookmark - the was crashing before we fixed clone bug with idStr
  doCommand(ID_PREVPAGE);
  doCommand(ID_SELALL);
  doCommand(ID_COPYSEL);
  doCommand(ID_PASTE);
  doCommand(ID_DELSEL);
  // make sure *cut* and paste preserves id
  doCommand(ID_SELALL);
  doCommand(ID_CUTSEL);
  doCommand(ID_PASTE);
  doCommand(ID_NEXTPAGENEW);
}

// test changing properties and ins space ruled with hyperref
void ScribbleTest::test10()
{
  // test creating new doc with different view modes (added because of crash)
  if(!syncSlave) {
    scribbleConfig->set("viewMode", 0);
    scribbleDoc->newDocument();
    scribbleConfig->set("viewMode", 2);
    scribbleDoc->newDocument();
    scribbleConfig->set("viewMode", 1);
    scribbleDoc->newDocument();
  }

  scribbleArea->gotoPos(0, Point(0,0));
  hr(150, 100);
  s2(200, 100);
  f2(250, 100);
  doCommand(ID_SELALL);

  scribbleDoc->activeArea->setStrokeProperties(StrokeProperties(Color::BLUE, -1));
  scribbleDoc->activeArea->setStrokeProperties(StrokeProperties(Color::INVALID_COLOR, 2));
  scribbleDoc->activeArea->setStrokeProperties(StrokeProperties(Color::INVALID_COLOR, 3));
  undo();

  // added due to bug: failed to create undo items for translated multistrokes
  scribbleMode->setMode(MODE_INSSPACERULED);
  ie(104, 100, 0, pen, press);
  ie(195, 98, 0, pen);
  ie(244, 103, 0, pen);
  ie(0, 0, 0, pen, release);
  undo();

  // Note pen not longer changed when selection active
  scribbleMode->setMode(MODE_INSSPACERULED);
  ie(104, 100, 0, pen, press);
  ie(195, 98, 0, pen);
  ie(298, 103, 0, pen);
  ie(0, 0, 0, pen, release);
}

// test column detection and free erase
void ScribbleTest::test11()
{
  scribbleArea->gotoPos(0, Point(0,0));
  PageProperties props(1200, 800, 0, 40, 100);
  scribbleDoc->setPageProperties(&props, false, true, false);
  // create 2 columns with 4 lines of "text" each
  for(int col = 0; col < 1000; col += 550) {
    for(int offset = 125; offset < 600; offset += 125) {
      s2(col + offset, 61);
      s2(col + offset + 25, 67);
      s2(col + offset + 50, 62);
      s2(col + offset + 75, 60);
    }
    for(int offset = 130; offset < 600; offset += 119) {
      s2(col + offset, 102);
      s2(col + offset + 25, 99);
      s2(col + offset + 50, 97);
      s2(col + offset + 75, 101);
    }
    for(int offset = 125; offset < 600; offset += 121) {
      s2(col + offset, 138);
      s2(col + offset + 25, 137);
      s2(col + offset + 50, 143);
      s2(col + offset + 75, 140);
    }
    for(int offset = 120; offset < 600; offset += 120) {
      s2(col + offset, 180);
      s2(col + offset + 26, 181);
      s2(col + offset + 53, 177);
      s2(col + offset + 76, 179);
    }
  }
  // need dense points
  //PointDensityFilter::MAX_POINT_DIST = 2;
  // draw column divider - points need to be dense and we've disabled MAX_POINT_DIST
  ie(640, 20, 0, pen, press);
  ie(605, 220, 0, pen);
  ie(0, 0, 0, pen, release);

  // insert space
  scribbleMode->setMode(MODE_INSSPACERULED);
  ie(225, 62, 0, pen, press);
  ie(350, 64, 0, pen);
  ie(505, 63, 0, pen);
  ie(0, 0, 0, pen, release);

  // apply free eraser to stroke, filled stroke, hyperref
  // ... also want a stroke which will be broken into > 2 pieces
  ie(140, 420, 0, pen, press);
  ie(145, 340, 0, pen);
  ie(150, 420, 0, pen);
  ie(155, 340, 0, pen);
  ie(160, 420, 0, pen);
  ie(0, 0, 0, pen, release);
  s2(200, 379);
  f2(250, 382);
  hr(300, 381);
  scribbleMode->setMode(MODE_ERASEFREE);
  ie(100, 380, 0, pen, press);
  ie(140, 380, 0, pen);
  ie(200, 380, 0, pen);
  ie(290, 380, 0, pen);
  ie(400, 380, 0, pen);
  ie(0, 0, 0, pen, release);
  //PointDensityFilter::MAX_POINT_DIST = 1000;
}

// test scaling ... test12_in.html includes an image
void ScribbleTest::test12()
{
  // easier to figure out what's going on if screen space and Dim space are the same
  scribbleArea->gotoPos(0, Point(0,0));
  // test added because of crash
  scribbleMode->setMode(MODE_BOOKMARK);
  ie(135, 110, 0, pen, press);
  ie(106, 100, 0, pen);
  ie(60, 60, 0, pen);
  ie(0, 0, 0, pen, release);
  //scribbleDoc->document->drawBookmarks(screenPaint, Rect());
  screenPaint->beginFrame();  bookmarkArea->doPaintEvent(screenPaint);  screenPaint->endFrame();
  undo();
  // back to our regularly scheduled programming...
  s2(200, 179);
  f2(250, 182);
  hr(300, 181);
  doCommand(ID_SELALL);
  // tap the selection to change to move sel free mode (since selectAll no longer ignores moveSelMode) ... nevermind
  //ie(300, 160, 0, pen, press);
  //ie(298, 162, 0, pen);
  //ie(0, 0, 0, pen, release);
  // resize selection
  ie(320, 198, 0, pen, press);
  ie(350, 250, 0, pen);
  ie(450, 300, 0, pen);
  ie(0, 0, 0, pen, release);
  // clear selection
  s2(550, 550);
  doCommand(ID_UNDO);
  doCommand(ID_REDO);
  // apply free eraser to image
  scribbleMode->setMode(MODE_ERASEFREE);
  ie(100, 40, 0, pen, press);
  ie(150, 90, 0, pen);
  ie(200, 140, 0, pen);
  ie(0, 0, 0, pen, release);
}

// test13 - test multiple file documents, since these have been the source of the two data loss bugs found so
//  far in Write
void ScribbleTest::test13()
{
  // setup output path
  std::string sfilename = outPath + u8"/test13_\u4E0B\u5348.html";
  const char* filename = sfilename.c_str();

  // create page 1
  scribbleArea->gotoPos(0, Point(0,0));
  s2(100, 100);
  // create page 2
  doCommand(ID_NEXTPAGENEW);
  scribbleArea->gotoPos(1, Point(-10,-10));  // account for previous behavior of NEXTPAGE
  s2(200, 200);
  // create page 3
  doCommand(ID_NEXTPAGENEW);
  scribbleArea->gotoPos(2, Point(-10,-10));  // account for previous behavior of NEXTPAGE
  s2(300, 300);
  // undo and redo page creation ... added because of a crash
  doCommand(ID_UNDO);
  doCommand(ID_UNDO);
  doCommand(ID_REDO);
  doCommand(ID_REDO);
  // return to page 1
  scribbleArea->gotoPos(0, Point(0,0));
  // save
  if(!syncSlave) {
    //scribbleArea->cfg->set("singleFile", false);
    scribbleDoc->saveDocument(filename, Document::SAVE_MULTIFILE);
    // reopen; only page 1 should be loaded
    scribbleDoc->openDocument(filename);
  }

  // change page properties - apply to all
  PageProperties props(0, 0, 30, 30, 30, Color::YELLOW, Color(0, 0, 0xFF, 0x7F));
  scribbleDoc->setPageProperties(&props, true, false, false);
  // save and reopen
  if(!syncSlave) {
    scribbleDoc->saveDocument(filename);
    scribbleDoc->openDocument(filename);
  }

  // insert a new page after page 1
  doCommand(ID_PAGEAFTER);
  scribbleArea->gotoPos(1, Point(-10,-10));  // account for previous behavior of NEXTPAGE
  s2(150, 150);
  // save and reopen
  if(!syncSlave) {
    scribbleDoc->saveDocument(filename);
    scribbleDoc->openDocument(filename);
  }

  // remove page 3 (formerly page 2)
  doCommand(ID_NEXTPAGENEW);
  doCommand(ID_DELPAGE);
  // save and reopen
  if(!syncSlave) {
    scribbleDoc->saveDocument(filename);
    scribbleDoc->openDocument(filename);
    // load all pages before deleting SVG files
    scribbleDoc->document->ensurePagesLoaded();
    // return to single file config for comparision of file result
    //scribbleArea->cfg->set("singleFile", true);
    // remove SVG files
    scribbleDoc->document->deleteFiles();   //ScribbleDoc::deleteDocument(filename);

    // test other file types
    std::string svgfile = outPath + u8"/test13_\u4E0B\u5348.svg";
    scribbleDoc->saveDocument(svgfile.c_str());
    scribbleDoc->openDocument(svgfile.c_str());
    scribbleDoc->document->ensurePagesLoaded();
    scribbleDoc->document->deleteFiles();   //ScribbleDoc::deleteDocument(svgfile.c_str());

    std::string svgzfile = outPath + u8"/test13_\u4E0B\u5348.svgz";
    scribbleDoc->saveDocument(svgzfile.c_str());
    scribbleDoc->openDocument(svgzfile.c_str());
    scribbleDoc->document->ensurePagesLoaded();
    scribbleDoc->document->deleteFiles();   //ScribbleDoc::deleteDocument(svgzfile.c_str());
  }
}

void ScribbleTest::test14()
{
  scribbleMode->moveSelMode = MODE_MOVESELFREE;  // selectAll no longer ignores moveSelMode
  // easier to figure out what's going on if screen space and Dim space are the same
  scribbleArea->gotoPos(0, Point(0,0));

  s2(200, 179);
  f2(250, 182);
  hr(300, 181);
  doCommand(ID_SELALL);
  // test scale selection with one negative and one positive scale factor
  ie(188, 162, 0, pen, press); // top left corner
  ie(350, 162, 0, pen);
  ie(450, 162, 0, pen);
  ie(0, 0, 0, pen, release);

  // test scale w/ stroke width scaling
  ie(450, 198, 0, pen, press, MODEMOD_PENBTN);
  ie(500, 250, 0, pen, INPUTEVENT_MOVE, MODEMOD_PENBTN);
  ie(550, 300, 0, pen, INPUTEVENT_MOVE, MODEMOD_PENBTN);
  ie(0, 0, 0, pen, release);

  // test rotation
  ie(435, 140, 0, pen, press);
  ie(460, 160, 0, pen);
  ie(490, 190, 0, pen);
  ie(0, 0, 0, pen, release);

  // why not...
  doCommand(ID_UNDO);
  doCommand(ID_REDO);

  // test smoothing and simplification
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 1.2, ScribblePen::TIP_FLAT | ScribblePen::WIDTH_PR, 1.0, 2.0));
  scribbleDoc->cfg->set("inputSimplify", 4);
  s3();
  scribbleDoc->cfg->set("inputSmoothing", 4);
  s4();
  scribbleDoc->cfg->set("inputSmoothing", 0);
  scribbleDoc->cfg->set("inputSimplify", 0);
}

// add snap to grid and line drawing tests too!
void ScribbleTest::test15()
{
  // switch to relative path data
  SvgWriter::DEFAULT_PATH_DATA_REL = true;

  scribbleArea->gotoPos(0, Point(0,0));
  s2(200, 179);
  f2(250, 182);
  hr(300, 181);

  // make sure realToStr doesn't write "-0"
  ie(400, 180, 0, pen, press);
  ie(410, 180.0004, 0, pen);
  ie(420, 180, 0, pen);
  ie(0, 0, 0, pen, release);

  // test z-order for highlighter (draw under)
  scribbleDoc->app->setPen(ScribblePen(Color::YELLOW, 20, ScribblePen::TIP_CHISEL | ScribblePen::DRAW_UNDER));
  s2(250, 190);
  undo();
  redo();

  s2(310, 180);
  scribbleMode->setMode(MODE_ERASESTROKE);
  ie(300, 180, 0, pen, press);
  ie(300, 180, 0, pen);
  ie(0, 0, 0, pen, release);
  undo();

  // test of reflow that should fail w/ cmpRuled bug (not sorting strokes on line left-to-right)
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 1, ScribblePen::TIP_ROUND));
  for(int offset = 15; offset < 500; offset += 139) {
    s1(offset + 60, 120);
    s1(offset + 90, 120);
    s1(offset, 120);
    s1(offset + 30, 120);
  }
  // insert space
  scribbleMode->setMode(MODE_INSSPACERULED);
  ie(102, 300, 0, pen, press);
  ie(200, 300, 0, pen);
  ie(310, 300, 0, pen);
  ie(0, 0, 0, pen, release);

  // test erase on negative insert space
  s2(180 + 60, 420);
  s2(180 + 90, 420);
  s2(180,      420);
  s2(180 + 30, 420);

  scribbleMode->setMode(MODE_INSSPACERULED);
  ie(180 + 45, 420, 0, pen, press);
  ie(180, 420, 0, pen);
  ie(140, 420, 0, pen);
  ie(0, 0, 0, pen, release);

  SvgDocument* svgDoc = SvgParser().parseFragment(
      "<g stroke='green' transform='translate(50, 50)'><rect fill='rgba(0, 0, 255, 0.8)' stroke='none' x='0' y='0' width='20' height='20'/>"
      "<path fill='red' d='M10 10 l2 4 1 2 0 3 -1 1 -1 0z'/></g>");
  Clipboard* clip = scribbleDoc->app->importExternalDoc(svgDoc);
  scribbleDoc->app->clipboard.reset(clip);
  scribbleDoc->app->clipboardPage = NULL;

  doCommand(ID_PASTE);
  doCommand(ID_DELSEL);
  undo();
  undo();
  doCommand(ID_PASTE);
}

// back and forth test for whiteboard

void ScribbleTest::waitForSync()
{
  // wait until all items are sent and echoed back from server, then wait until slave has received all items (same number of byyes)
  // alternative, wait until master and slave stroke counts are equal?
  ScribbleSync* ss = scribbleDoc->scribbleSync;
  while(ss->canSendHist() || ss->rcvdPos != ss->sentPos || ss->bytesRcvd > syncSlave->scribbleDoc->scribbleSync->bytesRcvd)
    ScribbleApp::processEvents();
}

void ScribbleTest::synctest01()
{
  scribbleArea->gotoPos(0, Point(0,0));
  hr(150, 100);
  s2(200, 100);
  f2(250, 100);
  doCommand(ID_SELALL);

  waitForSync();
  syncSlave->synctest01slave1();

  // move the selection
  ie(200, 100, 0, pen, press);
  ie(210, 120, 0, pen);
  ie(220, 160, 0, pen);
  ie(0, 0, 0, pen, release);

  waitForSync();
  syncSlave->synctest01slave2();
}

void ScribbleTest::synctest01slave1()
{
  scribbleArea->gotoPos(0, Point(0,0));
  s2(550, 550);
  // apply free eraser to image
  scribbleMode->setMode(MODE_ERASERULED);
  ie(245, 101, 0, pen, press);
  ie(270, 99, 0, pen);
  ie(300, 100, 0, pen);
  ie(0, 0, 0, pen, release);
}

void ScribbleTest::synctest01slave2()
{
  // restore the deleted stroke
  undo();

  // disconnect and reconnect to whiteboard
  startSyncTest(16);
  // wait for connection
  while(!scribbleDoc->scribbleSync->isSyncActive())
    ScribbleApp::processEvents();
}
