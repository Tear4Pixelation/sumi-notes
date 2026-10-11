// benchtest - a broad, repeatable in-app benchmark: `Sumi --benchtest` (run it headless through
//  tools/perf-bench.sh --mode benchtest, never on the desktop).  #included by scribbletest.cpp.
//
// Why it is not perftest: perftest calls ScribbleApp::processEvents() between steps, which is a
//  blocking SDL_WaitEvent - with nothing queued it sleeps until some unrelated event (a timer, a
//  Wayland callback) arrives, so its timings are mostly idle wait - and it never calls
//  Application::layoutAndDraw(), so nothing it does is ever painted.  Here every step drains the
//  queue without blocking and then paints a real frame, and the swap interval is set to 0 for the run
//  (SUMI_BENCH_VSYNC=1 keeps the driver default) so a frame is CPU/GPU work, not waiting for vblank.
//
// Every result is one line "BENCH <name>: <value> <unit>" (ms, kB, count), BENCH_DONE comes last.
//  Times are wall clock (CLOCK_MONOTONIC).  Per-event latencies are an input event plus the redraw it
//  causes, as mean and p95.  The large document is deterministic: one page of generated handwriting
//  (LCG, fixed seed), drawn through the real input path, then replicated to BENCH_PAGES pages.

#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <numeric>

static double benchNowMs()
{
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec*1000.0 + ts.tv_nsec/1.0E6;
}

// CPU time of the calling (main) thread: next to wall time on a loaded machine, it is the steadier number,
//  since waiting for a core or for the GPU does not count.  GPU driver threads are not included.
static double benchCpuMs()
{
  timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return ts.tv_sec*1000.0 + ts.tv_nsec/1.0E6;
}

static void benchOut(const char* name, double value, const char* unit);

// "<name>" in wall ms and "<name>_cpu" in main thread CPU ms since t0/c0
static void benchTotal(const std::string& name, double t0, double c0);

static void benchOut(const char* name, double value, const char* unit)
{
  fprintf(stderr, "BENCH %s: %.3f %s\n", name, value, unit);
  fflush(stderr);
}

static void benchTotal(const std::string& name, double t0, double c0)
{
  double wall = benchNowMs() - t0, cpu = benchCpuMs() - c0;
  benchOut(name.c_str(), wall, "ms");
  benchOut((name + "_cpu").c_str(), cpu, "ms");
}

// VmRSS / VmHWM in kB
static long benchProcStatus(const char* key)
{
  FILE* f = fopen("/proc/self/status", "r");
  if(!f) return -1;
  char line[256];
  long val = -1;
  size_t keylen = strlen(key);
  while(fgets(line, sizeof(line), f)) {
    if(strncmp(line, key, keylen) == 0 && line[keylen] == ':') { val = atol(line + keylen + 1); break; }
  }
  fclose(f);
  return val;
}

static void benchMem(const char* when)
{
  benchOut(fstring("rss_%s", when).c_str(), benchProcStatus("VmRSS"), "kB");
  benchOut(fstring("hwm_%s", when).c_str(), benchProcStatus("VmHWM"), "kB");
}

// mean and p95 of a list of samples, as "<name>_mean", "<name>_p95" (and the count once)
static void benchStats(const char* name, std::vector<double> samples)
{
  if(samples.empty()) return;
  double mean = std::accumulate(samples.begin(), samples.end(), 0.0)/samples.size();
  std::sort(samples.begin(), samples.end());
  size_t idx95 = std::min(samples.size() - 1, size_t(samples.size()*0.95));
  benchOut(fstring("%s_mean", name).c_str(), mean, "ms");
  benchOut(fstring("%s_p95", name).c_str(), samples[idx95], "ms");
  benchOut(fstring("%s_max", name).c_str(), samples.back(), "ms");
}

// deterministic pseudo random numbers, independent of srandpp and of the platform's rand()
struct BenchRng {
  uint32_t state;
  explicit BenchRng(uint32_t seed) : state(seed) {}
  double next() { state = state*1664525u + 1013904223u; return (state >> 8)/double(1 << 24); }  // [0, 1)
};

// drain queued events without blocking, then paint one frame of whatever is dirty
void ScribbleTest::benchFrame()
{
  SDL_Event event;
  while(SDL_PollEvent(&event))
    Application::gui->sdlEvent(&event);
  scribbleArea->doRefresh();
  Application::layoutAndDraw();
}

// one frame that repaints everything (the canvas content image included)
void ScribbleTest::benchFullFrame()
{
  scribbleArea->repaintAll(true);  // doRefresh() -> reqRepaint() then marks the widget dirty
  benchFrame();
}

// screen position of a point in current page coordinates
Point ScribbleTest::benchPageToScreen(Point pagept)
{
  return scribbleArea->dimToScreen(scribbleArea->pageDimToDim(pagept));
}

// one generated handwriting "word": a wavy pressure stroke starting at page point `origin`; with
//  `latency`, every move event and the frame it causes is timed into `latency`
void ScribbleTest::benchWord(BenchRng& rng, Point origin, std::vector<double>* latency)
{
  const int npts = 40;
  Dim width = 60 + 30*rng.next();
  Dim amp = 8 + 8*rng.next();
  Dim phase = 6.283*rng.next();
  Dim loops = 2 + 2*rng.next();
  for(int ii = 0; ii <= npts; ++ii) {
    Dim t = Dim(ii)/npts;
    Point pagept(origin.x + t*width,
        origin.y + amp*std::sin(t*loops*6.283 + phase) + 2*(rng.next() - 0.5));
    Point pt = benchPageToScreen(pagept);
    Dim pressure = 0.3 + 0.6*std::abs(std::sin(t*3.1416 + phase));
    double t0 = latency ? benchNowMs() : 0;
    ie(pt.x, pt.y, pressure, pen, ii == 0 ? press : 0);
    if(latency) {
      benchFrame();
      latency->push_back(benchNowMs() - t0);
    }
  }
  ie(0, 0, 1, pen, release);
  if(latency)
    benchFrame();
}

// a page of words: `lines` lines of `words` each, lines `pitch` apart; each line is brought near the top
//  of the view first, so every word is drawn on screen at the current zoom
void ScribbleTest::benchHandwriting(BenchRng& rng, int lines, int words, Dim pitch)
{
  for(int line = 0; line < lines; ++line) {
    Dim liney = 110 + line*pitch;
    scribbleArea->gotoPos(0, Point(-10, liney - 60));
    for(int word = 0; word < words; ++word)
      benchWord(rng, Point(80 + word*108 + 10*rng.next(), liney), NULL);
  }
}

// a press-move-release drag in screen coordinates, every move timed with its frame
static int benchPressMode = 0;

void ScribbleTest::benchDrag(Point from, Point to, int steps, int src, std::vector<double>* latency, Dim zigzag)
{
  ie(from.x, from.y, 1, src, press);
  benchPressMode = scribbleArea->currMode;  // what the press turned into (stroke, erase, move selection...)
  benchFrame();
  for(int ii = 1; ii <= steps; ++ii) {
    Dim t = Dim(ii)/steps;
    Dim zz = zigzag != 0 ? zigzag*((ii % 2) ? 1 : -1) : 0;
    double t0 = benchNowMs();
    ie(from.x + t*(to.x - from.x), from.y + t*(to.y - from.y) + zz, 1, src);
    benchFrame();
    if(latency)
      latency->push_back(benchNowMs() - t0);
  }
  ie(to.x, to.y, 1, src, release);
  benchFrame();
}

// a simple multi-page vector PDF (paths only, so no fonts are involved), with a valid xref table
static std::string benchMakePdf(int npages)
{
  BenchRng rng(1234);
  std::vector<std::string> objs;  // objs[i] is object number i+1
  objs.push_back("<</Type/Catalog/Pages 2 0 R>>");
  std::string kids;
  for(int ii = 0; ii < npages; ++ii)
    kids += fstring("%d 0 R ", 3 + 2*ii);
  objs.push_back(fstring("<</Type/Pages/Kids[%s]/Count %d>>", kids.c_str(), npages));
  for(int ii = 0; ii < npages; ++ii) {
    objs.push_back(fstring("<</Type/Page/Parent 2 0 R/MediaBox[0 0 595 842]/Contents %d 0 R>>", 4 + 2*ii));
    std::string content = "0.2 w\n";
    for(int jj = 0; jj < 300; ++jj) {
      content += fstring("%.2f %.2f %.2f RG %.1f %.1f m %.1f %.1f %.1f %.1f %.1f %.1f c S\n",
          rng.next(), rng.next(), rng.next(), 40 + 515*rng.next(), 40 + 762*rng.next(),
          40 + 515*rng.next(), 40 + 762*rng.next(), 40 + 515*rng.next(), 40 + 762*rng.next(),
          40 + 515*rng.next(), 40 + 762*rng.next());
    }
    for(int jj = 0; jj < 40; ++jj) {
      content += fstring("%.2f %.2f %.2f rg %.1f %.1f %.1f %.1f re f\n", rng.next(), rng.next(), rng.next(),
          40 + 500*rng.next(), 40 + 740*rng.next(), 5 + 50*rng.next(), 5 + 50*rng.next());
    }
    objs.push_back(fstring("<</Length %d>> stream\n", int(content.size())) + content + "endstream");
  }
  std::string pdf = "%PDF-1.4\n";
  std::vector<size_t> offsets;
  for(size_t ii = 0; ii < objs.size(); ++ii) {
    offsets.push_back(pdf.size());
    pdf += fstring("%d 0 obj ", int(ii + 1)) + objs[ii] + " endobj\n";
  }
  size_t xref = pdf.size();
  pdf += fstring("xref\n0 %d\n0000000000 65535 f \n", int(objs.size() + 1));
  for(size_t off : offsets)
    pdf += fstring("%010d 00000 n \n", int(off));
  pdf += fstring("trailer <</Size %d/Root 1 0 R>>\nstartxref\n%d\n%%%%EOF\n", int(objs.size() + 1), int(xref));
  return pdf;
}

static long long benchStartMs()
{
  // process start time (jiffies since boot, field 22 of /proc/self/stat) against CLOCK_BOOTTIME
  FILE* f = fopen("/proc/self/stat", "r");
  if(!f) return -1;
  char buf[1024];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = '\0';
  char* p = strrchr(buf, ')');  // comm may contain spaces
  if(!p) return -1;
  long long starttime = -1;
  int field = 2;
  for(char* tok = strtok(p + 1, " "); tok; tok = strtok(NULL, " ")) {
    if(++field == 22) { starttime = atoll(tok); break; }
  }
  if(starttime < 0) return -1;
  timespec ts;
  clock_gettime(CLOCK_BOOTTIME, &ts);
  long long nowms = ts.tv_sec*1000LL + ts.tv_nsec/1000000;
  return nowms - starttime*1000LL/sysconf(_SC_CLK_TCK);
}

void ScribbleTest::benchTest()
{
  const char* envpages = getenv("SUMI_BENCH_PAGES");
  const int npages = envpages && atoi(envpages) > 0 ? atoi(envpages) : 100;
  const char* tmpdir = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
  std::string dir = fstring("%s/sumi-bench-%d", tmpdir, int(getpid()));
  createPath(dir.c_str());
  std::string pagefile = dir + "/page.html", bightml = dir + "/big.html", bigsvgz = dir + "/big.svgz";
  std::string smallfile = dir + "/perf.html", pdffile = dir + "/bench.pdf", pdfsaved = dir + "/pdf.svgz";

  // startup: from process start to the first frame painted here (the test runs at the end of
  //  ScribbleApp::init, so this is init plus one full frame; library scan and tab restore are skipped)
  long long sinceStart = benchStartMs();
  benchFullFrame();
  if(sinceStart >= 0)
    benchOut("startup_to_first_frame", double(benchStartMs()), "ms");
  benchOut("gl_render", Application::glRender ? 1 : 0, "bool");

  bool keepVsync = getenv("SUMI_BENCH_VSYNC") && atoi(getenv("SUMI_BENCH_VSYNC")) > 0;
  if(Application::glRender && !keepVsync)
    SDL_GL_SetSwapInterval(0);
  benchOut("vsync_kept", keepVsync ? 1 : 0, "bool");
  benchOut("screen_w", scribbleArea->screenRect.width(), "px");
  benchOut("screen_h", scribbleArea->screenRect.height(), "px");
  benchMem("start");

  // a full repaint of the window, 60 times: the per frame cost every other number includes
  {
    std::vector<double> frames;
    double t0 = benchNowMs(), c0 = benchCpuMs();
    for(int ii = 0; ii < 60; ++ii) {
      double f0 = benchNowMs();
      benchFullFrame();
      frames.push_back(benchNowMs() - f0);
    }
    benchTotal("full_frame_60", t0, c0);
    benchStats("full_frame", frames);
  }

  scribbleMode->setMode(MODE_STROKE);
  scribbleDoc->app->setPen(ScribblePen(Color::BLACK, 1, ScribblePen::TIP_FLAT | ScribblePen::WIDTH_PR, 1.0, 2.0));
  ScribbleApp::cfg->set("singleFile", true);  // the app config: every new document falls back to it
  // generated wavy words are sometimes taken for a scratch-out on lift, which erases them: the document
  //  would depend on the recognizer.  It only runs on pen lift, which no latency number here includes.
  //  (--library runs never save the config, so nothing is changed for good)
  //  Set in the app config: each new or opened document gets a fresh config that falls back to it.
  bool liftScratchOut = ScribbleApp::cfg->Bool("liftScratchOut");
  ScribbleApp::cfg->set("liftScratchOut", false);

  // --- the perftest scenarios, painted, on a fresh document ---
  {
    scribbleDoc->newDocument();
    scribbleArea->gotoPos(0, Point(0,0));
    benchFrame();
    double t0 = benchNowMs(), c0 = benchCpuMs();
    for(Dim y = 20; y < 800; y += 20) {
      for(Dim x = 20; x < 800; x += 20) {
        s3(x, y);
        benchFrame();
      }
    }
    benchTotal("perf_draw_1521_strokes", t0, c0);
    t0 = benchNowMs(); c0 = benchCpuMs();
    doCommand(ID_SELALL);
    benchFrame();
    for(int ii = 0; ii < 10; ii++) {
      s3(400, 400);
      benchFrame();
    }
    benchTotal("perf_move_selection", t0, c0);
    doCommand(ID_CANCEL);
    // perftest pans 2000 frames; over these 1521 dense strokes a frame costs tens of ms, so 150 here
    scribbleMode->setMode(MODE_PAN);
    std::vector<double> panframes;
    t0 = benchNowMs(); c0 = benchCpuMs();
    ie(400, 800, 0, pen, press);
    for(int ii = 0; ii < 150; ii++) {
      double f0 = benchNowMs();
      ie(400, 16*ABS((ii % 100) - 50), 0, pen);
      benchFrame();
      panframes.push_back(benchNowMs() - f0);
    }
    ie(0, 0, 1, pen, release);
    benchFrame();
    benchTotal("perf_pan_150", t0, c0);
    benchStats("perf_pan_frame", panframes);
    t0 = benchNowMs(); c0 = benchCpuMs();
    scribbleDoc->saveDocument(smallfile.c_str());
    benchTotal("perf_save", t0, c0);
    t0 = benchNowMs(); c0 = benchCpuMs();
    scribbleDoc->openDocument(smallfile.c_str());
    benchFrame();
    benchTotal("perf_load", t0, c0);
    benchOut("perf_file_size", getFileSize(smallfile)/1024.0, "kB");
    scribbleMode->setMode(MODE_STROKE);
  }

  // --- generate one page of handwriting through the real input path ---
  const int lines = 30, words = 10;
  {
    scribbleDoc->newDocument();
    Page* page0 = scribbleDoc->document->pages[0];
    scribbleArea->gotoPos(0, Point(-10, -10));
    benchFrame();
    Dim pitch = (page0->height() - 160)/lines;
    BenchRng rng(42);
    double t0 = benchNowMs(), c0 = benchCpuMs();
    benchHandwriting(rng, lines, words, pitch);
    benchFrame();
    benchTotal("gen_page_300_strokes", t0, c0);
    // top level elements: groupStrokes() puts words written close together into one group
    benchOut("gen_page_elements", scribbleDoc->document->pages[0]->strokeCount(), "count");
    scribbleDoc->saveDocument(pagefile.c_str());
    std::string pagehtml = readFile(pagefile.c_str());
    int npaths = 0;
    for(size_t pos = pagehtml.find("<path"); pos != std::string::npos; pos = pagehtml.find("<path", pos + 5))
      ++npaths;
    benchOut("gen_page_paths", npaths, "count");  // includes the ruling lines
  }

  // replicate that page to the large document
  {
    std::string html = readFile(pagefile.c_str());
    size_t pagestart = html.find("<svg class=\"write-page\"");
    size_t bodyend = html.rfind("</body>");
    size_t pageend = bodyend != std::string::npos ? html.rfind("</svg>", bodyend) : std::string::npos;
    if(pagestart == std::string::npos || pageend == std::string::npos || pageend < pagestart) {
      benchOut("ERROR_page_template", 1, "bool");
      fprintf(stderr, "BENCH_DONE\n");
      return;
    }
    pageend += 6;
    std::string pagesvg = html.substr(pagestart, pageend - pagestart);
    std::string big = html.substr(0, pagestart);
    big.reserve(html.size() + pagesvg.size()*npages);
    for(int ii = 0; ii < npages; ++ii)
      big += pagesvg + "\n";
    big += html.substr(pageend);
    FileStream out(bightml.c_str(), "wb");
    out.write(big.data(), big.size());
    out.close();
    benchOut("big_pages", npages, "count");
    benchOut("big_html_size", big.size()/1024.0, "kB");
  }

  // --- the large document ---
  double t0 = benchNowMs(), c0 = benchCpuMs();
  scribbleDoc->openDocument(bightml.c_str());
  benchTotal("big_load_html", t0, c0);
  t0 = benchNowMs(); c0 = benchCpuMs();
  scribbleArea->gotoPos(0, Point(-10, -10));
  benchFullFrame();
  benchTotal("big_first_frame", t0, c0);
  benchMem("after_big_load");

  // scroll the whole document top to bottom, one screen-third per frame
  {
    scribbleMode->setMode(MODE_PAN);
    std::vector<double> frames;
    int step = std::max(50, int(scribbleArea->screenRect.height()/3));
    int last = scribbleDoc->document->numPages() - 1;
    t0 = benchNowMs(); c0 = benchCpuMs();
    for(int ii = 0; ii < 100000 && scribbleArea->getCurrPageNum() < last; ++ii) {
      double f0 = benchNowMs();
      scribbleArea->scrollBy(0, -step);
      benchFrame();
      frames.push_back(benchNowMs() - f0);
    }
    // to the bottom of the last page
    for(int ii = 0; ii < 20; ++ii) {
      double f0 = benchNowMs();
      scribbleArea->scrollBy(0, -step);
      benchFrame();
      frames.push_back(benchNowMs() - f0);
    }
    benchTotal("big_scroll_total", t0, c0);
    benchOut("big_scroll_frames", frames.size(), "count");
    benchStats("big_scroll_frame", frames);
    benchMem("after_scroll_all");
    scribbleMode->setMode(MODE_STROKE);
  }

  // zoom in and out around the middle of a page
  {
    scribbleArea->gotoPage(npages/2);
    benchFrame();
    std::vector<double> frames;
    Dim cx = scribbleArea->screenRect.center().x, cy = scribbleArea->screenRect.center().y;
    t0 = benchNowMs(); c0 = benchCpuMs();
    for(int pass = 0; pass < 2; ++pass) {
      for(int ii = 0; ii < 10; ++ii) { double f0 = benchNowMs(); scribbleArea->zoomBy(1.15, cx, cy); benchFrame(); frames.push_back(benchNowMs() - f0); }
      for(int ii = 0; ii < 20; ++ii) { double f0 = benchNowMs(); scribbleArea->zoomBy(1/1.15, cx, cy); benchFrame(); frames.push_back(benchNowMs() - f0); }
      for(int ii = 0; ii < 10; ++ii) { double f0 = benchNowMs(); scribbleArea->zoomBy(1.15, cx, cy); benchFrame(); frames.push_back(benchNowMs() - f0); }
    }
    benchTotal("big_zoom_total", t0, c0);
    benchStats("big_zoom_frame", frames);
    doCommand(ID_RESETZOOM);
    benchFrame();
  }

  // go to pages all over the document
  {
    std::vector<double> frames;
    t0 = benchNowMs(); c0 = benchCpuMs();
    for(int ii = 0; ii < 40; ++ii) {
      int pagenum = (ii*37) % npages;
      double f0 = benchNowMs();
      scribbleArea->gotoPage(pagenum);
      benchFrame();
      frames.push_back(benchNowMs() - f0);
    }
    for(int ii = 0; ii < 20; ++ii) {
      double f0 = benchNowMs();
      doCommand(ii < 10 ? ID_NEXTPAGE : ID_PREVPAGE);
      benchFrame();
      frames.push_back(benchNowMs() - f0);
    }
    benchTotal("big_goto_total", t0, c0);
    benchStats("big_goto_frame", frames);
  }

  // drawing on a page that already holds 300 strokes: latency per pen move event incl. redraw
  {
    scribbleArea->gotoPos(std::min(10, npages - 1), Point(-10, -10));
    benchFrame();
    BenchRng rng(7);
    std::vector<double> latency;
    t0 = benchNowMs(); c0 = benchCpuMs();
    for(int ii = 0; ii < 20; ++ii)
      benchWord(rng, Point(100 + (ii % 5)*110, 130 + (ii/5)*50 + 20), &latency);
    benchTotal("big_draw_20_strokes", t0, c0);
    benchStats("big_pen_event", latency);
  }

  // select every stroke on a full page, move the selection, undo/redo the move, delete, undo/redo
  {
    scribbleArea->gotoPos(std::min(20, npages - 1), Point(-10, -10));
    benchFrame();
    Point topleft = benchPageToScreen(Point(20, 20));
    Point botright(scribbleArea->screenRect.right - 20, scribbleArea->screenRect.bottom - 20);
    scribbleMode->setMode(MODE_SELECTRECT);
    t0 = benchNowMs(); c0 = benchCpuMs();
    benchDrag(topleft, botright, 20, pen, NULL);
    benchOut("big_select_rect_press_mode", benchPressMode, "mode");
    benchTotal("big_select_rect", t0, c0);
    t0 = benchNowMs(); c0 = benchCpuMs();
    doCommand(ID_SELALL);
    benchFrame();
    benchTotal("big_select_all", t0, c0);
    const Selection* sel = scribbleArea->selection();
    benchOut("big_selected_elements", sel ? double(const_cast<Selection*>(sel)->count()) : 0, "count");
    // drag starting on a stroke of the first line (a press on the selection moves it)
    Point grab = benchPageToScreen(Point(80 + 30, 110));
    std::vector<double> latency;
    scribbleMode->setMode(MODE_STROKE);
    t0 = benchNowMs(); c0 = benchCpuMs();
    benchDrag(grab, grab + Point(120, 90), 30, pen, &latency);
    benchOut("big_move_sel_press_mode", benchPressMode, "mode");
    benchTotal("big_move_sel_total", t0, c0);
    benchStats("big_move_sel_event", latency);
    t0 = benchNowMs(); c0 = benchCpuMs(); undo(); benchFrame(); benchTotal("big_undo_move", t0, c0);
    t0 = benchNowMs(); c0 = benchCpuMs(); redo(); benchFrame(); benchTotal("big_redo_move", t0, c0);
    if(!scribbleArea->hasSelection()) { doCommand(ID_SELALL); benchFrame(); }
    t0 = benchNowMs(); c0 = benchCpuMs(); doCommand(ID_DELSEL); benchFrame(); benchTotal("big_delete_sel", t0, c0);
    t0 = benchNowMs(); c0 = benchCpuMs(); undo(); benchFrame(); benchTotal("big_undo_delete", t0, c0);
    t0 = benchNowMs(); c0 = benchCpuMs(); redo(); benchFrame(); benchTotal("big_redo_delete", t0, c0);
    undo();
    doCommand(ID_CANCEL);
    benchFrame();
  }

  // erasers swept across full pages: whole-stroke, free (stroke splitting) and ruled (line by line) erase
  {
    const int modes[] = {MODE_ERASESTROKE, MODE_ERASEFREE, MODE_ERASERULED};
    const char* names[] = {"big_erase_stroke", "big_erase_free", "big_erase_ruled"};
    for(int mm = 0; mm < 3; ++mm) {
      scribbleArea->gotoPos(std::min(30 + mm, npages - 1), Point(-10, -10));
      benchFrame();
      scribbleMode->setMode(modes[mm]);
      int before = scribbleDoc->document->pages[scribbleArea->getCurrPageNum()]->strokeCount();
      std::vector<double> latency;
      t0 = benchNowMs(); c0 = benchCpuMs();
      for(int row = 0; row < 4; ++row) {
        Point from = benchPageToScreen(Point(60, 110 + row*100));
        Point to = benchPageToScreen(Point(1150, 110 + row*100));
        benchDrag(from, to, 60, pen, &latency, 12);
        if(row == 0)
          benchOut(fstring("%s_press_mode", names[mm]).c_str(), benchPressMode, "mode");
      }
      benchTotal(fstring("%s_total", names[mm]).c_str(), t0, c0);
      benchStats(fstring("%s_event", names[mm]).c_str(), latency);
      benchOut(fstring("%s_elements_delta", names[mm]).c_str(),
          scribbleDoc->document->pages[scribbleArea->getCurrPageNum()]->strokeCount() - before, "count");
      t0 = benchNowMs(); c0 = benchCpuMs();
      for(int row = 0; row < 4; ++row) undo();
      benchFrame();
      benchTotal(fstring("%s_undo4", names[mm]).c_str(), t0, c0);
    }
    scribbleMode->setMode(MODE_STROKE);
  }

  // page thumbnails (the sidebar's Pages view renders these)
  {
    std::vector<double> times;
    t0 = benchNowMs(); c0 = benchCpuMs();
    for(int ii = 0; ii < 20 && ii < npages; ++ii) {
      double f0 = benchNowMs();
      Image thumb = ScribbleDoc::renderPageThumbnail(scribbleDoc->document->pages[ii*5 % npages], 240);
      times.push_back(benchNowMs() - f0);
    }
    benchTotal("thumb_240_x20", t0, c0);
    benchStats("thumb_240", times);
  }

  // save and reload the large document, as .html and as .svgz
  {
    t0 = benchNowMs(); c0 = benchCpuMs();
    scribbleDoc->saveDocument(bightml.c_str());
    benchTotal("big_save_html", t0, c0);
    benchOut("big_save_html_size", getFileSize(bightml)/1024.0, "kB");
    t0 = benchNowMs(); c0 = benchCpuMs();
    scribbleDoc->saveDocument(bigsvgz.c_str());
    benchTotal("big_save_svgz", t0, c0);
    benchOut("big_save_svgz_size", getFileSize(bigsvgz)/1024.0, "kB");
    t0 = benchNowMs(); c0 = benchCpuMs();
    scribbleDoc->openDocument(bigsvgz.c_str());
    benchTotal("big_load_svgz", t0, c0);
    t0 = benchNowMs(); c0 = benchCpuMs();
    for(Page* page : scribbleDoc->document->pages)
      page->ensureLoaded();
    benchTotal("big_svgz_load_all_pages", t0, c0);
    t0 = benchNowMs(); c0 = benchCpuMs();
    scribbleDoc->openDocument(bightml.c_str());
    benchTotal("big_reload_html", t0, c0);
    t0 = benchNowMs(); c0 = benchCpuMs();
    for(Page* page : scribbleDoc->document->pages)
      page->ensureLoaded();
    benchTotal("big_html_load_all_pages", t0, c0);
    benchMem("after_big_reload");
  }

  // PDF import: a generated vector PDF, rasterized at the default resolution
  if(PdfImport::isAvailable()) {
    // SUMI_BENCH_PDF_PAGES for a longer import; the metric keeps its name
    const char* envpdf = getenv("SUMI_BENCH_PDF_PAGES");
    const int pdfpages = envpdf ? std::max(1, atoi(envpdf)) : 2;
    std::string pdf = benchMakePdf(pdfpages);
    {
      FileStream out(pdffile.c_str(), "wb");
      out.write(pdf.data(), pdf.size());
    }
    scribbleDoc->newDocument();
    benchFrame();
    t0 = benchNowMs(); c0 = benchCpuMs();
    int imported = 0;
    {
      Document scratch;
      PdfImport::Options opts;
      imported = PdfImport::importPdf(&scratch, pdffile.c_str(), opts);
      if(imported > 0)
        scribbleDoc->insertPagesFrom(&scratch);
    }
    benchFrame();
    benchTotal("pdf_import_2_pages", t0, c0);
    benchOut("pdf_imported_pages", imported, "count");
    benchMem("after_pdf_import");
    // and scroll through it, which decodes the page images
    std::vector<double> frames;
    scribbleMode->setMode(MODE_PAN);
    scribbleArea->gotoPos(0, Point(-10, -10));
    int step = std::max(50, int(scribbleArea->screenRect.height()/3));
    for(int ii = 0; ii < 400 && scribbleArea->getCurrPageNum() < scribbleDoc->document->numPages() - 1; ++ii) {
      double f0 = benchNowMs();
      scribbleArea->scrollBy(0, -step);
      benchFrame();
      frames.push_back(benchNowMs() - f0);
    }
    benchStats("pdf_scroll_frame", frames);
    // SUMI_BENCH_PDF_PACED=1: the same at reading pace - 150 ms idle before each step, time the frames only -
    //  on a fresh import, as the pass above left every page's texture cached.  A page scrolled onto then
    //  has had its image decoded ahead (ScribbleArea::prefetchImages); above, the next page is visible
    //  a frame or two after it is first asked for.
    if(getenv("SUMI_BENCH_PDF_PACED")) {
      scribbleDoc->newDocument();
      benchFrame();
      {
        Document scratch;
        PdfImport::Options opts;
        if(PdfImport::importPdf(&scratch, pdffile.c_str(), opts) > 0)
          scribbleDoc->insertPagesFrom(&scratch);
      }
      benchFrame();
      std::vector<double> paced;
      scribbleArea->gotoPos(0, Point(-10, -10));
      benchFrame();
      for(int ii = 0; ii < 400 && scribbleArea->getCurrPageNum() < scribbleDoc->document->numPages() - 1; ++ii) {
        usleep(150*1000);
        double f0 = benchNowMs();
        scribbleArea->scrollBy(0, -step);
        benchFrame();
        paced.push_back(benchNowMs() - f0);
      }
      benchStats("pdf_paced_scroll_frame", paced);
    }
    scribbleMode->setMode(MODE_STROKE);
  }

  scribbleDoc->newDocument();
  benchFrame();
  ScribbleApp::cfg->set("liftScratchOut", liftScratchOut);
  benchMem("end");
  if(!getenv("SUMI_BENCH_KEEP"))  // SUMI_BENCH_KEEP=1 leaves the generated files for a look
    removeDir(dir.c_str(), true);
  fprintf(stderr, "BENCH_DONE\n");
  fflush(stderr);
  if(exitAfterTest)
    exit(0);
}

// pdfsizetest - file size, time and memory of Import PDF at several resolutions, and of a save that rewrites
//  every page (what an edit on each page does; the first such save of an old build resampled the pages).
//  Not part of benchtest: it reads real files.  Run headless like benchtest; prints "PDFSIZE ..." lines.
//  SUMI_PDFSIZE_FILES  ':'-separated PDFs, each imported at every resolution of
//  SUMI_PDFSIZE_DPIS   ':'-separated DPIs (default 150:300)
//  SUMI_PDFSIZE_DOCS   ':'-separated .svgz documents, each loaded and saved whole to a new file
//  SUMI_PDFSIZE_REPS   saves per document (default 3); SUMI_PDFSIZE_OUT output directory (required)
static double pdfSizeProcCpuMs()
{
  timespec ts;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return ts.tv_sec*1000.0 + ts.tv_nsec/1.0E6;
}

static long long pdfSizeFileBytes(const std::string& path)
{
  FILE* file = fopen(path.c_str(), "rb");
  if(!file) return -1;
  fseek(file, 0, SEEK_END);
  long long size = ftell(file);
  fclose(file);
  return size;
}

// pixel size of the first page's background image (no decode needed) and how many pages have one
static void pdfSizeImageStats(Document& doc, int* imgw, int* imgh, int* nimages)
{
  *imgw = *imgh = *nimages = 0;
  for(Page* page : doc.pages) {
    if(!page->ensureLoaded(false) || !page->ruleNode) continue;
    SvgNode* node = page->ruleNode->selectFirst("image");
    if(!node) continue;
    SvgImage* img = static_cast<SvgImage*>(node);
    if(*nimages == 0) { *imgw = img->m_image.width; *imgh = img->m_image.height; }
    ++*nimages;
  }
}

// mark every page dirty, then save the whole document to outPath; wall ms returned, CPU ms of all threads
static double pdfSizeResave(Document& doc, const std::string& outPath, double* cpuMs)
{
  for(Page* page : doc.pages) {
    page->ensureLoaded(false);
    ++page->dirtyCount;
  }
  ++doc.dirtyCount;
  removeFile(outPath.c_str());
  double t0 = benchNowMs(), c0 = pdfSizeProcCpuMs();
  // compressLevel 2, as the app's saves (documentlist.cpp)
  bool ok = doc.save(new FileStream(outPath.c_str(), "wb+"), NULL,
      Document::SAVE_FORCE | (Document::saveflags_t(2) << 24));
  *cpuMs = pdfSizeProcCpuMs() - c0;
  double ms = benchNowMs() - t0;
  if(!ok) fprintf(stderr, "PDFSIZE error saving %s\n", outPath.c_str());
  return ms;
}

void ScribbleTest::pdfSizeTest()
{
  const char* outdir = getenv("SUMI_PDFSIZE_OUT");
  auto envList = [](const char* name, const char* dflt) {
    const char* val = getenv(name);
    return splitStr<std::vector>(val ? val : dflt, ':', true);
  };
  const int reps = getenv("SUMI_PDFSIZE_REPS") ? std::max(1, atoi(getenv("SUMI_PDFSIZE_REPS"))) : 3;
  fprintf(stderr, "PDFSIZE save_image_scale %.3f\n", SvgWriter::DEFAULT_SAVE_IMAGE_SCALED);
  for(const std::string& pdf : envList("SUMI_PDFSIZE_FILES", "")) {
    if(!outdir) break;
    std::string base = FSPath(pdf).baseName();
    for(const std::string& dpistr : envList("SUMI_PDFSIZE_DPIS", "150:300")) {
      int dpi = atoi(dpistr.c_str());
      std::string imported = fstring("%s/%s-%d.svgz", outdir, base.c_str(), dpi);
      removeFile(imported.c_str());
      double t0 = benchNowMs(), c0 = pdfSizeProcCpuMs();
      int npages = -1;
      {
        Document importDoc;
        PdfImport::ImportSaver saver(&importDoc, imported, NULL, 2);
        PdfImport::Options opts;
        opts.dpi = dpi;
        opts.saver = &saver;
        npages = PdfImport::importPdf(&importDoc, pdf.c_str(), opts);
        if(npages > 0 && !saver.finish())
          npages = -1;
      }
      double importMs = benchNowMs() - t0, importCpu = pdfSizeProcCpuMs() - c0;
      long long importBytes = pdfSizeFileBytes(imported);
      int imgw = 0, imgh = 0, nimg = 0;
      Document doc;
      doc.load(new FileStream(imported.c_str(), "rb"));
      pdfSizeImageStats(doc, &imgw, &imgh, &nimg);
      // decode one page image: what a page on screen holds (RGBA), and how long that takes
      double decodeMs = 0;
      SvgNode* first = !doc.pages.empty() && doc.pages[0]->ruleNode ? doc.pages[0]->ruleNode->selectFirst("image") : NULL;
      if(first) {
        const Image& img = static_cast<SvgImage*>(first)->m_image;
        double d0 = benchNowMs();
        Image decoded = Image::decodeBuffer(img.encData.data(), img.encData.size());
        decoded.bytes();  // decodeBuffer only reads the header
        decodeMs = benchNowMs() - d0;
      }
      fprintf(stderr, "PDFSIZE import %s dpi %d pages %d bytes %lld per_page %lld ms %.0f cpu_ms %.0f"
          " img %dx%d decoded_bytes %lld decode_ms %.1f\n", base.c_str(), dpi, npages, importBytes,
          npages > 0 ? importBytes/npages : 0, importMs, importCpu, imgw, imgh, (long long)imgw*imgh*4, decodeMs);
      // two rewrites of every page, as two editing sessions would do (a save moves doc to its new file)
      for(int pass = 1; pass <= 2; ++pass) {
        std::string resaved = fstring("%s/%s-%d-resave%d.svgz", outdir, base.c_str(), dpi, pass);
        double cpu = 0, ms = pdfSizeResave(doc, resaved, &cpu);
        Document again;
        again.load(new FileStream(resaved.c_str(), "rb"));
        pdfSizeImageStats(again, &imgw, &imgh, &nimg);
        fprintf(stderr, "PDFSIZE resave%d %s dpi %d bytes %lld ms %.0f cpu_ms %.0f img %dx%d\n", pass,
            base.c_str(), dpi, pdfSizeFileBytes(resaved), ms, cpu, imgw, imgh);
      }
    }
  }
  for(const std::string& path : envList("SUMI_PDFSIZE_DOCS", "")) {
    if(!outdir) break;
    std::string base = FSPath(path).baseName();
    Document doc;
    double t0 = benchNowMs();
    doc.load(new FileStream(path.c_str(), "rb"));
    for(Page* page : doc.pages)
      page->ensureLoaded(false);
    fprintf(stderr, "PDFSIZE docload %s pages %d bytes %lld ms %.0f\n", base.c_str(), doc.numPages(),
        pdfSizeFileBytes(path), benchNowMs() - t0);
    for(int rep = 0; rep < reps; ++rep) {
      // alternate between two names: a save moves the document to the file it wrote
      std::string resaved = fstring("%s/%s-resaved%d.svgz", outdir, base.c_str(), rep % 2);
      double cpu = 0, ms = pdfSizeResave(doc, resaved, &cpu);
      fprintf(stderr, "PDFSIZE docsave %s rep %d bytes %lld ms %.0f cpu_ms %.0f\n", base.c_str(), rep,
          pdfSizeFileBytes(resaved), ms, cpu);
    }
  }
  fprintf(stderr, "PDFSIZE_DONE\n");
  fflush(stderr);
  if(exitAfterTest)
    exit(0);
}
