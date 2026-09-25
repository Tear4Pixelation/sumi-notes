#include "scribbletest.h"
#include <fstream>

#include "usvg/svgparser.h"
#include "application.h"
#include "strokebuilder.h"
#include "scribblesync.h"
#include "scribbleapp.h"  // only for sync tests

// document scanning math; unlike everything else here it needs neither GL nor a document
#include "scantest.cpp"
#include "shapetest.cpp"
#include "colortest.cpp"

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

// Phase 5: restyling must move the theme's own ink and nothing else, and must undo in one step.
// Layers (LAYERS_INVESTIGATION.md).  runLayerTests() in layertest.cpp covers the table's own logic;
//  everything here needs a document: the lock actually blocking the editing paths, the undo item, the
//  round trip through the document config, and the wire format.
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

  int undoBefore = scribbleDoc->history->undoSteps();

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

  // COLORS_SPEC.md §10.1: inverting a themed document mirrors its paper rather than XOR-ing to a
  //  negative, so inverting twice must land exactly back where it started - ink, paper and all.
  //  A lossy mirror would be invisible until someone toggled it twice and found their colors drifted.
  {
    // KNOWN DEFECT, and the reason for this line: undoing a restyle restores the stroke colors (they
    //  are StrokeChangedItems) but *not* the recipe, which lives in the document config and is not an
    //  undo item. So right now the document holds theme B's recipe and theme A's ink, and any restyle
    //  from here would find nothing to map. Putting theme A back makes the state consistent again.
    //  The real fix is COLORS_SPEC.md §8's ThemeChangedItem; see COLORS_HANDOFF.md §7.
    scribbleDoc->setTheme(themeA, true, false);

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

void ScribbleTest::runAll(bool runsynctest)
{
  nFailed = 0;
  int nThumbsFailed = 0;
  int nUnitFailed = runScanTests() + runShapeTests() + runColorTests();
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
