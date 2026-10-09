#include <fstream>
#include <time.h>

#include "scribbleapp.h"
#include "application.h"
#include "basics.h"
#include "mainwindow.h"
#include "scribbledoc.h"
#include "scribblewidget.h"
#include "scribblesync.h"
#include "bookmarkview.h"
#include "scribbleinput.h"
#include "documentlist.h"
#include "tagdoclist.h"
#include "addpagemenu.h"
#include "tagstore.h"
#include "doclibrary.h"
#include "filepicker.h"
#include "pdfimport.h"
#include "notefulimport.h"
#include "rulingdialog.h"
#include "themedialog.h"
#include "scandialog.h"
#include "cameradialog.h"
#include "pentoolbar.h"
#include "linkdialog.h"
#include "configdialog.h"
#include "touchwidgets.h"
#include "usvg/pdfwriter.h"
#include "ulib/unet.h"
#include "usvg/svgparser.h"
#include "undopersist.h"
#if PLATFORM_WIN
#include "windows/winhelper.h"
#include <shellapi.h>  // for ShellExecute for openUrl
#elif PLATFORM_OSX
#include "macos/macoshelper.h"
#elif PLATFORM_LINUX
#include "linux/linuxtablet.h"
#endif

ScribbleApp* ScribbleApp::app = NULL;
MainWindow* ScribbleApp::win = NULL;
ScribbleConfig* ScribbleApp::cfg = NULL;
Dim ScribbleApp::topInset = 0;
Dialog* ScribbleApp::currDialog = NULL;
Uint32 ScribbleApp::scribbleSDLEvent = 0;

ScribbleApp::ScribbleApp(int argc, char* argv[])
{
  // print the current version to aid investigating errors reported by users
  PLATFORM_LOG("Sumi r" PPVALUE_TO_STRING(SCRIBBLE_REV_NUMBER) "\n");
  // seed RNG (only applies to this thread)
  srandpp(mSecSinceEpoch());
  srand(randpp());

  // load config
  const char* basepath = appDir.empty() ? "." : appDir.c_str();
#if PLATFORM_ANDROID
  const char* appstorage = SDL_AndroidGetExternalStoragePath();
  if(!appstorage)
    appstorage = "/sdcard/Android/data/com.styluslabs.writeqt/files";  // prevent crash
  docRoot = "/sdcard/styluslabs/write/";
  // can't seem to move files into app storage on Android 11, so trash folder needs to be outside it
  tempPath = "/sdcard/styluslabs/.temp/";
  // can still access folders w/o permission - alternative would be to check for or create a file (.nomedia?)
  if(!FSPath(docRoot).exists() || !hasAndroidPermission()) {
    docRoot = FSPath(appstorage, "/").c_str();
    tempPath = FSPath(appstorage, ".temp/").c_str();
  }
  // use old location if existing config file, but otherwise we want to use appstorage local even if we
  //  get sdcard permission
  cfgFile = "/sdcard/styluslabs/write.xml";  // also accept write.xml to make it easier to copy from desktop
  if(!FSPath(cfgFile).exists())
    cfgFile = "/sdcard/styluslabs/.write.xml";
  if(!FSPath(cfgFile).exists())
    cfgFile = FSPath(appstorage, ".write.xml").c_str();
  savedPath = FSPath(appstorage, ".saved/").c_str();
#elif PLATFORM_IOS
  const char* ioshome = getenv("HOME");
  docRoot = FSPath(ioshome, "Documents/").c_str();
  FSPath iosLibRoot = FSPath(ioshome, "Library/");
  // allow user to supply custom config via write.xml in Documents/
  FSPath cfgOverride(docRoot, "write.xml");
  cfgFile = cfgOverride.exists() ? cfgOverride.c_str() : iosLibRoot.child("write.xml").c_str();
  tempPath = iosLibRoot.child("Caches/").c_str();  // iOS does not backup Library/Caches
  savedPath = "Library/saved/";  // relative to HOME, since HOME can change!
#elif PLATFORM_WIN
  std::string env_userprofile = wstr_to_utf8(_wgetenv(utf8_to_wstr("userprofile").data()));
  std::string env_temp = wstr_to_utf8(_wgetenv(utf8_to_wstr("TEMP").data()));
  std::string env_appdata = wstr_to_utf8(_wgetenv(utf8_to_wstr("APPDATA").data()));
  FSPath appDataCfg(env_appdata.size() ? env_appdata : "", "Stylus Labs/write.xml");
  FSPath basePathCfg(basepath, "write.xml");
  // use base path config if it is writable, else use appdata config if it is writable, else use base path
  //  config if we can create it, i.e., if we can write to program folder
  // The reason for preferring config in program folder is so user can complete reset by removing folder
  bool useBase = basePathCfg.exists("r+") || !env_appdata.size() || (!appDataCfg.exists("r+") && basePathCfg.exists("w+"));
  cfgFile = useBase ? basePathCfg.c_str() : appDataCfg.c_str();
  tempPath = env_temp.size() ? FSPath(env_temp, ".styluslabs/").c_str() : "C:/Windows/Temp/.styluslabs/";
  // DocumentList is completely broken if docRoot doesn't exist
  FSPath docrootinfo((env_userprofile.size() ? env_userprofile : "C:/Users/Public"), "Documents/");
  if(!docrootinfo.exists()) docrootinfo = docrootinfo.parent();
  docRoot = docrootinfo.exists() ? docrootinfo.c_str() : "C:/";
#else  // Unix and Mac
  // would like to store write.xml in app bundle on Mac, but this seems to be discouraged if even possible
  const char* env_home = getenv("HOME");
  env_home = env_home && env_home[0] ? env_home : basepath;
  FSPath baseCfg(basepath, "write.xml");
  FSPath homeCfg(env_home, ".config/sumi/sumi.xml");
  // Write kept its config in ~/.config/styluslabs/; move ours out once (config, saved/ and library/, which
  //  live beside it) - only those, since a Stylus Labs install of Write may share the folder
  FSPath oldCfgDir(env_home, ".config/styluslabs/");
  if(!baseCfg.exists("r+") && !homeCfg.exists() && oldCfgDir.child("write.xml").exists()
      && createPath(homeCfg.parent())) {
    moveFile(oldCfgDir.child("write.xml"), homeCfg);
    for(const char* subdir : {"saved", "library"})
      if(isDirectory(oldCfgDir.childPath(subdir).c_str()) && !isDirectory(homeCfg.parent().childPath(subdir).c_str()))
        moveFile(oldCfgDir.child(subdir), homeCfg.parent().child(subdir));
  }
  cfgFile = baseCfg.exists("r+") ? baseCfg.c_str() : homeCfg.c_str();
  docRoot = FSPath(env_home, "/").c_str();
  tempPath = "/tmp/.styluslabs/";
#endif

  // on desktop, use config file in application dir for debug builds
#if PLATFORM_DESKTOP
  if(SCRIBBLE_DEBUG)
    cfgFile = FSPath(basepath, "write.xml").c_str();
  savedPath = FSPath(cfgFile).parent().childPath("saved/");
#elif PLATFORM_ANDROID
  // loading /system/fonts/Roboto-Regular.ttf seems to not work on some devices (not seen personally)
  FSPath sansFont(savedPath, "Roboto-Regular.ttf");
  if(!sansFont.exists()) {
    createPath(savedPath.c_str());
    AndroidHelper::rawResourceToFile("Roboto-Regular.ttf", sansFont.c_str());
  }
  FSPath fallbackFont(savedPath, "DroidSansFallback.ttf");
  if(!fallbackFont.exists()) {
    createPath(savedPath.c_str());
    AndroidHelper::rawResourceToFile("DroidSansFallback.ttf", fallbackFont.c_str());
  }
  // the document browser's typefaces (setupResources() in resources.cpp)
  for(const char* designFont : {"Raleway-Bold.ttf", "Satoshi-Medium.otf"}) {
    FSPath fontFile(savedPath, designFont);
    if(!fontFile.exists()) {
      createPath(savedPath.c_str());
      AndroidHelper::rawResourceToFile(designFont, fontFile.c_str());
    }
  }
#endif
  createPath(tempPath.c_str());
  // beside saved/: app-private on every platform, never in the user's library (undopersist.h)
  if(!savedPath.empty())
    UndoPersist::dir = FSPath(savedPath).parent().childPath(PLATFORM_ANDROID ? ".undo-history/" : "undo-history/");

  cfg = new ScribbleConfig;
  if(!cfg->loadConfigFile(cfgFile.c_str())) {
#if PLATFORM_DESKTOP
    if(!createPath(FSPath(cfgFile).parent().c_str()))
      cfgFile = FSPath(basepath, "write.xml").c_str();
#endif
    runType = "first";
  }

  // allow config values to be passed on command line as --name=value, where value may be in quotes
  bool saveconfig = false;
  for(int ii = 1; ii < argc; ++ii) {
    StringRef arg(argv[ii]);
    if(arg.startsWith("--")) {
      arg += 2;
      auto pieces = splitStringRef(arg, '=');
      if(pieces.size() > 1) {
        if(pieces[1][0] == '"' || pieces[1][0] == '\'')
          pieces[1].chop(1) += 1;  // remove delimiting quotes
        if(pieces[0] == "library") {
          libraryOverride = pieces[1].toString();
          continue;
        }
        // we don't want to save values passed on command line to permanent config file; temporary soln is
        //  to just disable config write; command-line config is just for testing, so this is fine
        disableConfigSave = true;
        // Maybe add a passThru flag to ScribbleConfig (to pass set() to parent)?
        cfg->setConfigValue(pieces[0].toString().c_str(), pieces[1].toString().c_str());
      }
      else {
        if(arg == "saveconfig")
          saveconfig = true;  //disableConfigSave = false;
        else if(arg == "reset") {
          delete cfg;
          cfg = new ScribbleConfig;
        }
        else if(arg == "exit")
          finish();  // exit immediately
        else if(arg == "out" && ii+1 < argc)
          outDoc = argv[++ii];
        else if(arg == "library" && ii+1 < argc)
          libraryOverride = argv[++ii];
        else if(arg.endsWith("test"))
          runType = arg.toString();
        else if(ii+1 < argc && argv[ii+1][0] != '-') {  // support space instead of '=' between arg and value
          disableConfigSave = true;
          cfg->setConfigValue(arg.data(), argv[++ii]);
        }
      }
    }
    else if(!arg.startsWith("-"))  // macOS passes some weird -psn_0... arg on first run
      argDoc = argv[ii];
  }
  disableConfigSave = disableConfigSave && !saveconfig;
  // --library DIR: a throwaway library for docs screenshots and demos. Nothing from the session may reach
  //  the saved config (libraryPath, recent documents pointing into DIR), and the real library's recent
  //  documents must not show up in the screenshots, so this overrides --saveconfig.
  if(!libraryOverride.empty()) {
    disableConfigSave = true;
    cfg->set("recentDocs", "");
  }
}

void ScribbleApp::init()
{
  scribbleSDLEvent = SDL_RegisterEvents(1);
  // every file is picked with the system's dialog; the in-app list only where there is none
  FilePicker::setTempDir(tempPath);
  FilePicker::setFallback([this](bool save, const char* exts, const std::string& name){
    std::string extstr = exts ? exts : "";
    int mode = save ? (extstr == "pdf" ? DocumentList::SAVE_PDF : DocumentList::SAVE_DOC)
        : (extstr == "jpg jpeg png" ? DocumentList::CHOOSE_IMAGE : DocumentList::CHOOSE_DOC);
    std::string filename = execDocumentList(mode, exts);
    return documentList && documentList->result > 0 ? filename : std::string();
  });
  // Default pens
  auto dflttip = ScribblePen::TIP_FLAT | ScribblePen::WIDTH_PR;
  switch(std::max(0, 8 - int(cfg->pens.size()))) {
    case 8: cfg->pens.push_back(ScribblePen(Color::BLACK, 1.6, dflttip, 0.9, 2.0));
    case 7: cfg->pens.push_back(ScribblePen(Color::RED, 1.6, dflttip, 0.9, 2.0));
    case 6: cfg->pens.push_back(ScribblePen(Color::DARKGREEN, 1.6, dflttip, 0.9, 2.0));
    case 5: cfg->pens.push_back(ScribblePen(Color::BLUE, 1.6, dflttip, 0.9, 2.0));
    case 4: cfg->pens.push_back(
        ScribblePen(Color::BLACK, 3.0, dflttip | ScribblePen::WIDTH_DIR, 0.8, 2.0, 0, 45));
    case 3: cfg->pens.push_back(
        ScribblePen(Color::BLACK, 1.8, ScribblePen::TIP_FLAT | ScribblePen::WIDTH_SPEED, 0.7, 0, 0.5, 0));
    case 2: cfg->pens.push_back(ScribblePen(Color::BLACK, 4.0, ScribblePen::TIP_ROUND));
    case 1: cfg->pens.push_back(  //Color(255, 255, 0, 127)
        ScribblePen(Color(255, 127, 255, 127), 34, ScribblePen::TIP_CHISEL | ScribblePen::DRAW_UNDER));
  }

  // load mode logic
  scribbleMode = new ScribbleMode(cfg);
  scribbleMode->loadModes(cfg->String("toolModes"));
  scribbleMode->setMode(MODE_STROKE);

  // create android helper needed to emit signals for Android callbacks since those run on different thread
#if PLATFORM_ANDROID
  AndroidHelper::mainWindowInst = this;
  // request interception of volume keys
  //if(cfg->Int("volButtonMode") > 0) qputenv("QT_ANDROID_VOLUME_KEYS", "1");

  // No more device detection - too many devices, and we don't do it for any other platforms
  // detect pen if necessary -  see scribbleinput.h for explanation of REDETECT_PEN, DETECTED_PEN
  //int pentype = cfg->Int("penType");
  //if(pentype < 0 || (pentype >= ScribbleInput::REDETECT_PEN && pentype < ScribbleInput::DETECTED_PEN)) {
  //  pentype = AndroidHelper::detectPenType(pentype < 0 ? 0 : pentype);
  //  cfg->set("penType", pentype);
  //  // enable single touch draw (disabled by default) and pan from edge if no pen
  //  if(pentype <= 0) {
  //    cfg->set("singleTouchMode", 2);
  //  }
  //}
#endif
  // keep screen on
  cfg->Bool("keepScreenOn") ? SDL_DisableScreenSaver() : SDL_EnableScreenSaver();
  //AndroidHelper::doAction(A_KEEP_SCREEN_ON);

  // setup page sizes on first run
  if(cfg->Float("pageWidth") == 0) {
    int w, h;
    getScreenPageDims(&w, &h);
    cfg->set("pageWidth", Dim(w));
    cfg->set("pageHeight", Dim(h));
    cfg->set("marginLeft", Dim(MIN(100, w/7)));
  }

  // memory limit
  Document::memoryLimit = 1024*1024*size_t(cfg->Int("maxMemoryMB"));  // MB to bytes

  // file extension is used several times (no longer with the "." included!)
  fileExt = cfg->String("docFileExt");
  //nameFilter = "Write Document (*" + fileExt + ")";
  // user agent string for http requests
  httpUserAgent = std::string("Mozilla/5.0 (") + PLATFORM_NAME + ") Sumi r" + PPVALUE_TO_STRING(SCRIBBLE_REV_NUMBER);

  /// UI creation

  win = createMainWindow();  // probably should be done in ScribbleApp
  win->sdlWindow = sdlWindow;  // window created by Application in order to create an OpenGL context
  win->addHandler([this](SvgGui*, SDL_Event* event){ return sdlEventHandler(event); });

  ScribbleDoc* doc = new ScribbleDoc(this, cfg, scribbleMode);
  mActiveArea = new ScribbleArea();
  doc->addArea(mActiveArea);
  scribbleAreas.push_back(mActiveArea);
  scribbleDocs.push_back(doc);

  bookmarkArea = new BookmarkView(cfg, doc);  //bookmarkArea->setScribbleDoc(doc);

  // create UI elements
  win->setupUI(this);
  // add window to SvgGui
  gui->showWindow(win, NULL, false);

  penToolbar = static_cast<PenToolbar*>(win->penToolbarAutoAdj->contents);
  penToolbar->onChanged = [this](int c){ penChanged(c); };
  overlayWidget = win->overlayWidget;

  loadConfig();
  // newDocument results in a call to refreshUI(), so all UI components have to be initialized!
  doc->newDocument();

  // restore the pen of the last used draw tool
  currPen = scribbleMode->currDrawPen();
  currPenIndex = 0;
  bookmarkColor = Color::fromArgb(cfg->Int("bookmarkColor"));

  const char* recentDocsStr = cfg->String("recentDocs");
  auto recentDocsSplit = splitStringRef(StringRef(recentDocsStr), ":::", true);
  for(const StringRef& s : recentDocsSplit)
    recentDocs.emplace_back(s.toString());
  onLoadFile("");  // this will call populateRecentFiles();
  //scribbleDoc->activeArea->setFocus();

  // update check
#if ENABLE_UPDATE
  if(cfg->Bool("updateCheck")
      && cfg->Int("lastUpdateCheck") + cfg->Int("updateCheckInterval") < int(mSecSinceEpoch()/1000))
    updateCheck(false);
#endif

  // testing
#ifdef SCRIBBLE_TEST
  if(StringRef(runType).endsWith("test")) {
    SCRIBBLE_LOG(runTest(runType).c_str());
    finish();
    return;
  }
#endif

  // clear temp folder
  removeDir(tempPath.c_str(), false);

  initLibrary();
  // the open tabs come back unloaded - each one is just a path until it is shown (editortabs.cpp)
  if(outDoc.empty())
    restoreTabs();
  // not for a command line conversion (--out), which must not stop to ask anything
  if(outDoc.empty()) {
    offerLibraryMigration();
    askDefaultPageSize();
  }

#if PLATFORM_ANDROID
  // this will cause permission prompt for fresh install
  if(!FSPath(cfgFile).exists())
    cfg->set("currFolder", "/sdcard/styluslabs/write/");
  // Android permissions disaster: upgrading from target API 29 w/ write permission to 30 will set permission
  //  to "media only", which allows some files created by us outside Android/data app folder to be writable,
  //  others read-only, others neither; can't create new files - so we need to prompt user to grant manage
  //  files permission or reset to Android/data app folder
  const char* currfolder = cfg->String("currFolder");
  if(!hasAndroidPermission()) {
    if(!StringRef(currfolder).contains("com.styluslabs.writeqt") && !requestAndroidPermission()) {
      cfg->set("currFolder", docRoot.c_str());
      cfg->set("reopenLastDoc", false);  // prevent opening of document from changing currFolder
      openOrCreateDoc();  // showing dialog prevents delayedShowDocList from working
      return;
    }
  }
  else if(StringRef(currfolder).startsWith("/sdcard/styluslabs/write/")) {
    //PLATFORM_LOG("Creating /sdcard/styluslabs/write/\n");
    createPath("/sdcard/styluslabs/write/");
  }
  // we are now ready to handle initial intent
  //  if we were sent a document to open, openDocument will close doc list that is shown by newDocument()
  AndroidHelper::processInitialIntent();
  if(activeDoc()->fileName()[0])
    return;
#elif PLATFORM_IOS
  if(libraryManaged()) {
    // SDL's view stays the root and Write's own browser is the way in; everything below is then the same
    //  as on the other platforms, except that the browser is shown directly (see delayedShowDocList)
    initLibraryMode();
    if(cfg->Bool("reopenLastDoc") && recentDocs.size() > 0
        && FSPath(recentDocs[0]).exists() && doOpenDocument(recentDocs[0]))
      return;
    if(runType == "first" && openHelp())
      return;
    openOrCreateDoc();
    return;
  }
  const char* bkmk = cfg->Bool("reopenLastDoc") ? cfg->String("iosBookmark0") : NULL;
  if(runType == "first" && openHelp())
    bkmk = "!";  // this will cause main Write view to be shown after initializing doc browser
  initDocumentBrowser(bkmk);
  return;
#endif

  if(!argDoc.empty()) {
    if(StringRef(argDoc).startsWith("swb://")) {
      if(!openSharedDoc(argDoc)) {
        showNotify(fstring(_("\"%s\" could not be opened."), argDoc.c_str()), 2);
        PLATFORM_LOG("Error opening whiteboard link %s\n", argDoc.c_str());
      }
      return;
    }
    FSPath argInfo(canonicalPath(argDoc));
    if(argInfo.isDir()) {
      if(argInfo.exists())
        cfg->set("currFolder", argInfo.c_str());  // open doc list to passed folder
    }
    else {
      if(PdfImport::isPdfFile(argInfo.c_str())) {
        std::string err, imported = importPdfToDocFile(argInfo.c_str(), &err);
        if(imported.empty()) {
          showNotify(fstring(_("\"%s\" could not be imported."), argDoc.c_str()), 2);
          PLATFORM_LOG("Error importing %s: %s\n", argDoc.c_str(), err.c_str());
          return;
        }
        argInfo = FSPath(imported);  // fall through and open the document we just created
      }
      // don't use doOpenDocument() because it will display dialog on error
      // use argInfo instead of argDoc because relative path causes lots of problems
      Document::loadresult_t res = activeDoc()->openDocument(argInfo.c_str());
      if(res != Document::LOAD_FATAL && res != Document::LOAD_EMPTYDOC) {
        onLoadFile(activeDoc()->fileName());
        // a document named on the command line was chosen deliberately from where it is, so copying it into
        //  the library is offered rather than done; declining edits it in place. A --out conversion only reads it.
        if(outDoc.empty() && libraryManaged() && !isInLibrary(argInfo.c_str())) {
          auto choice = messageBox(Question, _("Document library"), fstring(_("%s is not in your document library."
              "\n\nCopy it into the library and edit the copy, or edit the original where it is?"),
              argInfo.fileName().c_str()), {_("Copy to Library"), _("Edit Original")});
          if(choice == _("Copy to Library"))
            importActiveDocToLibrary(argInfo.c_str());
        }
        if(!outDoc.empty()) {
          FSPath outinfo(outDoc);
          if(outinfo.exists())
            PLATFORM_LOG("Error: output file %s already exists\n", outinfo.c_str());
          else if(outinfo.extension() == "pdf") {
            if(!writePDF(outinfo.c_str()))
              PLATFORM_LOG("Error writing %s\n", outinfo.c_str());
          }
          else {
            auto flags = outinfo.extension() == "html" ? Document::SAVE_MULTIFILE : 0;
            if(!activeDoc()->saveDocument(outinfo.c_str(), Document::SAVE_FORCE | flags))
              PLATFORM_LOG("Error saving %s\n", outinfo.c_str());
          }
        }
      }
      else {
        showNotify(fstring(_("\"%s\" could not be opened."), argDoc.c_str()), 2);
        PLATFORM_LOG("Error opening %s\n", argDoc.c_str());
      }
      return;
    }
  }
  // reopenLastDoc decides, as it always has, whether a document is shown at start: the tab that was active
  if(cfg->Bool("reopenLastDoc") && !tabs.empty()) {
    int lastActive = std::min(std::max(0, cfg->Int("activeTab")), tabs.size() - 1);
    if(showTabInArea(lastActive, activeArea())) {
      onLoadFile(activeDoc()->fileName());
      return;
    }
  }
  if(cfg->Bool("reopenLastDoc") && recentDocs.size() > 0
      && FSPath(recentDocs[0]).exists() && doOpenDocument(recentDocs[0]))
    return;
  if(runType == "first" && openHelp())
    return;

  // delay display of doc list of desktop platforms so that it is shown on top of main window
  delayedShowDocList = true;
}

ScribbleApp::~ScribbleApp()
{
  gui->closeWindow(win);
  // TODO: should use unique_ptr for these ... but then we need a bunch of get()s
  delete documentList;
  delete bookmarkArea;
  for(ScribbleDoc* doc : scribbleDocs)
    delete doc;
  // background tabs' documents are owned by their tabs
  for(EditorTab& tab : tabs.tabs) {
    if(tab.doc && std::find(scribbleDocs.begin(), scribbleDocs.end(), tab.doc) == scribbleDocs.end())
      delete tab.doc;
    tab.doc = NULL;
  }
  for(ScribbleArea* area : scribbleAreas)
    delete area;
  delete scribbleMode;
  // destroy other objects before writing out config, in case destructors set config values
  if(!disableConfigSave)
    cfg->saveConfigFile(cfgFile.c_str(), true);
  delete cfg;  cfg = NULL;
  delete win;  win = NULL;
#if PLATFORM_DESKTOP
  removeDir(tempPath.c_str(), true);
#endif
}

// for now, first button will be default (triggered by Enter key) and last button cancel (triggered by Esc)
// - if we need more flexibility, could use prefix chars, e.g. buttons = "Save|*Save All|~Cancel"
std::string ScribbleApp::messageBox(MessageType type,
    std::string title, std::string message, std::vector<std::string> buttons)
{
  // copied from usvg/test/mainwindow.cpp ... can we deduplicate?
  Dialog* dialog = createPopupDialog(title.c_str());
  Widget* dialogBody = dialog->selectFirst(".body-container");

  //auto buttons = splitStr<std::vector>(buttons.c_str(), '|');
  for(size_t ii = 0; ii < buttons.size(); ++ii) {
    Button* btn = dialog->addButton(buttons[ii].c_str(), [dialog, ii](){ dialog->finish(int(ii)); });
    if(ii == 0)
      dialog->acceptBtn = btn;
    if(ii + 1 == buttons.size())
      dialog->cancelBtn = btn;
  }

  SvgText* msgNode = createTextNode(message.c_str());
  dialogBody->addWidget(new Widget(msgNode));
  // wrap message text as needed
  std::string bmsg = SvgPainter::breakText(msgNode, 0.8*win->winBounds().width());
  if(bmsg != message) {
    msgNode->clearText();
    msgNode->addText(bmsg.c_str());
  }

  // currDialog used by ScribbleSync to clear blocking dialog on reconnect
  Dialog* prevDialog = currDialog;
  currDialog = dialog;
  int res = execDialog(dialog);
  currDialog = prevDialog;
  delete dialog;
  return (res >= 0 && res < (int)buttons.size()) ? buttons[res] : "";  //button_text.back();
}

/// config ///

static int tabUnloadCheckMs(int timeoutSecs);

void ScribbleApp::loadConfig()
{
  SvgWriter::DEFAULT_SAVE_IMAGE_SCALED = cfg->Bool("savePicScaled") ? std::max(Dim(1), gui->paintScale) : 0;
  Element::ERASE_IMAGES = cfg->Bool("eraseOnImage");
  // this should really be per-document, but stick here for now while we consider auto-detecting value
  Page::BLANK_Y_RULING = cfg->Float("blankYRuling");
#if PLATFORM_ANDROID
  AndroidHelper::acceptVolKeys = cfg->Int("volButtonMode") != 0;
#endif
  // do this here avoids need for restart to change theme
  bool lightTheme = cfg->Int("uiTheme") == 2;
  if(lightTheme) {
    win->node->addClass("light");
    gui->setWindowXmlClass("light");  // for new windows and dialogs
  }
  else {
    win->node->removeClass("light");
    gui->setWindowXmlClass("");
  }
  // canvas fill is painted directly, so it can't pick up the --canvas CSS var itself
  ScribbleArea::BACKGROUND_COLOR =
      lightTheme ? ScribbleArea::BACKGROUND_COLOR_LIGHT : ScribbleArea::BACKGROUND_COLOR_DARK;

  // editor tabs: unload background tabs idle past tabUnloadSecs (editortabs.cpp)
  int tabUnloadSecs = cfg->Int("tabUnloadSecs");
  if(tabUnloadTimer) {
    gui->removeTimer(tabUnloadTimer);
    tabUnloadTimer = NULL;
  }
  if(tabUnloadSecs > 0) {
    int checkMs = tabUnloadCheckMs(tabUnloadSecs);
    tabUnloadTimer = gui->setTimer(checkMs, win, [this, checkMs](){
      unloadIdleTabs();
      return checkMs;
    });
  }

  // If we want to retain discard changes, we should make copy of doc before autosave
  // reasonable not to disable this when losing focus (on desktop), unless we save immediately
  int autoSaveInterval = cfg->Int("autoSaveInterval");
  if(autoSaveInterval > 0) {
    autoSaveInterval = std::max(autoSaveInterval, 30)*1000;  // sec to ms
    autoSaveTimer = gui->setTimer(autoSaveInterval, win, autoSaveTimer, [this, autoSaveInterval]() {
      for(ScribbleDoc* d : scribbleDocs) {
        if(d->isModified() && d->fileName()[0]) {
          // delay autosave until pen is lifted
          if(activeDoc()->getActiveMode() == MODE_NONE)
            d->saveDocument();
          else
            d->autoSaveReq = true;
        }
      }
      return autoSaveInterval;
    });
  }
}

// started from loadConfig(), so a changed tabUnloadSecs takes effect without a restart
static int tabUnloadCheckMs(int timeoutSecs)
{
  // a quarter of the timeout, so a tab goes at most 25% late, but not more often than once a second
  return std::min(std::max(timeoutSecs*1000/4, 1000), 60000);
}

void ScribbleApp::saveConfig()
{
  // save window layout
  //cfg->set("mainWindowGeometry", saveGeometry().toBase64().constData());
  //cfg->set("mainWindowState", saveState().toBase64().constData());
  cfg->set("bookmarkColor", bookmarkColor.argb());
  cfg->set("recentDocs", joinStr(recentDocs, ":::").c_str());
  // the tab list, so a restart brings it back (restoreTabs)
  if(tabsEnabled()) {
    cfg->set("openTabs", tabs.serialize().c_str());
    cfg->set("activeTab", activeArea() ? std::max(0, activeTabIndex()) : 0);
  }
  // save mode of tools
  cfg->set("toolModes", scribbleMode->saveModes().c_str());
  if(penToolbar)
    penToolbar->saveConfig(cfg);
  // moved here from ~ScribbleApp(), which usually is never called on mobile
  int nstrokes = 0;
  for(ScribbleArea* area : scribbleAreas) {
    nstrokes += area->strokeCounter;
    area->strokeCounter = 0;
  }
  cfg->set("strokeCounter", cfg->Int("strokeCounter") + nstrokes);
}

void ScribbleApp::storagePermission(bool granted)
{
  SvgGui::pushUserEvent(scribbleSDLEvent, STORAGE_PERMISSION, (void*)granted);
}

/// Split screen / multi-doc handling ///

ScribbleDoc* ScribbleApp::activeDoc() const { return activeArea()->scribbleDoc; }

void ScribbleApp::setActiveArea(ScribbleArea* area)
{
  if(mActiveArea == area)
    return;

  ScribbleDoc* olddoc = activeDoc();
  olddoc->doCancelAction();
  mActiveArea->widget->focusIndicator->node->addClass("inactive");
  area->widget->focusIndicator->node->removeClass("inactive");
  if(area->scribbleDoc != mActiveArea->scribbleDoc)
    setWinTitle(area->scribbleDoc->fileName());  // refreshUI will take care of adding '*' if modified
  area->scribbleDoc->activeArea = area;
  mActiveArea = area;
  if(activeDoc() != olddoc)
    repaintBookmarks(true);
  gui->setFocused(area->widget);  // this will find the focusable ancestor
  refreshUI(activeDoc(), UIState::SetDoc);
}

void ScribbleApp::openSplit()
{
  activeDoc()->addArea(scribbleAreas[1]);
  setActiveArea(scribbleAreas[1]);
  // the new area shows the current doc, but MainWindow covers it with the split placeholder until the
  //  user chooses between this doc and another one (via openSplitDoc())
}

// choose the document for the second area of a split; called from the split placeholder
bool ScribbleApp::openSplitDoc()
{
  // openDocument() calls maybeSave(), which is not necessary when opening split
  return openOrCreateDoc(true);
}

// hide scribbleAreas[1]
bool ScribbleApp::closeSplit()
{
  if(scribbleAreas[0]->scribbleDoc != scribbleAreas[1]->scribbleDoc) {
    setActiveArea(scribbleAreas[1]);
    if(!maybeSave())  // should we always prompt ... maybe better to stick w/ usual behavior
      return false;
    // with tabs the second pane's document stays open as a tab (or, untitled, goes); either way the
    //  pane is emptied below and rebuildShownDocs() sorts out which
    if(!tabsEnabled())
      activeDoc()->newDocument();
  }
  setActiveArea(scribbleAreas[0]);
  if(tabsEnabled()) {
    detachDoc(scribbleAreas[1]);
    rebuildShownDocs();
    updateSplitLabels();
    onLoadFile(activeDoc()->fileName());
    return true;
  }
  scribbleAreas[1]->scribbleDoc->removeArea(scribbleAreas[1]);
  for(ScribbleArea* area : scribbleAreas)
    area->widget->fileNameLabel->setVisible(false);
  // make sure the current doc is first in recent files list (so it is reopened on restart)
  onLoadFile(activeDoc()->fileName());
  return true;
}

/// Event handling ///

// NOTE: this is called from sdlEventHandler on Android since it is not safe to save document on UI thread
//  w/o sync w/ SDL_main thread, which could be modifying document
// return 0 to prevent event from being added to queue for normal processing, 1 otherwise
int ScribbleApp::sdlEventFilter(SDL_Event* event)
{
  switch (event->type) {
  case SDL_APP_TERMINATING:
    // note that we expect SDL_APP_WILLENTERBACKGROUND will always be sent before SDL_APP_TERMINATING
    return PLATFORM_ANDROID ? 1 : 0;  // handle on our thread on Android (call finish())
  case SDL_APP_LOWMEMORY:
  case SDL_APP_WILLENTERBACKGROUND:
#if PLATFORM_ANDROID
    SvgGui::pushUserEvent(scribbleSDLEvent, APP_SUSPEND);
    saveSem.wait();
#else
    appSuspending();
#endif
    return 0;
  case SDL_APP_DIDENTERBACKGROUND:
    Application::isSuspended = true;
    return 0;
  case SDL_APP_WILLENTERFOREGROUND:  // restore state
    return 0;
  case SDL_APP_DIDENTERFOREGROUND:  // restart loops
    Application::isSuspended = false;
    return 1;  // make sure event loop wakes up
  default:
    return 1;  // No special processing, add it to the event queue
  }
}

void ScribbleApp::maybeQuit()
{
  if(win->modalOrSelf() == documentList)
    documentList->finish(DocumentList::REJECTED);
  else if(tagDocList && win->modalOrSelf() == tagDocList)
    tagDocList->finish(TagDocList::REJECTED);
  else if(win->modalOrSelf() != win)
    return;  // send ESC key to cancel dialog?  flash dialog for attention?
  if(maybeSave()) {
    // save other doc if two doc split
    if(scribbleDocs.size() > 1) {
      ScribbleArea* otherArea = (activeArea() == scribbleAreas[0]) ? scribbleAreas[1] : scribbleAreas[0];
      if(otherArea->scribbleDoc && otherArea->scribbleDoc != activeDoc()) {
        setActiveArea(otherArea);
        if(!maybeSave())
          return;   // don't quit
      }
    }
    // background tabs were saved when they were left; only a whiteboard peer can have changed one since
    for(EditorTab& tab : tabs.tabs) {
      if(tab.doc && tab.doc->nViews == 0)
        saveTabDoc(tab.doc);
    }
    saveConfig();
    finish();
  }
}

static int systemClipboardSerial()
{
#if PLATFORM_IOS
  return iosClipboardChangeCount();
#elif PLATFORM_WIN
  return GetClipboardSequenceNumber();
#elif PLATFORM_OSX
  return macosClipboardChangeCount();
#elif PLATFORM_ANDROID
  return AndroidHelper::doAction(A_CLIPBOARD_SERIAL);
#else  // Linux
  return -1;
#endif
}

// main event dispatcher
bool ScribbleApp::sdlEventHandler(SDL_Event* event)
{
  if(FilePicker::handleEvent(event))
    return true;
  switch(event->type) {
  case SDL_QUIT:
    maybeQuit();
    return true;
  case SDL_WINDOWEVENT:
    // only window event sent at start on Android is ENTER
    if(event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED
        || (PLATFORM_ANDROID && event->window.event == SDL_WINDOWEVENT_ENTER)) {
      if(delayedShowDocList) {
        delayedShowDocList = false;
        // allow initial doc list to be canceled on desktop
        if(!SCRIBBLE_DEBUG) openOrCreateDoc();
      }
      isWindowActive = true;
      // requesting app store review
#if PLATFORM_IOS
      if(cfg->Int("strokeCounter") > 4000 && cfg->Int("lastReviewPrompt") + 180*24*60*60 < int(mSecSinceEpoch()/1000)) {
        cfg->set("lastReviewPrompt", int(mSecSinceEpoch()/1000));
        cfg->set("strokeCounter", 0);
        iosRequestReview();  // "https://apps.apple.com/app/id1498369428?action=write-review"
      }
#elif PLATFORM_ANDROID
      if(cfg->Int("strokeCounter") > 10000 && cfg->Int("lastReviewPrompt") == 0) {
        cfg->set("lastReviewPrompt", int(mSecSinceEpoch()/1000));
        auto choice = messageBox(Question, _("Leave a review?"),
            _("You can support the development of Sumi by leaving a review."), {_("OK"), _("Cancel")});
        if(choice == _("OK"))
          openURL("http://play.google.com/store/apps/details?id=com.styluslabs.writeqt");
      }
#endif
      if((PLATFORM_LINUX && clipboardExternal) || (cfg->Bool("preloadClipboard") && clipboardSerial != systemClipboardSerial()))
        loadClipboard();
      // external modification check timer
#if !PLATFORM_IOS
      gui->setTimer(5000, win);
      checkExtModified();
#endif
    }
    else if(event->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
      isWindowActive = false;
      storeClipboard();
#if !PLATFORM_IOS
      gui->removeTimer(win);  // external modification check timer
#endif
    }
    else if(event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
#if PLATFORM_OSX
      // we may want this for other desktop platforms too, but need to test first
      Dim dpi = cfg->Int("screenDPI");
      setupUIScale(dpi >= 10 && dpi <= 1200 ? dpi : 0);
#elif PLATFORM_MOBILE
      win->orientationChanged();
#endif
    }
    return false;  // propagate event for GUI
  case SDL_KEYDOWN:
    return keyPressEvent(event);
  case SDL_KEYUP:
    ScribbleInput::pressedKey = 0;
    return false;
  case SDL_DROPFILE:
    dropEvent(event);
    return true;
  case SDL_CLIPBOARDUPDATE:
    // SDL seems to only send this event for external changes
    clipboardExternal = true;
    // because loading clipboard is async on X11, we can't wait until paste command as on other platforms
    // when switching between two instances of Write, we get the WindowActivate event before the clipboard
    //  change event (since the other instance only updates the clipboard when it gets WindowDeactivate)
    if(isWindowActive && (PLATFORM_LINUX || (cfg->Bool("preloadClipboard") && clipboardSerial != systemClipboardSerial())))
      loadClipboard();
    return true;
#if PLATFORM_ANDROID
  case SDL_APP_TERMINATING:
    // SDL expects main() to return and will call it again, so force exit even if dialog open
    finish();
    return true;
#endif
  default:
    if(event->type == ScribbleSync::sdlEventType) {
      ScribbleDoc* ssdoc = static_cast<ScribbleDoc*>(event->user.data1);
      ScribbleSync* ssync = static_cast<ScribbleSync*>(event->user.data2);
      // we do this instead checking our scribbleDocs list to support ScribbleTest sync test
      if(ssdoc->scribbleSync == ssync)  // make sure ScribbleSync* is still valid
        ssync->sdlEvent(event);
      return true;
    }
    else if(event->type == SvgGui::TIMER) {
      checkExtModified();
      return true;
    }
    else if(event->type == SvgGui::FOCUS_GAINED) {
      // isFocusable is just set to steal focus from pen toolbar text boxes - we don't actually want focus
      win->focusedWidget = NULL;
    }
    else if(event->type == scribbleSDLEvent) {
      if(event->user.code == INSERT_IMAGE) {
        std::unique_ptr<Image> image(static_cast<Image*>(event->user.data1));
        bool fromintent = event->user.data2;
        // a scan goes through the same picker as a plain image insert, so intercept it here rather
        //  than duplicating the per-platform picker code
        if(pendingScan && !fromintent)
          finishScan(std::move(*image));
        else
          insertImage(std::move(*image), fromintent);
      }
      else if(event->user.code == DISMISS_DIALOG) {
        if(currDialog)
          currDialog->finish((intptr_t)event->user.data1);
        currDialog = NULL;
      }
      else if(event->user.code == SIMULATE_PEN_BTN) {
        // we want barrel tap to support other alternative behavior besides changing mode (e.g. sel scale internal)
        ScribbleInput::simulatePenBtn = !ScribbleInput::simulatePenBtn;
        // this is done to provide visual feedback; if pen button behavior changes for other modes in the future,
        //  we'll need to revisit this
        if(ScribbleInput::simulatePenBtn)
          scribbleMode->setMode(cfg->Int("penButtonMode"), true);  // once = true
        else
          scribbleMode->scribbleDone();  // revert to previous mode
        win->updateMode();
      }
      else if(event->user.code == IAP_COMPLETE) {
        delete ScribbleArea::watermark;
        ScribbleArea::watermark = NULL;
        if(win->iapButton)
          win->iapButton->setVisible(false);
        win->redraw();
      }
#if ENABLE_UPDATE
      else if(event->user.code == UPDATE_CHECK)
        updateInfoReceived((char*)event->user.data1);
#endif
#if PLATFORM_ANDROID
      else if(event->user.code == APP_SUSPEND) {
        appSuspending();
        saveSem.post();  // signal completion to Android thread
      }
      else if(event->user.code == STORAGE_PERMISSION) {
        if(event->user.data1) {
          const char* p = "/sdcard/styluslabs/write/";
          if(createPath(p)) {
            docRoot = p;
            if(cfg->loadConfigFile("/sdcard/styluslabs/.write.xml"))
              cfgFile = "/sdcard/styluslabs/.write.xml";
            tempPath = "/sdcard/styluslabs/.temp/";
            createPath(tempPath.c_str());
          }
          // documents written to the app storage fallback go to shared storage, where they outlive Write
          adoptPermanentLibrary();
        }
        if(!openHelp())
          openOrCreateDoc();
      }
#endif
      return true;
    }
    return false;
  }
}

const int ScribbleApp::volUpActions[] = {0, ID_REDO, ID_ZOOMIN, ID_PREVPAGE, ID_NEXTPAGE, ID_EMULATEPENBTN};
const int ScribbleApp::volDnActions[] = {0, ID_UNDO, ID_ZOOMOUT, ID_NEXTPAGE, ID_PREVPAGE, ID_EMULATEPENBTN};

bool ScribbleApp::keyPressEvent(SDL_Event* event)
{
  SDL_Keycode key = event->key.keysym.sym;
  ScribbleInput::pressedKey = 0;  // will only be set to new key if not handled
  switch(key) {
  case SDLK_ESCAPE:
    // tags being placed (docs/agent/page-tags.md) ride on a hovering pointer, which never presses the
    //  canvas, so ScribbleWidget does not see this Esc
    if(activeArea() && activeArea()->placingTags()) {
      activeArea()->cancelTagPlacement();
      return true;
    }
    // cancel action now handled in ScribbleWidget
    // quick exit for debugging version
    if(SCRIBBLE_DEBUG)
      finish();
    break;
  case SDLK_SEMICOLON:
    penSelected(currPenIndex > 0 ? currPenIndex-1 : cfg->pens.size() - 1);
    break;
  case SDLK_QUOTE:
    penSelected(currPenIndex+1 < int(cfg->pens.size()) ? currPenIndex+1 : 0);
    break;
#if PLATFORM_ANDROID
  case SDLK_AC_BACK:
    // hide bookmarks if autohide enabled; otherwise, display document list
    // use Android 4.4 immersive mode instead of disabling back button
    if(cfg->Bool("backKeyEnabled")) {
      // back key press on android which is not accepted generates a close event; this seems to
      //  work automatically for dialogs, but wasn't for main window
      if(backToDocList)
        openDocument();
      else {
        AndroidHelper::moveTaskToBack();
        // if backToDocList is false and we do moveTaskToBack(), focus will return to the launching activity
        //  (e.g. file manager app); but Write may be subsequently launched from recent tasks list or
        //  launcher, in which case we want back key to show doc list
        // Note, openDocument(std::string) may be called from doc list dialog's event loop, so we have to avoid
        //  running this line in that case!
        backToDocList = true;
      }
    }
    break;
#endif
  case SDLK_VOLUMEUP:
  case SDLK_VOLUMEDOWN:
  {
    // note that overriding volume button behavior doesn't work (and isn't allowed) on iOS
    int idx = cfg->Int("volButtonMode");
    if(volUpActions[idx] == ID_EMULATEPENBTN)
      ScribbleInput::pressedKey = key;
    else if(idx > 0)
      activeDoc()->doCommand(key == SDLK_VOLUMEUP ? volUpActions[idx] : volDnActions[idx]);
    break;
  }
  default:
    // Action shortcuts
    if(!win->shortcuts.empty()) {
      // we could get a slight optimization by returning if pressed key is a modifier, but not worth the trouble
      std::string keystr;
      Uint16 mods = event->key.keysym.mod;
#if PLATFORM_OSX
      if(mods & KMOD_GUI) { keystr.append("Ctrl+"); }  // Mac OS Cmd key
#else
      if(mods & KMOD_CTRL) { keystr.append("Ctrl+"); }
#endif
      if(mods & KMOD_ALT) { keystr.append("Alt+"); }
      if(mods & KMOD_SHIFT) { keystr.append("Shift+"); }
      keystr.append(SDL_GetKeyName(event->key.keysym.sym));
      auto it = win->shortcuts.find(keystr);
      if(it != win->shortcuts.end()) {
        if(it->second && it->second->enabled())
          it->second->onTriggered(); //gui, event);
        return true;
      }
    }
    // currently, saved pens are custom menu items w/ no action, so shortcuts won't work - fix this!
    if(key >= SDLK_1 && key <= SDLK_8) {
      if(key - SDLK_1 < int(cfg->pens.size()))
        penSelected(key - SDLK_1);
      break;
    }
    ScribbleInput::pressedKey = key;
    return false;  // key not handled
  }
  return true;  // key handled
}

#if PLATFORM_IOS
// these are called from ioshelper.m
void pencilBarrelTap(void)
{
  // I don't think this is called from a separate thread, but we need an event so that processEvents loop runs
  SDL_Event event = {};
  event.type = ScribbleApp::scribbleSDLEvent;
  event.user.code = ScribbleApp::SIMULATE_PEN_BTN;
  event.user.timestamp = SDL_GetTicks();
  SDL_PushEvent(&event);
  PLATFORM_WakeEventLoop();  // might not be needed
}

void* loadDocumentContents(void* data, size_t len, size_t capacity, const char* url, void* uidoc)
{
  return new UIDocStream(data, len, capacity, url, uidoc); //, len + (4 << 20));  // reserve extra 4 MB
}

const char* getCfgString(const char* name, const char* dflt) { return ScribbleApp::cfg->String(name, dflt); }
//const char* getCfgInt(const char* name, int dflt) { return ScribbleApp::cfg->Int(name, dflt); }

void iapCompleted(void)
{
  // I don't think this is called from a separate thread, but we need an event so that processEvents loop runs
  SDL_Event event = {};
  event.type = ScribbleApp::scribbleSDLEvent;
  event.user.code = ScribbleApp::IAP_COMPLETE;
  event.user.timestamp = SDL_GetTicks();
  SDL_PushEvent(&event);
  PLATFORM_WakeEventLoop();  // might not be needed
}
#elif PLATFORM_ANDROID
bool ScribbleApp::hasAndroidPermission()
{
  // 0x1 - WRITE_EXTERNAL_STORAGE, 0x2 - API level >= 30, 0x4 - MANAGE_EXTERNAL_STORAGE
  int permis = AndroidHelper::doAction(A_CHECK_PERM);
  return permis == 0x1 || (permis & 0x4);
  //return FSPath("/sdcard/styluslabs/write/.nomedia").exists();
}

bool ScribbleApp::requestAndroidPermission()
{
  if(hasAndroidPermission())
    return false;  // already has permission
  auto choice = messageBox(Question, _("Storage Access"),
      _("To open documents in shared folders, enable storage access for Sumi."), {_("OK"), _("Cancel")});
  if(choice != _("OK"))
    return false;
  AndroidHelper::doAction(A_REQ_PERM);
  ScribbleApp::app->appSuspending();
  finish();
  return true;
}
#endif

Clipboard* ScribbleApp::importExternalDoc(SvgDocument* doc)
{
  // discard if no paintable content; this won't catch empty doc if viewbox is set!
  if(!doc->bounds().isValid()) {
    delete doc;
    return NULL;
  }
  // clipboard effectively unwraps top level document - avoid this if doing so will alter doc
  if(doc->stylesheet()) {
    // remove CSS selector and convert CSS style to inline style
    doc->setStylesheet(NULL);
    doc->cssToInlineStyle();
    // remove CSS fragment nodes ... we should support select("style") by checking SvgXmlFragment name
    for(SvgNode* node : doc->select("unknown")) {
      if(strcmp(static_cast<SvgXmlFragment*>(node)->fragment->name(), "style") == 0) {
        node->parent()->asContainerNode()->removeChild(node);
        delete node;
      }
    }
  }
  Clipboard* clip = NULL;
  if(!doc->viewBox().isValid() && doc->getTransform().isTranslate())
    clip = new Clipboard(doc);  // this effectively removes the top level <svg>
  else {
    clip = new Clipboard;
    clip->content->addChild(doc);
  }
  // make sure width and height are set for <svg>s with viewBox so they doesn't use page width and height,
  //  which can change
  // perhaps we should do this in ScribbleArea::doPasteAt instead, but I'd rather not have a bunch of code
  //  handling edge cases for external SVG spread about
  for(SvgNode* node : clip->content->children()) {
    if(node->type() == SvgNode::DOC) {
      SvgDocument* subdoc = static_cast<SvgDocument*>(node);
      if(subdoc->viewBox().isValid()) {
        if(subdoc->width().isPercent())
          subdoc->setWidth(std::min(activeArea()->getViewWidth()/3.0, activeArea()->getCurrPage()->width()/3));
        if(subdoc->height().isPercent())
          subdoc->setHeight(std::min(activeArea()->getViewHeight()/3.0, activeArea()->getCurrPage()->height()/3));
      }
    }
  }
  return clip;
}

void ScribbleApp::dropEvent(SDL_Event* event)
{
#if PLATFORM_IOS
  UIDocStream* strm = (UIDocStream*)event->user.data1;
  long mode = (long)event->user.data2;
  if((mode == iosOpenDocMode || mode == iosChooseDocMode) && libraryManaged()) {
    // From Files, another app, or Import Document... The browser may be up (e.g. a cold start from
    //  "Open in Write"), and the document replaces it.
    if(tagDocList && tagDocList->isVisible())
      tagDocList->finish(TagDocList::REJECTED);
    std::string filename = strm->name();
    if(isInLibrary(filename)) {
      // already ours: open it as the plain file it is, not through UIDocument
      delete strm;
      if(maybeSave())
        doOpenDocument(filename);
      return;
    }
    if(!maybeSave()) {
      delete strm;
      return;
    }
    if(!doOpenDocument(strm))
      openOrCreateDoc(false);
    else if(!importActiveDocToLibrary(filename)) {
      // never keep editing the original through its UIDocument, which would write it back
      activeDoc()->newDocument();
      openOrCreateDoc(false);
    }
  }
  else if(mode == iosOpenDocMode || mode == iosChooseDocMode) {
    if(!doOpenDocument(strm))
      openOrCreateDoc(false);  // reopen doc browser on failure
    else if(activeArea() == scribbleAreas[0]) {  // only save bookmark for primary doc
      char* bkmk = iosGetSecuredBookmark(strm->uiDocument);
      cfg->set("iosBookmark0", bkmk ? bkmk : "");
      free(bkmk);
    }
  }
  else if(mode == iosUpdateDocMode) {
    // figure out which ScribbleDoc owns the doc
    for(ScribbleDoc* doc : scribbleDocs) {
      UIDocStream* docstrm = (UIDocStream*)doc->document->blockStream.get();
      if(docstrm && docstrm->uiDocument == strm->uiDocument) {
        if(doc->isModified()) {
          // restore old buffer on WriteDocument, since we may need it for ensurePagesLoaded for saveAs
          iosSaveDocument(strm->uiDocument, docstrm->data(), 0);  // len = 0 disables saving
          // free new buffer immediately
          strm->uiDocument = NULL;
          delete strm;
          iosSaveAs(iosConflictSaveMode);  //execDocumentList(DocumentList::SAVE_DOC);
        }
        else {
          docstrm->uiDocument = NULL;
          if(doc->openDocument(strm) == Document::LOAD_OK)
            messageBox(Info, _("Document reloaded"),
                fstring(_("%s has been reloaded due to modification outside of Sumi."),
                docShortName(strm->name()).c_str()));
        }
        break;
      }
    }
  }
  else if(mode == iosInsertDocMode)  // insert document
    activeDoc()->insertDocument(strm);
  else if(mode == iosSaveAsMode || mode == iosConflictSaveMode) {
    auto flags = FSPath(strm->name()).extension() == "html" ? Document::SAVE_MULTIFILE : 0;
    if(activeDoc()->saveDocument(strm, Document::SAVE_FORCE | flags))
      onLoadFile(strm->name());
    else
      messageBox(Error, _("Save As"), _("Error saving document. Please try a different folder."));
  }
#else
  FSPath fileinfo(event->drop.file);
  SDL_free(event->drop.file);
  std::string basename = fileinfo.baseName();
  std::string ext = fileinfo.extension();
  if(!fileinfo.exists())
    return;
  if((documentList && documentList->isVisible()) || (tagDocList && tagDocList->isVisible())) {}  // always try to open as doc if doc list visible
  else if(ext == "svgz" || (ext == "svg" && !StringRef(basename).chop(3).endsWith("_page"))) {
    // we load as Document to check if this is a Write document, then reload as Write doc or external SVG
    //  depending on result - not ideal, but this isn't expected to be a common use case
    FileStream* strm = new FileStream(fileinfo.c_str(), "rb");
    Document newdoc;  // newdoc will take ownership of strm
    // ensurePagesLoaded() will return true as long as content has valid bounds
    if(newdoc.load(strm, true) != Document::LOAD_OK && newdoc.numPages() < 2) {
      // Page may modify SVG (e.g. removing viewBox), so just reload
      SvgDocument* svgdoc = SvgParser().parseFile(fileinfo.c_str());
      Clipboard* clip = svgdoc ? importExternalDoc(svgdoc) : NULL;
      if(clip) {
        // can we get drop event on Android?  if so, how to handle this?
        int x, y, x0, y0;
        SDL_GetGlobalMouseState(&x, &y);
        SDL_GetWindowPosition(win->sdlWindow, &x0, &y0);
        // offset.isNan() selects PasteCenter instead of PasteOrigin
        clip->content->addClass("external");
        activeArea()->clipboardDropped(clip, Point(x - x0, y - y0)*gui->inputScale, Point(NaN, NaN));
        delete clip;
        return;
      }
    }
  }
  else if(ext == "bmp" || ext == "jpg" || ext == "jpeg" || ext == "gif" || ext == "png") {
    // TODO: we should figure out what area image was dropped on and insert there!
    insertImage(fileinfo.c_str());
    return;
  }
  // default: try to load as document
  if(maybeSave())
    openDocument(fileinfo.c_str());
#endif
}

void ScribbleApp::appSuspending()
{
  if(!activeDoc())  // app may not be initialized yet
    return;
  // save if modified or multiple file doc (to save view position)
  for(ScribbleDoc* doc : scribbleDocs) {
    if(doc->nViews > 0 && (doc->isModified() || doc->cfg->Bool("saveUnmodified") || doc->document->isEmptyFile())) {
      // no chance to prompt user, so don't use doSave()
      if(!doc->fileName()[0]) {
        if(doc->isModified()) { // don't bother unless doc is actually modified
          doc->saveDocument(createRecoveryName(FSPath(libraryManaged() ? libraryRoot : cfg->String("currFolder"), "Untitled").c_str()).c_str());
          onLoadFile(doc->fileName());  // add to recent doc list before saving config
        }
      }
      else
        doc->saveDocument();  //checkExtModified(doc) || doc->saveDocument() -- can't prompt anyway
    }
  }
  for(EditorTab& tab : tabs.tabs) {
    if(tab.doc && tab.doc->nViews == 0)
      saveTabDoc(tab.doc);
  }
  saveConfig();
  if(!disableConfigSave)
    cfg->saveConfigFile(cfgFile.c_str(), true);
}

/// clipboard ///
// move this stuff (and overlay stuff?) to a ClipboardManager class?

// sync with system clipboard - remember that on Linux, clipboard contents are lost when owning app closes
void ScribbleApp::loadClipboard()
{
#if !PLATFORM_LINUX
  // on X11 (not sure about Wayland), we only get SDL_CLIPBOARDUPDATE when we lose ownership of clipboard,
  //  so we must try to load every time we gain focus unless we own it - so we can't reset clipboardExternal
  clipboardExternal = false;
#endif
  clipboardSerial = systemClipboardSerial();

#if PLATFORM_WIN
  Image clipImg = getClipboardImage();
  if(!clipImg.isNull()) {
    setClipboardToImage(std::move(clipImg));
    return;
  }
#elif PLATFORM_IOS
  // getClipboardImage calls imagePicked() and returns 1 if image available on clipboard
  if(iosGetClipboardImage())
    return;
#endif

#if PLATFORM_LINUX
  // SDL_GetClipboardText on Linux doesn't support INCR, so fails w/ large amounts of text
  requestClipboard(win->sdlWindow);  // async, so will set clipboard when contents are ready
#else
  const char* clipText = SDL_GetClipboardText();
  if(clipText && clipText[0])
    loadClipboardText(clipText);
  SDL_free((void*)clipText);
#endif
}

void ScribbleApp::loadClipboardText(const char* clipText, size_t len)
{
  SvgDocument* svgDoc = NULL;
  StringRef clipRef = StringRef(clipText, len ? len : strlen(clipText)).trimL();
  // don't try to load clipboard as SVG unless first char is "<"
  if(clipRef.startsWith("<")) {
    // don't require ' ' after "<svg" since it could be any whitespace
    svgDoc = clipRef.startsWith("<svg") || clipRef.startsWith("<?xml") || clipRef.startsWith("<!DOCTYPE") ?
        SvgParser().parseString(clipRef.data(), clipRef.size()) :
        SvgParser().parseFragment(clipRef.data(), clipRef.size());
    if(!svgDoc) {
      int offset = clipRef.find("<svg");
      if(offset >= 0)
        svgDoc = SvgParser().parseString(clipRef.data() + offset, clipRef.size() - offset);
    }
  }
  Clipboard* clip = svgDoc ? importExternalDoc(svgDoc) : NULL;
  if(clip) {
    clipboard.reset(clip);
    clipboard->content->addClass("external");
    clipboardPage = NULL;
    clipboardFlags = 0;
    refreshUI(activeDoc(), UIState::ClipboardChange);  // enabled state of paste may have changed
  }
}

// called when main window loses focus
bool ScribbleApp::storeClipboard()
{
  if(!clipboard || !newClipboard)
    return false;
  std::ostringstream ss;
  clipboard->saveSVG(ss);
  SDL_SetClipboardText(ss.str().c_str());
  newClipboard = false;
  clipboardExternal = false;
  clipboardSerial = systemClipboardSerial();
  return true;
}

void ScribbleApp::setClipboardToImage(Image img, bool lossy)
{
  Dim imgw = img.getWidth()*ScribbleView::unitsPerPx;
  Dim imgh = img.getHeight()*ScribbleView::unitsPerPx;
  Dim s = std::min(Dim(1),
      std::min(activeArea()->getCurrPage()->width()/2/imgw, activeArea()->getCurrPage()->height()/2/imgh));
  Rect bbox = Rect::ltwh(100*ScribbleView::unitsPerPx, 100.5*ScribbleView::unitsPerPx, imgw*s, imgh*s);
  if(img.encoding == Image::UNKNOWN)
    img.encoding = lossy ? Image::JPEG : Image::PNG;
  Element* pic = new Element(new SvgImage(std::move(img), bbox));
  clipboard.reset(new Clipboard);
  clipboard->addStroke(pic);
  //clipboard->content->addClass("external");  -- not needed since we already have scaled image
  clipboardPage = NULL;
  clipboardFlags = 0;
  refreshUI(activeDoc(), UIState::ClipboardChange);
}

void ScribbleApp::setClipboard(Clipboard* cb, const Page* srcpage, int flags)
{
  clipboard.reset(cb);
  clipboardPage = srcpage;
  clipboardFlags = flags;
  newClipboard = true;
  clipboardExternal = false;
  clipboardSerial = systemClipboardSerial();
}

// this is needed to get reasonable behavior w/ iOS split-screen since clipboard notifications don't work
void ScribbleApp::pasteClipboard()
{
  if(clipboardSerial != systemClipboardSerial())  // false on Linux since clipboardSerial is always -1
    loadClipboard();
  doCommand(ID_PASTE);
}

/// openURL ///

bool ScribbleApp::openURL(const char* url)
{
#if PLATFORM_WIN
  HINSTANCE result = ShellExecute(0, 0, PLATFORM_STR(url), 0, 0, SW_SHOWNORMAL);
  // ShellExecute returns a value greater than 32 if successful
  return (int)result > 32;
#elif PLATFORM_ANDROID
  AndroidHelper::openUrl(url);
  return true;
#elif PLATFORM_IOS
  if(!strchr(url, ':'))
    iosOpenUrl((std::string("http://") + url).c_str());
  else
    iosOpenUrl(url);
  return true;
#elif PLATFORM_OSX
  return strchr(url, ':') ? macosOpenUrl(url) : macosOpenUrl((std::string("http://") + url).c_str());
#else  // Linux
  system(fstring("xdg-open '%s' || x-www-browser '%s' &", url, url).c_str());
  return true;
#endif
}

/// pen ///

// type of `changed` should be PenToolbar::ChangeFlag but I'm lazy
void ScribbleApp::penChanged(int changed)
{
  const ScribblePen& pen = penToolbar->pen;
  // persist the pen and width presets soon after the last change, not only when the app is backgrounded
  //  (which mobile may never deliver before killing it); a burst of changes (a slider drag) writes once
  if(changed != PenToolbar::YIELD_FOCUS && !disableConfigSave) {
    penSaveTimer = gui->setTimer(1500, win, penSaveTimer, [this]() {
      penSaveTimer = NULL;
      saveConfig();
      writeConfigFile();
      return 0;
    });
  }
  if(changed == PenToolbar::YIELD_FOCUS)
    gui->setFocused(activeArea()->widget);
  else if(penToolbar->mode == PenToolbar::PEN_MODE)
    setPen(pen);
  else if(changed == PenToolbar::PEN_CHANGED && penToolbar->mode == PenToolbar::SELECTION_MODE) {
    // "Use as Pen": the selection's color and width go to the draw pen, and nothing else does.  The
    //  toolbar's pen here is the selection's - built from a color and an absolute width, no flags - and
    //  taking it whole made the pen absolute, dropped the marker's tip and DRAW_UNDER, and converted the
    //  tool's width presets to units on the next setPen() (pen-and-tools.md, "Pen and selection")
    ScribblePen drawPen = currPen;
    if(pen.color.alpha() > 0) {
      int alpha = drawPen.color.alpha();  // keep the marker translucent, as the shape row does
      drawPen.color = pen.color;
      drawPen.color.setAlpha(alpha);
    }
    if(pen.width > 0)
      drawPen.width = drawPen.hasFlag(ScribblePen::WIDTH_RELATIVE) ? pen.width/penToolbar->lineHeight() : pen.width;
    setPen(drawPen);
  }
  else if(penToolbar->mode == PenToolbar::BOOKMARK_MODE)
    bookmarkColor = pen.color;
  else if(penToolbar->mode == PenToolbar::SELECTION_MODE) {
    size_t hpos = activeDoc()->history->histPos();
    // must always create undo item if syncing
    bool undoable = !(changed & PenToolbar::UNDO_PREV) || historyPos == hpos || activeDoc()->scribbleSync;
    if(undoable)
      historyPos = hpos;

    StrokeProperties props(changed & PenToolbar::COLOR_CHANGED ? pen.color : Color::INVALID_COLOR,
        changed & PenToolbar::WIDTH_CHANGED ? pen.width : -1);
    if(changed & PenToolbar::DASH_CHANGED)
      props.dashStyle = penToolbar->dashStyle;
    activeArea()->setStrokeProperties(props, undoable);
  }
}

void ScribbleApp::setPen(const ScribblePen& pen)
{
  currPen = pen;
  // keep the active draw tool's stored pen up to date so it survives a tool switch and app restart
  scribbleMode->currDrawPen() = pen;
}

void ScribbleApp::setDrawTool(int tool)
{
  scribbleMode->drawTool = tool;
  currPen = scribbleMode->currDrawPen();
  setMode(MODE_STROKE);
}

// The pen and a selection share the one toolbar, so this decides whose it is.  It used to be the
//  selection's whenever ink was selected, whatever the tool: select with Switch Back on (the default) and
//  the pen comes back with the selection still up, and the pen row then showed the selection's absolute
//  width with no Relative size toggle, and its color and width edited the selection.  A draw tool's row is
//  the pen's; the selection popup's color and width items are how a selection is edited then, so the
//  toolbar is the selection's only while that popup is open (or about to be).  Other tools (select, move,
//  a shape with its handles) keep it on the selection, as before.
bool ScribbleApp::penToolbarEditsSelection(const ScribbleArea* area, int mode, bool selPopupOpen)
{
  // a selected ruling region is not ink: picking a color then must not recolor the region or the ink it
  //  carries, so the toolbar stays on the pen
  if(!area || !area->hasSelection() || area->selectedRegion())
    return false;
  return selPopupOpen || ScribbleMode::getModeType(mode) != MODE_STROKE;
}

void ScribbleApp::updatePenToolbar(bool forSelPopup)
{
  bool selPopupOpen = forSelPopup || (win && win->selPopup && win->selPopup->isVisible());
  if(penToolbarEditsSelection(activeArea(), scribbleMode->getMode(), selPopupOpen)) {
    int dashStyle;
    ScribblePen pen = activeArea()->getPenForSelection(&dashStyle);
    penToolbar->setPen(pen, PenToolbar::SELECTION_MODE, dashStyle);
  }
  else if(scribbleMode->getMode() == MODE_BOOKMARK)
    penToolbar->setPen(ScribblePen(bookmarkColor, -1), PenToolbar::BOOKMARK_MODE);
  else
    penToolbar->setPen(currPen, PenToolbar::PEN_MODE);
}

// load saved pen
void ScribbleApp::penSelected(int penindex)
{
  ScribblePen* pen = activeDoc()->cfg->getPen(penindex);
  if(pen) {
    currPenIndex = penindex;
    setPen(*pen);
    setMode(MODE_STROKE);
    if(cfg->Bool("applyPenToSel")) {
      // a relative pen's width is in line heights, the selection's in document units
      Page* page = activeArea()->getCurrPage();
      Dim width = pen->hasFlag(ScribblePen::WIDTH_RELATIVE)
          ? pen->width*(page ? page->yruling(true) : Page::BLANK_Y_RULING) : pen->width;
      activeArea()->setStrokeProperties(StrokeProperties(pen->color, width));  // no-op if no sel
    }
  }
}

/// painting and commands ///

Dim ScribbleApp::getPreScale()
{
  // TODO: remove use of this fn
  return Dim(1.0);  //cfg->Float("preScale");
}

void ScribbleApp::getScreenPageDims(int* w, int* h)
{
//#ifdef Q_OS_ANDROID
//  int androiddims = android_detectScreenSize();
//  int screenw = androiddims >> 16;
//  int screenh = androiddims & 0x0000FFFF;
//#else
  SDL_Rect r;
#if PLATFORM_IOS
  SDL_GL_GetDrawableSize(sdlWindow, &r.w, &r.h);
#else
  SDL_GetDisplayBounds(std::max(0, SDL_GetWindowDisplayIndex(sdlWindow)), &r);  // or SDL_GetDisplayUsableBounds
#endif
  // 0,0 page size causes problems (e.g. errors loading doc)
  r.w = std::max(200, r.w);
  r.h = std::max(200, r.h);
  *w = 2*int((std::min(r.w, r.h)*ScribbleView::unitsPerPx - 24)/2);
  *h = 2*int((std::max(r.w, r.h)*ScribbleView::unitsPerPx - 24)/2);
}

// the choices for the default size of new pages; paper sizes are Page Setup's own (RulingDialog::predefSizes)
std::vector<ScribbleApp::PageSizePreset> ScribbleApp::pageSizePresets()
{
  int screenw, screenh;
  getScreenPageDims(&screenw, &screenh);
  auto paper = [](const char* title, int index) {
    return PageSizePreset{title, RulingDialog::predefSizes[index][0], RulingDialog::predefSizes[index][1]};
  };
  return {paper(_("A4"), 5), paper(_("A4 (landscape)"), 6), paper(_("Letter"), 3), paper(_("Letter (landscape)"), 4),
      {_("Screen"), screenw, screenh}, {_("Screen (landscape)"), screenh, screenw}};
}

// Letter is the paper of the Americas (and the Philippines); everywhere else it is A4
static bool regionUsesLetter(const char* region, size_t len)
{
  static const char* letterRegions[] = {"US", "CA", "MX", "PH", "CL", "CO", "VE", "CR", "GT", "SV", "PR", "PA",
      "DO", "NI", "BZ"};
  if(len != 2)
    return false;
  for(const char* letterRegion : letterRegions) {
    if(strncmp(region, letterRegion, 2) == 0)
      return true;
  }
  return false;
}

// Only a hint for which answer to offer first, so an unknown locale simply means A4.  The POSIX variables
//  come first, in their order of precedence - LC_PAPER is the setting for exactly this question.  SDL's
//  locale API covers Windows and Android, but the Windows and macOS SDL branches predate it.
bool ScribbleApp::localeUsesLetter()
{
  for(const char* var : {"LC_ALL", "LC_PAPER", "LANG"}) {
    const char* locale = getenv(var);
    if(!locale || !locale[0])
      continue;
    // ll_CC[.encoding][@modifier]; "C" and "POSIX" name no region
    const char* region = strchr(locale, '_');
    if(!region)
      return false;
    ++region;
    return regionUsesLetter(region, strcspn(region, ".@"));
  }
#if SDL_VERSION_ATLEAST(2, 0, 14)
  SDL_Locale* locales = SDL_GetPreferredLocales();
  bool letter = locales && locales[0].language && locales[0].country
      && regionUsesLetter(locales[0].country, strlen(locales[0].country));
  SDL_free(locales);
  return letter;
#else
  return false;
#endif
}

// Once, on first start (or the first start of a build that has this).  The default used to be the screen
//  turned on its side, which on a 16:9 monitor is a 0.56 strip narrower than any paper - and since pages
//  grow as they are written on, the width, i.e. the shape, is all a default size really decides.
void ScribbleApp::askDefaultPageSize()
{
  if(cfg->Bool("pageSizeAsked"))
    return;
  cfg->set("pageSizeAsked", true);
  bool letterFirst = localeUsesLetter();
  std::string a4 = _("A4 (210 × 297 mm)"), letter = _("Letter (8.5 × 11 in)");
  std::vector<std::string> buttons = letterFirst ? std::vector<std::string>{letter, a4} : std::vector<std::string>{a4, letter};
  std::string choice = messageBox(Question, _("Page size"),
      _("Which paper size should new pages use?\n\nThis can be changed later under Preferences > General."), buttons);
  // dismissed without an answer: the size the locale suggests is still better than the screen's
  bool useLetter = choice == letter || (choice != a4 && letterFirst);
  int preset = useLetter ? 3 : 5;
  cfg->set("pageWidth", Dim(RulingDialog::predefSizes[preset][0]));
  cfg->set("pageHeight", Dim(RulingDialog::predefSizes[preset][1]));
  // the startup document was created before the question, so its first page still has the old size
  if(!activeDoc()->fileName()[0] && !activeDoc()->isModified())
    activeDoc()->newDocument();
  else
    activeDoc()->updateGhostPage();
}

void ScribbleApp::refreshUI(ScribbleDoc* doc, int reason)
{
  win->refreshUI(doc, reason);
}

// newdoc means change of Document or of ScribbleDoc - in either case we want to reset scroll position
void ScribbleApp::repaintBookmarks(bool newdoc)
{
  if(newdoc)
    bookmarkArea->setScribbleDoc(activeDoc());  // use this to reset scroll position
  if(win->actionShow_Bookmarks->checked())
    bookmarkArea->repaintBookmarks();  // force refresh
}

void ScribbleApp::doCommand(int cmd)
{
  activeDoc()->doCommand(cmd);
}

void ScribbleApp::setMode(int mode)
{
  if((scribbleMode->getMode() == MODE_PAGESEL) != (mode == MODE_PAGESEL)) {
    for(ScribbleDoc* doc : scribbleDocs) {
      doc->clearSelection();
      doc->uiChanged(UIState::SelChange);
      doc->repaintAll();
      doc->doRefresh();
    }
  }
  // a multi-point shape has to be committed before the tool that created it goes away (spec 4)
  for(ScribbleDoc* doc : scribbleDocs)
    doc->finishShapes();
  // ruling regions show their "..." button only while the select tool is in hand, so taking it up or
  //  putting it down has to repaint the pages
  bool wasSelect = ScribbleMode::getModeType(scribbleMode->getMode()) == MODE_SELECT;
  scribbleMode->setMode(mode);
  if(wasSelect != (ScribbleMode::getModeType(scribbleMode->getMode()) == MODE_SELECT)) {
    for(ScribbleDoc* doc : scribbleDocs) {
      doc->repaintAll();
      doc->doRefresh();
    }
  }
  win->updateMode();
}

void ScribbleApp::hideBookmarks()
{
  if(win->bookmarkPanel->isVisible())
    win->toggleBookmarks();
}

void ScribbleApp::showSelToolbar(Point pos)
{
  // sel toolbar only opened on pen up, so don't make pressed (because outside_pressed event will invoke 2nd
  //  call to ScribbleArea::doReleaseEvent)
  win->refreshSelPopup();
  gui->showContextMenu(win->selPopup, pos, NULL, false);   //if(!pos.isNaN())
}

bool ScribbleApp::oneTimeTip(const char* id, Point pos, const char* message)
{
  return win->oneTimeTip(id, pos, message);
}

/// file handling ///

// Document handling in UI:
// - save automatically on exit (configurable); prompt if document doesn't have a filename
// - save as to save under a different name
// - "discard changes" to exit without saving

void ScribbleApp::insertDocument()
{
  FilePicker::openFile(_("Insert Document"), "svgz svg html htm", [this](const std::string& filename){
    if(!filename.empty() && activeDoc()->insertDocument(new FileStream(filename.c_str(), "rb")) != Document::LOAD_OK)
      messageBox(Warning, _("Error inserting document"), fstring(_("An error occured opening %s"), docDisplayName(filename).c_str()));
  });
}

std::string ScribbleApp::execDocumentList(int mode, const char* exts, bool cancelable)
{
#if PLATFORM_IOS
  // close current document (since it could be touched by doc browser) if doc browser can't be cancelled
  if(mode == DocumentList::OPEN_DOC) {
    // use doc chooser for split ... would be nice to use doc browser to allow creating new doc, but
    //  confusing UX and we'd need to add a cancel button to browser
    if(!cancelable) {
      if(activeDoc()->nViews < 2)
        activeDoc()->newDocument();
      showDocumentBrowser();
    }
    else
      iosPickDocument(iosChooseDocMode);
  }
  else if(mode == DocumentList::CHOOSE_DOC)
    iosPickDocument(iosInsertDocMode);
  else if(mode == DocumentList::SAVE_DOC)
    iosSaveAs(iosSaveAsMode);
  return "";
#else
  if(!documentList)
    documentList = new DocumentList(docRoot.c_str(), tempPath.c_str());
  documentList->setup(win, DocumentList::Mode_t(mode), exts, cancelable);
  execWindow(documentList);
  return documentList->result > 0 ? documentList->selectedFile : "";
#endif
}

void ScribbleApp::execTagDocList(bool openResult)
{
  if(!tagDocList)
    tagDocList = new TagDocList(tagBrowserRoot().c_str());
  if(libraryTemporary && PLATFORM_ANDROID)
    showNotify(_("Documents are in app storage and will be deleted if Sumi is uninstalled. "
        "Allow access to all files to keep them."), 2);
  const char* currFile = activeDoc() ? activeDoc()->fileName() : "";
  tagDocList->setup(win, currFile && currFile[0]);
  execWindow(tagDocList);
  if(!openResult)
    return;
  if(tagDocList->result == TagDocList::EXISTING_DOC || tagDocList->result == TagDocList::NEW_DOC) {
    if(!tagDocList->selectedFile.empty() && doOpenDocument(tagDocList->selectedFile))
      gotoSelectedPage();
  }
  else if(TagDocList::isImport(tagDocList->result)) {
    importFromBrowser(tagDocList->result);
    execTagDocList(openResult);  // back to the browser; the picked file arrives while it is showing
  }
}

std::string ScribbleApp::tagBrowserRoot() const
{
  if(tagDocList)
    return tagDocList->root().c_str();
  // same effective root DocumentList's currDir starts from (scribbleapp.cpp docRoot is the platform
  // default, e.g. raw $HOME on Linux - fine for DocumentList, which only ever lists one folder the
  // user chose, but this view recurses the whole root unconditionally, so it needs the narrower,
  // user-configured document folder when one is set)
  FSPath root = libraryRoot;
  if(!libraryManaged()) {
    root = cfg->String("currFolder");
    if(!root.isAbsolute())
      root = canonicalPath(root);
    if(!root.isDir() || !root.exists())
      root = docRoot;
  }
  return root.c_str();
}

// a page card in the document browser opens its notebook at that page, not where it was left
void ScribbleApp::gotoSelectedPage()
{
  int pagenum = tagDocList ? tagDocList->selectedPage : -1;
  if(pagenum >= 0 && pagenum < activeDoc()->document->numPages()) {
    activeDoc()->jumpToPage(pagenum);
    activeArea()->flashPageTags(tagDocList->selectedPageTags);
  }
}

/// document library

bool ScribbleApp::libraryManaged() const
{
  return !libraryRoot.empty() && cfg->Bool("useTagDocList") && !PLATFORM_EMSCRIPTEN;
}

bool ScribbleApp::isInLibrary(const std::string& filename) const
{
  // contains() also evens out iOS's /var vs. /private/var, on both sides
  return !libraryRoot.empty() && DocLibrary::contains(libraryRoot, filename);
}

// where a fresh install puts its library: somewhere that survives uninstalling Write (on Android that means
//  shared storage, not Android/data), and on desktop a folder of its own rather than the documents folder
//  itself, since the browser shows everything under its root
std::string ScribbleApp::defaultLibraryBase() const
{
#if PLATFORM_ANDROID
  return "/sdcard/Documents/Sumi/";
#elif PLATFORM_IOS
  // the app's own Documents folder, which is also what the Files app shows as Sumi's
  return FSPath(getenv("HOME"), "Documents/Sumi/").c_str();
#else
  std::string documentsDir = DocLibrary::userDocumentsDir();
  return FSPath(documentsDir.empty() ? docRoot : documentsDir, "Sumi/").c_str();
#endif
}

// used only while the real library cannot be written: app storage on Android (deleted on uninstall, hence
//  the warning), next to the config file elsewhere (e.g. the library is on a drive that is not mounted)
static std::string fallbackLibraryBase(const std::string& cfgFile)
{
#if PLATFORM_ANDROID
  const char* appstorage = SDL_AndroidGetExternalStoragePath();
  return FSPath(appstorage ? appstorage : "/sdcard/Android/data/com.styluslabs.writeqt/files", "Write/").c_str();
#else
  return FSPath(cfgFile).parent().child("library/").c_str();
#endif
}

void ScribbleApp::initLibrary()
{
  // wasm has only a virtual filesystem, so it keeps its own document handling
  if(!cfg->Bool("useTagDocList") || PLATFORM_EMSCRIPTEN)
    return;
  if(!libraryOverride.empty()) {
    // the user named this folder explicitly, so unlike acquire() adopt it even if it already holds
    //  documents (a prepared demo set); no fallback, no moving documents in from the fallback
    FSPath dir(libraryOverride + "/");
    if(createPath(dir.c_str()) && (DocLibrary::isLibraryDir(dir) || DocLibrary::markLibrary(dir))) {
      libraryRoot = libraryTarget = dir.c_str();
      // Save As / Insert still go through DocumentList, which starts in currFolder
      cfg->set("currFolder", libraryRoot.c_str());
      return;
    }
    // falling back to the real library would put the user's own documents in the screenshots
    PLATFORM_LOG("--library: cannot use %s as the document library\n", dir.c_str());
    finish();
    return;
  }
  std::string saved = cfg->String("libraryPath");
#if PLATFORM_IOS
  // the app's container (.../Containers/Data/Application/<UUID>/) can move when the app is updated, and
  //  the config moves with it - so a saved path into the old container means the same folder in this one
  const char* containerDir = "/Containers/Data/Application/";
  size_t containerPos = saved.find(containerDir);
  size_t uuidEnd = containerPos == std::string::npos ? containerPos : saved.find('/', containerPos + strlen(containerDir));
  if(uuidEnd != std::string::npos && getenv("HOME"))
    saved = FSPath(getenv("HOME"), saved.substr(uuidEnd + 1)).c_str();
#endif
  libraryTarget = saved.empty() ? defaultLibraryBase() : FSPath(saved + "/").c_str();
  bool canWrite = true;
#if PLATFORM_ANDROID
  // without all-files access a directory in shared storage may be creatable, but after a reinstall the
  //  files in it are unreadable - so don't put documents there until we have the permission
  canWrite = hasAndroidPermission();
#endif
  if(canWrite) {
    // a saved library is reused as is; only a fresh install goes looking for an empty directory
    if(saved.empty())
      libraryRoot = DocLibrary::acquire(libraryTarget);
    else if(DocLibrary::isLibraryDir(libraryTarget))
      libraryRoot = libraryTarget;
    // the user deleted it, or emptied it marker and all: start a new one there
    else if(DocLibrary::isEmptyDir(libraryTarget)
        || (!FSPath(libraryTarget).exists() && FSPath(libraryTarget).parent().exists()))
      libraryRoot = DocLibrary::acquire(libraryTarget, 1);
  }
  if(!libraryRoot.empty()) {
    libraryTarget = libraryRoot;
    if(saved.empty())
      cfg->set("libraryPath", FSPath(libraryRoot).filePath().c_str());
    // documents written while the library was unavailable
    std::string fallback = fallbackLibraryBase(cfgFile);
    if(DocLibrary::isLibraryDir(fallback) && !DocLibrary::findDocuments(fallback, "svgz svg html", 8).empty()) {
      if(!DocLibrary::moveContents(fallback, libraryRoot))
        PLATFORM_LOG("Some documents could not be moved from %s to %s\n", fallback.c_str(), libraryRoot.c_str());
    }
    return;
  }
  // Permanent location unavailable: work in the fallback for now. libraryPath is deliberately left alone,
  //  so the documents go back where they belong once it is reachable again (adoptPermanentLibrary).
  libraryRoot = DocLibrary::acquire(fallbackLibraryBase(cfgFile));
  libraryTemporary = !libraryRoot.empty();
  PLATFORM_LOG("Document library %s unavailable, using %s\n", libraryTarget.c_str(), libraryRoot.c_str());
#if !PLATFORM_ANDROID
  // Android explains itself every time the browser opens (execTagDocList); here it is a one-off surprise
  if(libraryTemporary && outDoc.empty())
    messageBox(Warning, _("Document library"), fstring(_("The document library at %s is not available. "
        "New documents will be kept in %s and moved to the library when it is available again."),
        libraryTarget.c_str(), libraryRoot.c_str()));
#endif
}

// the permanent location just became writable (Android: all-files access was granted)
void ScribbleApp::adoptPermanentLibrary()
{
  if(!libraryTemporary)
    return;
  // acquire() rather than creating the directory outright: after a reinstall this is how the library the
  //  previous install left behind is found again, marker and all
  std::string permanent = DocLibrary::acquire(libraryTarget);
  if(permanent.empty() || !moveLibraryTo(permanent))
    return;
  libraryTemporary = false;
  showNotify(fstring(_("Documents moved to %s"), permanent.c_str()), 1);
}

// Moves every document in the library to newRoot (an acquired library directory) and follows them: open
//  documents are reopened from their new path, recent documents are rewritten, and the browser is repointed.
bool ScribbleApp::moveLibraryTo(const std::string& newRoot)
{
  std::string oldRoot = libraryRoot;
  std::vector<std::pair<ScribbleDoc*, std::string>> openDocs;  // doc, path relative to the library
  for(ScribbleDoc* doc : scribbleDocs) {
    std::string filename = doc->fileName();
    if(filename.empty() || !isInLibrary(filename))
      continue;
    if(doc->isModified())
      doc->saveDocument();
    openDocs.push_back({doc, FSPath(filename).relativeTo(FSPath(oldRoot))});
  }
  bool allMoved = DocLibrary::moveContents(oldRoot, newRoot);
  libraryRoot = newRoot;
  libraryTarget = newRoot;
  cfg->set("libraryPath", FSPath(newRoot).filePath().c_str());
  for(std::string& recent : recentDocs) {
    if(StringRef(recent).startsWith(oldRoot.c_str()))
      recent = newRoot + recent.substr(oldRoot.size());
  }
  for(auto& openDoc : openDocs) {
    FSPath moved(newRoot, openDoc.second);
    if(moved.exists() && openDoc.first->openDocument(moved.c_str()) == Document::LOAD_OK && openDoc.first == activeDoc())
      onLoadFile(moved.c_str(), false);
  }
  // the other tabs follow: a background one is clean (saved when it was left), so it is simply unloaded
  //  and reloads from its new path when it is next shown
  for(EditorTab& tab : tabs.tabs) {
    if(!StringRef(tab.path).startsWith(oldRoot.c_str()))
      continue;
    if(tab.doc && tab.doc->nViews == 0 && !tab.doc->isModified()) {
      delete tab.doc;
      tab.doc = NULL;
    }
    if(!tab.doc)
      tab.path = newRoot + tab.path.substr(oldRoot.size());
  }
  syncTabs();
  if(tagDocList)
    tagDocList->setRoot(libraryRoot.c_str());
  writeConfigFile();
  if(!allMoved)
    messageBox(Warning, _("Document library"), fstring(_("Some files could not be moved and were left in %s."), oldRoot.c_str()));
  return true;
}

// the library location preference was changed
bool ScribbleApp::relocateLibrary(const std::string& newBase)
{
  std::string expanded = newBase;
  const char* home = getenv("HOME");
  if(home && StringRef(expanded).startsWith("~"))
    expanded.replace(0, 1, home);
  FSPath chosen(expanded + "/");
  if(!chosen.isAbsolute()) {
    messageBox(Warning, _("Document library"), _("Please enter the full path of a folder for the library."));
    return false;
  }
  if(DocLibrary::contains(libraryRoot, chosen)) {
    messageBox(Warning, _("Document library"), _("The library cannot be moved into a folder inside itself."));
    return false;
  }
  // an empty folder (or an existing library) is used as is; a folder with anything else in it gets a Write
  //  folder of its own inside it, the same rule a fresh install follows
  std::string newRoot = (DocLibrary::isLibraryDir(chosen) || DocLibrary::isEmptyDir(chosen) || !chosen.exists())
      ? DocLibrary::acquire(chosen, 1) : DocLibrary::acquire(chosen.child("Sumi/"));
  if(newRoot.empty()) {
    messageBox(Warning, _("Document library"), fstring(_("%s cannot be used for the library."), newBase.c_str()));
    return false;
  }
  if(canonicalPath(FSPath(newRoot)) == canonicalPath(FSPath(libraryRoot)))
    return true;
  auto choice = messageBox(Question, _("Document library"),
      fstring(_("Move all documents from %s to %s?"), libraryRoot.c_str(), newRoot.c_str()), {_("Move"), _("Cancel")});
  if(choice != _("Move"))
    return false;
  libraryTemporary = false;
  return moveLibraryTo(newRoot);
}

// Once, the first time the library is used: documents from wherever Write used to keep them can be copied in.
//  Copied, never moved - the classic browser may still be pointing at them, and the user may have their own
//  arrangement there.
void ScribbleApp::offerLibraryMigration()
{
  if(!libraryManaged() || cfg->Bool("libraryMigrated") || !libraryOverride.empty())
    return;
  cfg->set("libraryMigrated", true);
  std::vector<std::string> sources;
  std::string oldFolder = cfg->String("currFolder");
  if(!oldFolder.empty())
    sources.push_back(oldFolder);
#if PLATFORM_ANDROID
  sources.push_back("/sdcard/styluslabs/write/");
  const char* appstorage = SDL_AndroidGetExternalStoragePath();
  if(appstorage)
    sources.push_back(FSPath(appstorage, "/").c_str());
#else
  sources.push_back(docRoot);
#endif
  // Multi-file HTML documents are left out: they are several files, and copying just the .html would
  //  copy a document with no pages. Depth-limited because a source may be a whole home directory.
  std::string exts = cfg->String("docFileExt") + std::string(" svgz");
  std::vector<FSPath> docs;
  std::set<std::string> seen;
  for(const std::string& source : sources) {
    if(!isDirectory(source.c_str()))
      continue;
    for(const FSPath& doc : DocLibrary::findDocuments(source, exts, 3, libraryRoot)) {
      if(seen.insert(canonicalPath(doc)).second)
        docs.push_back(doc);
    }
  }
  if(docs.empty())
    return;
  auto choice = messageBox(Question, _("Document library"), fstring(_("Sumi now keeps all documents in one "
      "library folder:\n%s\n\nCopy your %d existing documents there? The originals are left where they are."),
      libraryRoot.c_str(), int(docs.size())), {_("Copy"), _("Skip")});
  if(choice != _("Copy"))
    return;
  auto copied = DocLibrary::copyInto(docs, libraryRoot);
  // tag names live in the tag index of whatever root the tag browser was using, i.e. oldFolder
  FSPath oldTags(FSPath(oldFolder + "/").childPath(".write-tags")), newTags(FSPath(libraryRoot).childPath(".write-tags"));
  if(!oldFolder.empty() && oldTags.exists() && !newTags.exists())
    copyFile(oldTags, newTags);
  for(std::string& recent : recentDocs) {
    auto it = copied.find(recent);
    if(it != copied.end())
      recent = it->second;
  }
  if(copied.size() < docs.size())
    messageBox(Warning, _("Document library"),
        fstring(_("%d of %d documents could not be copied."), int(docs.size() - copied.size()), int(docs.size())));
}

// The active document was just opened from outside the library: save it into the library and carry on
//  editing the copy, so the library is the only place documents are ever written.
bool ScribbleApp::importActiveDocToLibrary(const std::string& srcFile)
{
  FSPath dest = DocLibrary::uniquePath(FSPath(libraryRoot), FSPath(srcFile).baseName(), cfg->String("docFileExt"));
  activeDoc()->checkAndClearErrors(true);  // clear errors so document can be saved
  if(!activeDoc()->saveDocument(dest.c_str(), Document::SAVE_FORCE)) {
    messageBox(Error, _("Import document"), fstring(_("%s could not be copied into the document library at %s."),
        FSPath(srcFile).fileName().c_str(), libraryRoot.c_str()));
    return false;
  }
  // the original was registered as recent when it was opened; offering it there would import it again
  recentDocs.erase(std::remove(recentDocs.begin(), recentDocs.end(), srcFile), recentDocs.end());
  onLoadFile(dest.c_str());
  showNotify(fstring(_("Copied %s into the document library"), FSPath(srcFile).fileName().c_str()), 1);
  return true;
}

// "Import Document..." - the classic folder browser's one remaining job: picking a file from anywhere,
//  which doOpenDocument() then copies into the library
bool ScribbleApp::importDocument()
{
  if(!maybeSave())
    return false;
  // doOpenDocument() copies it into the library; the document may have changed while the picker was up
  FilePicker::openFile(_("Import Document"), PdfImport::isAvailable() ? "svgz svg html htm pdf" : "svgz svg html htm",
      [this](const std::string& filename){
    if(!filename.empty() && maybeSave())
      doOpenDocument(filename);
  });
  return true;
}

bool ScribbleApp::openOrCreateDoc(bool cancelable)
{
  // TagDocList has no CHOOSE_DOC/SAVE_DOC modes, so DocumentList (or the iOS pickers) stays for insert
  //  document and save as
  if(libraryManaged())
    return openOrCreateDocTagged(cancelable);
  return openOrCreateDocClassic(cancelable);
}

// TagDocList counterpart of openOrCreateDocClassic(); it creates new documents itself (as DocumentList
//  does), but offers no ruling choice, so NEW_DOC skips that step
bool ScribbleApp::openOrCreateDocTagged(bool cancelable)
{
  backToDocList = true;
  if(!cancelable && PLATFORM_MOBILE)
    cfg->set("reopenLastDoc", false);
  execTagDocList(false);
  int res = tagDocList ? tagDocList->result : TagDocList::REJECTED;
  std::string filename = tagDocList ? tagDocList->selectedFile : "";
  if(res == TagDocList::OPEN_WHITEBOARD) {
    if(!openSharedDoc())
      return openOrCreateDocTagged(cancelable);  // try again
  }
  else if(TagDocList::isImport(res)) {
    importFromBrowser(res);
    return openOrCreateDocTagged(cancelable);  // back to the browser; the picked file arrives while it is showing
  }
  else if(res == TagDocList::EXISTING_DOC && !filename.empty()) {
    if(!doOpenDocument(filename))
      return openOrCreateDocTagged(cancelable);
    gotoSelectedPage();
  }
  else if(res == TagDocList::NEW_DOC && !filename.empty()) {
    if(!doOpenDocument(filename))
      return openOrCreateDocTagged(cancelable);
    applyNewDocChoices(tagDocList->newDocChoices);
    activeDoc()->saveDocument((IOStream*)NULL,
        FSPath(filename).extension() == "html" ? Document::SAVE_MULTIFILE : Document::SAVE_NORMAL);
    askThemeForNewDoc();
  }
  else {  // canceled
#if PLATFORM_ANDROID
    cfg->set("reopenLastDoc", true);
#endif
    checkExtModified();  // handle case of current doc being deleted or renamed from the browser
  }
  return true;
}

bool ScribbleApp::openOrCreateDocClassic(bool cancelable)
{
  backToDocList = true;
  // on Android, reopenLastDoc is not a user pref, but determined by whether user was viewing doc vs. doc list
  if(!cancelable && PLATFORM_MOBILE)
    cfg->set("reopenLastDoc", false);
  // let's always allow doc list to be closed on desktop
  std::string filename = execDocumentList(DocumentList::OPEN_DOC, NULL, cancelable || !PLATFORM_MOBILE);
  int res = documentList ? documentList->result : DocumentList::REJECTED;
  if(res == DocumentList::OPEN_HELP) {
    if(!openHelp()) {
      openURL("http://www.styluslabs.com/write/Help.html");
      return openOrCreateDocClassic(cancelable);  // try again
    }
  }
  else if(filename.empty()) {  // document list canceled
#if PLATFORM_ANDROID
    cfg->set("reopenLastDoc", true);  // reopenLastDoc is cleared by openDocument() on Android
#endif
    checkExtModified();  // handle case of current doc being deleted or renamed via document list
  }
  else if(res == DocumentList::OPEN_WHITEBOARD) {
    //cfg->set("currFolder", filename.c_str());
    if(!openSharedDoc())
      return openOrCreateDocClassic(cancelable);  // try again
  }
  else if(res == DocumentList::EXISTING_DOC) {
    //if(scribbleDoc->fileName != documentList->selectedFile || getFileMTime(currFileName.c_str()) != currFileLastMod)
    return doOpenDocument(filename) || openOrCreateDocClassic(cancelable);
  }
  else if(res == DocumentList::OPEN_COPY) {
    if(!doOpenDocument(documentList->selectedSrcFile))
      return openOrCreateDocClassic(cancelable);  //false;
    if(!activeDoc()->saveDocument(filename.c_str(), Document::SAVE_FORCE)) {
      // this should never happen since doc list creates the file first
      doNewDocument();
      messageBox(Error, _("Save As"), _("Error saving document. Please try a different folder."));
      return false;
    }
    onLoadFile(filename);  // filename not the same as when opened
  }
  else if(res == DocumentList::NEW_DOC) {
    if(!doOpenDocument(filename))
      return openOrCreateDocClassic(cancelable);  //false;
    int ruleidx = documentList->selectedRuling;
    if(ruleidx > 0 && ruleidx < 8) {
      PageProperties props(0, 0, RulingDialog::predefRulings[ruleidx][0],
          RulingDialog::predefRulings[ruleidx][1], RulingDialog::predefRulings[ruleidx][2],
          Color::fromRgb(cfg->Int("pageColor")), Color::fromArgb(cfg->Int("ruleColor")),
          RulingDialog::predefDotRadii[ruleidx]);
      activeDoc()->setPageProperties(&props, true, true, false, false);
    }
    // force multi-file for .html extension, since new file will default to single file because _page001.svg
    //  doesn't exist; user can use .htm for single file doc
    if(FSPath(filename).extension() == "html")
      activeDoc()->saveDocument((IOStream*)NULL, Document::SAVE_MULTIFILE);
    askThemeForNewDoc();
  }
  return true;
}

bool ScribbleApp::openHelp()
{
  if(!maybeSave())
    return false;
  const char* helpdoc = "Intro.svg";
  FSPath helpTemp = FSPath(tempPath).child(helpdoc);
  if(helpTemp.exists())
    removeFile(helpTemp.c_str());
#if PLATFORM_ANDROID
  bool ok = AndroidHelper::rawResourceToFile("Intro.svg", helpTemp.c_str());
#else
  FSPath helpSrc(appDir, helpdoc);
  bool ok = helpSrc.exists() && copyFile(helpSrc, helpTemp);
#endif
  if(ok && activeDoc()->openDocument(helpTemp.c_str()) != Document::LOAD_FATAL) {
    onLoadFile(activeDoc()->fileName(), false);  // add to recents = false
    // this is a hack to prevent crash casting to UIDocStream for thumbnail on iOS
    activeDoc()->cfg->set("saveThumbnail", false);
    return true;
  }
  return false;
}

std::string ScribbleApp::docDisplayName(std::string filename, int maxwidth)
{
#if PLATFORM_IOS
  return docShortName(filename, maxwidth);
#else
  if(filename.empty())
    return _("Untitled");  //New Document" -- this makes title button look like a new doc button
#if PLATFORM_ANDROID
  StringRef fref(filename);
  if(fref.startsWith(docRoot.c_str()))
    fref += docRoot.size();
  if(fref.endsWith(fileExt.c_str()))
    fref.chop(fileExt.size() + 1);
  filename = fref.toString();
#else
  const char* homestr = getenv("HOME");
  if(homestr && StringRef(filename).startsWith(homestr))
    filename.replace(0, strlen(homestr), "~");
#endif
  // elide from left if necessary
  /*if(maxwidth > 0) {
    Dim textwidth = Painter::textBounds(0, 0, filename.c_str());
    if(textwidth > maxwidth) {
      int tokeep = (filename.size()*maxwidth)/textwidth - 3.5;
      filename.replace(0, filename.size() - tokeep, "...");
    }
  }*/
  return filename;
#endif
}

std::string ScribbleApp::docShortName(const std::string& filename, int maxwidth)
{
  return filename.empty() ? _("Untitled") : FSPath(filename).baseName();
}

void ScribbleApp::setWinTitle(const std::string& filename)
{
  const char* winTitle =
      SCRIBBLE_DEBUG ? (" - Sumi (r" PPVALUE_TO_STRING(SCRIBBLE_REV_NUMBER) ")") : " - Sumi";
  win->setTitle((docDisplayName(filename) + winTitle).c_str());
  win->titleStr = docShortName(filename);  // storage for full title string, since button text may be elided
  win->titleButton->setText(win->titleStr.c_str());
  win->titleButton->setShowTitle(true);  // text may have been hidden by layout
}

// to be called after document successfully loaded
void ScribbleApp::onLoadFile(const std::string& filename, bool addrecent)
{
  dismissNotify();  // close any old notifications
  setWinTitle(filename);
  activeArea()->widget->fileNameLabel->setText(docShortName(filename).c_str());

  if(!filename.empty() && addrecent) {
    FSPath fileinfo(filename);
    // update recent docs; no automatic way to find or remove string w/o case sensitivity
    for(size_t ii = 0; ii < recentDocs.size(); ++ii) {
      if(strcasecmp(filename.c_str(), recentDocs[ii].c_str()) == 0) {
        recentDocs.erase(recentDocs.begin() + ii);
        break;
      }
    }
    // unbelieveably didn't have any limit on growth of recentDocs previously
    while(recentDocs.size() > 10)
      recentDocs.pop_back();
    // populate list before adding current file so it isn't shown on list while open
    populateRecentFiles();
#if PLATFORM_IOS
    // since we don't save security scoped bookmarks (yet?), don't add external files to recents
    if(!StringRef(filename).startsWith(docRoot.c_str()))
      return;
#endif
    recentDocs.insert(recentDocs.begin(), filename);
    //cfg->set("currFolder", fileinfo.parentPath().c_str());  -- now handled by DocumentList
  }
  else
    populateRecentFiles();
  syncTabs();
}

void ScribbleApp::newDocument()
{
  if(maybeSave()) {
    // DocumentList creates a new blank HTML file, which we then open
    if(!openOrCreateDoc())
      doNewDocument();
  }
}

void ScribbleApp::doNewDocument()
{
  // a tab whose file is still there stays open behind the new document; one deleted outside the app
  //  (checkExtModified) is reset to untitled, which closes its tab
  ScribbleDoc* doc = activeDoc();
  if(tabsEnabled() && tabs.findDoc(doc) >= 0 && FSPath(doc->fileName()).exists())
    showUntitledInArea(activeArea());
  else
    doc->newDocument();
  onLoadFile("");
  askThemeForNewDoc();
}

// Picking the theme belongs at the start, while the document is still empty: it is the one moment the
//  choice costs nothing, and a theme chosen later has to be reconciled with what is already drawn.
//
// Called from *both* new-document paths.  doNewDocument() is only the fallback used when there is no
//  document list; the ordinary desktop route is openOrCreateDoc()'s NEW_DOC branch, which creates a
//  file and opens it. Hooking only one of them is why this looked like it did nothing.
// Not hooked in ScribbleDoc::newDocument(), which is also the reset path used by document close and by
//  ScribbleTest - neither may open a dialog.
// What the Create Notebook dialog chose.  The layout becomes the document's default as well as its first
//  page's, so Add Page continues in it.  Saved straight away by the caller: the cover and the tags are what
//  the document list shows for this notebook, and an untouched new document is otherwise never written.
void ScribbleApp::applyNewDocChoices(const NewDocChoices& choices)
{
  ScribbleDoc* doc = activeDoc();
  PageProperties props = AddPageMenu::layoutToProps(choices.layout, doc->cfg);
  doc->setPageProperties(&props, true, true, false, false);
  doc->cfg->set("coverColor", int(choices.coverColor.argb()));
  doc->cfg->set("tags", TagStore::formatTagList(choices.tagIds).c_str());
}

void ScribbleApp::askThemeForNewDoc()
{
  if(cfg->Bool("themeAskOnNew", true))
    asyncDialog(new ThemeDialog(activeDoc()));
}

bool ScribbleApp::openDocument(std::string filename)
{
  if(documentList && documentList->isVisible())
    documentList->finish(DocumentList::REJECTED);
  if(tagDocList && tagDocList->isVisible())
    tagDocList->finish(TagDocList::REJECTED);
  backToDocList = false;
  // don't save clean doc - on Android, I think doc will always be clean when we're called
  return maybeSave() && doOpenDocument(filename);  //!activeDoc()->isModified() ||
}

bool ScribbleApp::openDocument()
{
  // on Android, we want doc list to be cancelable, but we still want to turn off reopenLastDoc
#if PLATFORM_ANDROID
  cfg->set("reopenLastDoc", false);
#endif
  // cancelable, except for the iOS system browser, which has no way back to the document
  return maybeSave() && openOrCreateDoc(!PLATFORM_IOS || libraryManaged());
}

bool ScribbleApp::doOpenDocument(std::string filename)
{
  // a PDF isn't a Write document - import it (which then opens the document it produces).  Handling
  // this here rather than at each call site covers the command line, drag and drop, the document
  // list and Android intents in one place.
  if(PdfImport::isPdfFile(filename.c_str()))
    return doImportPdf(filename);

  // if SVG file, try to find the parent HTML file
  if(FSPath(filename).extension() == "svg") {
    // if user is trying to open a _pageXXX.svg file (not just any SVG file), prompt to open whole document
    // we do not check for existance of of HTML file, since Document::load will try to open all SVG files if
    //  the HTML file is missing
    size_t chopat = filename.rfind("_page");
    if(chopat != std::string::npos) {
      std::string htmlfile = filename.substr(0, chopat) + ".html";  // + fileExt;
      auto choice = messageBox(Question, _("Sumi"),
          _("You are attempting to open a single page SVG file. Would you like to try opening the entire document instead?"),
          {_("Yes"), _("No")});
      if(choice == _("Yes"))
        filename = htmlfile;
    }
  }
  // Opened from outside the library (drag and drop, an Android intent, Import Document...): read the
  //  original, never write it, and continue in a copy saved into the library.  This also makes a
  //  read-only original a non-issue, so the Save As prompt below is never reached for one.
  if(libraryManaged() && !isInLibrary(filename)) {
    FileStream* srcStrm = new FileStream(filename.c_str(), "rb");
    if(!doOpenDocument(srcStrm))
      return false;
    if(importActiveDocToLibrary(filename))
      return true;
    srcStrm->filename.clear();  // import failed: still never save back over the original
    return false;
  }
  FileStream* strm = new FileStream(filename.c_str(), "rb+");
  if(strm->is_open())
    return doOpenDocument(strm);
  // try opening as read-only
  strm->open("rb");
  if(!doOpenDocument(strm))  // still need to show error even if we can't open stream at all
    return false;
  // successfully opened read-only
  strm->filename.clear();  // prevent attempts to save to read-only location
#if PLATFORM_ANDROID
  requestAndroidPermission();
#endif
  messageBox(Warning, _("Read-only file"),
      _("The document cannot be saved to its current location, please choose a new location."));
  return doSaveAs();
}

bool ScribbleApp::doOpenDocument(IOStream* filestrm)
{
  std::string filename = filestrm->name();
  // Editor tabs: a document already open is switched to, not reloaded (which would drop its undo
  //  history); any other gets its own ScribbleDoc, so the one being left stays open as a tab.  An
  //  untitled document being left is not a tab, and is opened over as it always was.
  ScribbleDoc* leftDoc = NULL;
  TabViewState leftView;
  if(tabsEnabled() && !filename.empty()) {
    int existing = tabs.find(filename);
    if(existing >= 0) {
      delete filestrm;
      if(!showTabInArea(existing, activeArea()))
        return false;
      onLoadFile(activeDoc()->fileName());
      return true;
    }
    ScribbleDoc* prev = activeDoc();
    if(tabs.findDoc(prev) >= 0 || prev->nViews > 1) {
      tabInsertAnchor = tabs.findDoc(prev);
      leftDoc = prev;
      DocPosition pos = activeArea()->getPos();
      leftView.pagenum = pos.pagenum;
      leftView.x = pos.pos.x;
      leftView.y = pos.pos.y;
      leftView.zoom = activeArea()->getZoom();
      ScribbleDoc* doc = new ScribbleDoc(this, cfg, scribbleMode);
      detachDoc(activeArea());
      attachDoc(doc, activeArea(), NULL);
      scribbleDocs.push_back(doc);
    }
  }
  else if(activeDoc()->nViews > 1) {
    if(filename == activeDoc()->fileName())
      return true;  // same doc selected for split ... do nothing
    // single doc split -> separate doc split
    if(scribbleDocs.size() < 2) {
      ScribbleDoc* doc = new ScribbleDoc(this, cfg, scribbleMode);
      doc->newDocument();
      scribbleDocs.push_back(doc);
    }
    ScribbleDoc* otherDoc = (activeDoc() == scribbleDocs[0]) ? scribbleDocs[1] : scribbleDocs[0];
    activeDoc()->removeArea(activeArea());
    otherDoc->addArea(activeArea()); // this makes otherDoc the active doc
    repaintBookmarks(true);
    for(ScribbleArea* area : scribbleAreas) {
      if(area != activeArea())
        area->widget->fileNameLabel->setText(docShortName(area->scribbleDoc->fileName()).c_str());
      area->widget->fileNameLabel->setVisible(true);
    }
  }
  else if(scribbleDocs.size() > 1) {
    ScribbleDoc* otherDoc = (activeDoc() == scribbleDocs[0]) ? scribbleDocs[1] : scribbleDocs[0];
    if(filename == otherDoc->fileName()) {
      // separate doc split -> single doc split
      activeDoc()->newDocument();  // close the doc being hidden!
      activeDoc()->removeArea(activeArea());
      otherDoc->addArea(activeArea()); // this makes otherDoc the active doc
      repaintBookmarks(true);
      for(ScribbleArea* area : scribbleAreas)
        area->widget->fileNameLabel->setVisible(false);
      onLoadFile(activeDoc()->fileName());
      delete filestrm;
      return true;  // do not reload doc in this case
    }
  }

  // try to open document - note that even if filestrm is not open, this could work (e.g. for missing .html)
  Document::loadresult_t res = activeDoc()->openDocument(filestrm);
  if(tabsEnabled()) {
    if(res == Document::LOAD_FATAL && leftDoc) {
      // back to the document that was left, where it was
      ScribbleDoc* failed = detachDoc(activeArea());
      attachDoc(leftDoc, activeArea(), &leftView);
      rebuildShownDocs();  // deletes the failed one
      (void)failed;
      tabInsertAnchor = -1;
      repaintBookmarks(true);
    }
    else
      rebuildShownDocs();
    updateSplitLabels();
  }
  if(res == Document::LOAD_OK || res == Document::LOAD_EMPTYDOC) {
    onLoadFile(activeDoc()->fileName());
#if PLATFORM_MOBILE
    cfg->set("reopenLastDoc", true);
#endif
    return true;
  }
  else if(res != Document::LOAD_FATAL) {
    activeDoc()->checkAndClearErrors(true);  // clear errors so document can be saved
    onLoadFile(activeDoc()->fileName());
    if(res == Document::LOAD_NEWERVERSION) {
      messageBox(Warning, _("Newer document"), _("This document was created with a more recent"
          " version of Sumi. Saving with this version may result in data loss."));
    }
    else if(res == Document::LOAD_NONWRITE) {
      return openExternalDoc();
    }
    else {
      // we used to automatically save a copy, but on iOS, we cannot create new document outside app
      //  folder w/o user interaction; prompting makes things clearer for user anyway
      auto choice = messageBox(Warning, _("Damaged document"), _("Errors occurred while opening the document.  "
          "To avoid possible data loss, a copy should be saved."), {_("Save a copy"), _("Open anyway")});
      if(choice == _("Save a copy"))
        doSaveAs();
    }
    return true;
  }
#if PLATFORM_ANDROID
  if(requestAndroidPermission())
    return false;
#endif
  messageBox(Warning, _("Error opening document"), fstring(_("\"%s\" could not be opened."), filename.c_str()));
  return false;  //openOrCreateDoc() && !PLATFORM_IOS;
}

// handle opening external SVG (vs. importExternalDoc, which handles loading external doc to Clipboard)
bool ScribbleApp::openExternalDoc()
{
  auto choice = messageBox(Warning, _("Foreign document"), _("This file does not appear to be a Sumi document."
      "  Saving with Sumi could result in data loss - you will be prompted to save a copy."),
      {_("Use as background"), _("Ungroup all"), _("No change")});
  if(choice == _("Ungroup all")) {
    for(Page* page : activeDoc()->document->pages) {
      for(int ii = 0; ii < 32; ++ii) {  // counter is just to protect against infinite loop
        Selection sel(page);
        sel.selectAll();
        if(!sel.containsGroup())
          break;
        sel.ungroup();
      }
    }
  }
  else if(choice == _("Use as background")) {
    for(Page* page : activeDoc()->document->pages)
      page->contentToRuling();
  }
  doSaveAs();
  return true;
}

bool ScribbleApp::maybeSave(bool prompt)
{
  ScribbleDoc* doc = activeDoc();
  if(!doc->fileName()[0]) {
    if(!doc->isModified())
      return true;
    auto ret = messageBox(Question, _("Unsaved document"),
        _("The document has never been saved.\nWhat would you like to do?"),
        {_("Save"), _("Discard"), _("Cancel")});
    return (ret == _("Save")) ? doSaveAs() : (ret == _("Discard"));
  }
  if(prompt || doc->cfg->Bool("savePrompt")) {
    if(!doc->isModified())
      return true;
    auto ret = messageBox(Warning, _("Modified document"),
        fstring(_("Save changes to \"%s\"?"), docDisplayName(doc->fileName()).c_str()),
        {_("Save"), _("Discard"), _("Cancel")});
    if(ret != _("Save"))
      return (ret == _("Discard"));
  }
  // isEmptyFile(): we always save if doc loaded from empty file so that it has a thumbnail
  //  perhaps we should save immediately after loading empty file instead
  if(doc->isModified() || doc->cfg->Bool("saveUnmodified") || doc->document->isEmptyFile())
    return saveDocument();
  // no need to save ... in the future, we might save doc state to config here
  return true;
}

// close docs affected by document list file operation; one remaining bug is with split view filename label
void ScribbleApp::closeDocs(const FSPath& path)
{
  for(ScribbleDoc* doc : scribbleDocs) {
    if(StringRef(doc->fileName()).startsWith(path.c_str()) && !doc->isModified()) {
      doc->newDocument();
      if(activeDoc() == doc)
        onLoadFile("");
    }
  }
  // tabs no pane shows go with their files (they are saved, so nothing is lost); shown ones were reset
  //  to untitled above, which syncTabs() takes as closing them
  for(int ii = tabs.size() - 1; ii >= 0; --ii) {
    ScribbleDoc* doc = tabs[ii].doc;
    if((!doc || doc->nViews == 0) && StringRef(tabs[ii].path).startsWith(path.c_str())
        && !(doc && doc->isModified())) {
      tabs.remove(ii);
      releaseDocIfOrphan(doc);
    }
  }
  syncTabs();
}

// returns true to indicate scribbleDoc is synced with disk file (so no save/loaded needed)
// An alternative would be QFileSystemWatcher, but then we'd have to deal with it being triggered by our own
//  writes to file
bool ScribbleApp::checkExtModified()
{
  for(ScribbleDoc* doc : scribbleDocs) {
    if(doc->activeArea)
      checkExtModified(doc);
  }
  return false;
}

bool ScribbleApp::checkExtModified(ScribbleDoc* doc)
{
#if PLATFORM_IOS || PLATFORM_EMSCRIPTEN
  return false;
#else
  std::string filename = doc->fileName();
  if(filename.empty() || doc->fileLastMod == 0 || !cfg->Bool("warnExtModified"))
    return false;
  const Timestamp lastmod = getFileMTime(filename.c_str());
  if(lastmod == doc->fileLastMod)
    return false;
  doc->fileLastMod = 0;  // prevent additional alerts until document is saved or reloaded

  setActiveArea(doc->activeArea);
  // just reload if our version isn't modified
  if(!doc->isModified()) {
    // assume external deletion was intentional
    if(!FSPath(filename).exists()) {
      doNewDocument();
      return true;
    }
    if(doc->openDocument(filename.c_str()) == Document::LOAD_OK) {
      // don't bother displaying message if doc list is on top
      if(documentList == NULL || !documentList->isVisible()) {
        messageBox(Info, _("Document reloaded"),
            fstring(_("%s has been reloaded due to modification outside of Sumi."), docShortName(filename).c_str()));
      }
      return true;
    }
  }
#ifdef QT_CORE_LIB
  // if we can't display a message box, just save to new file
  if(QGuiApplication::applicationState() == Qt::ApplicationSuspended) {
    doSaveAs();
    QMessageBox::information(this, "Sumi",
        "Document was saved to new file because original was modified outside Sumi.");
    return true;
  }
#endif
  // the bad case: conflict between local and disk versions
  auto choice = messageBox(Warning, _("Save conflict"),
      fstring(_("%s has been modified outside Sumi.\nWhat would you like to do?"), docDisplayName(filename).c_str()),
      {_("Keep Both"), _("Discard Other"), _("Discard Current")});
  if(choice == _("Discard Other") && doc->saveDocument(filename.c_str())) {}
  else if(choice == _("Discard Current") && doc->openDocument(filename.c_str()) == Document::LOAD_OK) {}
  else
    doSaveAs();
  // this will cause doSave() to return immediately
  return true;
#endif
}

// this handles "Save" button
bool ScribbleApp::saveDocument()
{
  return doSave(activeDoc()) || doSaveAs();
}

bool ScribbleApp::doSave(ScribbleDoc* doc)
{
  if(!doc->fileName()[0])
    return false;
  if(checkExtModified(doc))
    return true;
  if(doc->saveDocument())
    return true;
  messageBox(Warning, _("Save error"), _("An error occurred saving the document.  Please try saving"
      " to a different location.  Contact support@styluslabs.com if this error persists."));
  return false;
}

#if PLATFORM_EMSCRIPTEN
#include "emscripten.h"

EM_JS(void, jsSaveFile, (const char* filename, const void* data, int len),
{
  const view = new Uint8Array(Module.HEAPU8.buffer, data, len);
  const blob = new Blob([view], { type: 'octet/stream' });
  const url = window.URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.style = 'display:none';
  document.body.appendChild(a);
  a.href = url;
  a.download = UTF8ToString(filename);
  a.click();
  //setTimeout(function() { ... }, 0)
  window.URL.revokeObjectURL(url);
  document.body.removeChild(a);
});

bool ScribbleApp::doSaveAs()
{
  // Document will take ownership of memstream ... consider a flag to prevent this
  MemStream* strm = new MemStream;
  activeDoc()->checkAndClearErrors(true);  // clear errors so document can be saved
  activeDoc()->saveDocument(strm, Document::SAVE_FORCE);
  jsSaveFile("Untitled.svgz", strm->data(), strm->size());
  return true;
}
#else
#if PLATFORM_MOBILE
// Save As on Android and iOS: the system's export dialog gets a copy, and the document stays in the
//  library - nothing outside the app's storage can be edited in place there. iOS without the library keeps
//  its UIDocument based Save As (iosSaveAs, through execDocumentList).
bool ScribbleApp::doSaveAs()
{
  ScribbleDoc* doc = activeDoc();
  doc->checkAndClearErrors(true);  // clear errors so document can be saved
  if(PLATFORM_IOS && !libraryManaged()) {
    execDocumentList(DocumentList::SAVE_DOC, "pdf svgz svg html htm");
    return false;
  }
  if(!doc->fileName()[0]) {
    // never saved: it gets a name in the library first, as a document created from the browser would
    if(!libraryManaged()) {
      execDocumentList(DocumentList::SAVE_DOC, "pdf svgz svg html htm");
      return false;
    }
    FSPath dest = DocLibrary::uniquePath(FSPath(libraryRoot), "Untitled", cfg->String("docFileExt"));
    if(!doc->saveDocument(dest.c_str(), Document::SAVE_FORCE)) {
      messageBox(Error, _("Save As"), _("Error saving document."));
      return false;
    }
    onLoadFile(dest.c_str());
  }
  else if(!saveDocument())
    return false;
  std::string src = doc->fileName();
  FilePicker::saveFile(_("Save As"), FSPath(src).fileName(), FSPath(src).extension().c_str(),
      [src](const std::string& dest){ return copyFile(FSPath(src), FSPath(dest)); },
      [this](const std::string& dest){ if(!dest.empty()) showNotify(_("Saved a copy"), 1); });
  return true;
}
#else
bool ScribbleApp::doSaveAs()
{
  activeDoc()->checkAndClearErrors(true);  // clear errors so document can be saved
  // standard UX is to always create document file before editing, but on desktop the document list can
  //  be cancelled to allow editing a new unnamed file; the name comes from the system's save dialog
  const char* currName = activeDoc()->fileName();
  std::string suggested = (currName[0] ? FSPath(currName).baseName() : std::string("Untitled"))
      + "." + cfg->String("docFileExt");
  // the document's own format first: a dialog that adds no extension gets that one
  std::string exts = cfg->String("docFileExt");
  for(const char* ext : {"svgz", "svg", "html", "htm", "pdf"}) {
    if(exts != ext)
      exts += std::string(" ") + ext;
  }
  std::string filename = FilePicker::savePath(_("Save As"), suggested, exts.c_str());
  if(filename.empty())
    return false;
  if(FSPath(filename).extension() == "pdf") {
    writePDF(filename);
    return false;  // exporting PDF doesn't count as saving!
  }
  auto flags = FSPath(filename).extension() == "html" ? Document::SAVE_MULTIFILE : 0;
  if(!activeDoc()->saveDocument(filename.c_str(), Document::SAVE_FORCE | flags)) {
    messageBox(Error, _("Save As"), _("Error saving document. Please try a different folder."));
    return false;
  }
  onLoadFile(filename);
  //syncRequired = true;
  return true;
}
#endif  // PLATFORM_MOBILE
#endif  // PLATFORM_EMSCRIPTEN

// for now, this is actually "Discard Changes"
void ScribbleApp::revert()
{
  // don't prompt unless document is modified; just use standard save prompt message
  if(activeDoc()->isModified() && !maybeSave(true))
    return;
  // always create a new document so we don't ask user again if they cancel doc list
  doNewDocument();
  // show doc list if enabled
  openOrCreateDoc();
}

// toappend defaults to " - recovered"
std::string ScribbleApp::createRecoveryName(std::string filename, const char* toappend)
{
  if(FSPath(filename).extension() == fileExt)
    filename.erase(filename.size() - fileExt.size() - 1);
  filename.append(toappend);
  // find available file name
  std::string recovfile = filename + "." + fileExt;
  for(int suffix = 1; FSPath(recovfile).exists(); suffix++)
    recovfile = fstring("%s (%d).%s", filename.c_str(), suffix, fileExt.c_str());
  return recovfile;
}

/// recent files ///

// what to do if recentDocs is empty?
void ScribbleApp::populateRecentFiles()
{
  // recent files menu is optional
  if(!win->menuRecent_Files)
    return;

  SDL_Rect screenrect;
  SDL_GetDisplayBounds(std::max(0, SDL_GetWindowDisplayIndex(sdlWindow)), &screenrect);  // or SDL_GetDisplayUsableBounds
  int maxwidth = 0.75*MIN(screenrect.w, screenrect.h);
  for(size_t ii = 0; ii < recentDocs.size(); ++ii) {
    if(recentFileActions.size() <= ii) {
      // should we use MainWindow::createAction instead?
      recentFileActions.emplace_back(new Action(fstring("recentFile%d", ii).c_str(), ""));
      win->menuRecent_Files->addAction(recentFileActions[ii].get());
    }
    std::string filename = recentDocs[ii];
    recentFileActions[ii]->setVisible(true);
    std::string shortname = docDisplayName(recentDocs[ii], maxwidth);
    recentFileActions[ii]->setTitle(fstring("%d %s", ii+1, shortname.c_str()).c_str());
    recentFileActions[ii]->onTriggered = [this, filename](){ openRecentFile(filename); };
    //recentFileActions[ii]->setUserData(recentDocs[ii]);
  }
  for(size_t ii = recentDocs.size(); ii < recentFileActions.size(); ++ii)
    recentFileActions[ii]->setVisible(false);
}

void ScribbleApp::openRecentFile(const std::string& filename)
{
  // maybeSave updates recent files, so we must save filename before we call it!
  if(maybeSave()) {
    // don't show error message if file has been deleted
    if(!FSPath(filename).exists() || !doOpenDocument(filename)) {
      // if opening doc fails, remove from recent doc list
      recentDocs.erase(std::remove(recentDocs.begin(), recentDocs.end(), filename), recentDocs.end());
      populateRecentFiles();
    }
  }
}

/// images ///

/// PDF import

// Busy box for work done on the UI thread (PDF and Noteful import).  Nothing is drawn while that work runs,
//  so a notification set before it never appeared and the app looked frozen; update() paints the box
//  itself.  It also pumps the OS queue (events are only queued, not dispatched, so nothing re-enters) so
//  the window isn't flagged as not responding.  Shown over the top window - the browser as well as the
//  main window - and not at all when there is no window yet (command line conversion).
class ProgressBox
{
public:
  ProgressBox(const std::string& title)
  {
    SvgGui* gui = Application::gui;
    if(!gui || gui->windows.empty() || !gui->windows.front()->isVisible())
      return;
    dialog = createPopupDialog(title.c_str());
    Widget* body = dialog->selectFirst(".body-container");
    // the dialog keeps the size of its first layout, and a longer message ("page 10 of 120") was then
    //  squeezed into it - so reserve the width up front
    SvgRect* sizer = new SvgRect(Rect::wh(std::min(Dim(420), 0.8*gui->windows.front()->winBounds().width()), 0));
    sizer->setAttribute("fill", "none");
    body->containerNode()->addChild(sizer);
    msgText = new TextBox(createTextNode(""));
    body->addWidget(msgText);
    gui->showModal(dialog, gui->windows.front()->modalOrSelf());
  }

  ~ProgressBox() { close(); }

  void close()
  {
    if(!dialog) return;
    Application::gui->closeWindow(dialog);
    delete dialog;
    dialog = NULL;
  }

  void update(const std::string& msg)
  {
    if(!dialog) return;
    msgText->setText(msg.c_str());
    // pages of a small PDF go by faster than they could be drawn
    int64_t now = mSecSinceEpoch();
    if(now - lastDraw < 100) return;
    lastDraw = now;
    SDL_PumpEvents();
    Application::layoutAndDraw();
  }

private:
  Dialog* dialog = NULL;
  TextBox* msgText = NULL;
  int64_t lastDraw = 0;
};

void ScribbleApp::importPDF()
{
  if(!PdfImport::isAvailable()) {
    messageBox(Warning, _("Import PDF"), _("This build of Sumi does not include PDF support."));
    return;
  }
  if(!maybeSave())
    return;
  FilePicker::openFile(_("Import PDF"), "pdf", [this](const std::string& filename){
    if(!filename.empty() && maybeSave())
      doImportPdf(filename);
  });
}

// Renders every page of the PDF into a new Write document saved alongside the PDF, then opens it.
// Going through a file (instead of mutating the open document in place) keeps the undo history, the
// views and the sync machinery out of it entirely - the app just opens an ordinary document.
bool ScribbleApp::doImportPdf(const std::string& pdfPath)
{
  std::string err;
  std::string docPath = importPdfToDocFile(pdfPath, &err);
  if(docPath.empty()) {
    messageBox(Warning, _("Import PDF"),
        fstring(_("Error importing %s: %s"), FSPath(pdfPath).fileName().c_str(), err.c_str()));
    return false;
  }
  return doOpenDocument(docPath);
}

// Insert PDF: the PDF's pages go into the open document, after the current page (as Insert Document does),
//  not into a new document.  They are rendered exactly as for Import PDF (importPdf: same DPI, page size and
//  rule-layer background, no ruling), into a scratch Document that ScribbleDoc::insertPagesFrom then moves in
//  as one undo step - so Undo removes the whole PDF and sync sends it as a page insertion.  Unlike Import PDF
//  nothing is streamed to disk: the pages (encoded images only) are held until the document is saved.
void ScribbleApp::insertPDF()
{
  if(!PdfImport::isAvailable()) {
    messageBox(Warning, _("Insert PDF"), _("This build of Sumi does not include PDF support."));
    return;
  }
  FilePicker::openFile(_("Insert PDF"), "pdf", [this](const std::string& filename){
    if(!filename.empty())
      insertPdfPages(filename);
  });
}

bool ScribbleApp::insertPdfPages(const std::string& pdfPath)
{
  std::string fileName = FSPath(pdfPath).fileName();
  ProgressBox progress(_("Insert PDF"));
  progress.update(fstring(_("Importing %s..."), fileName.c_str()));
  PdfImport::MemoryBudget budget(importMemoryLimit());
  PdfImport::Options opts;
  opts.dpi = std::max(72, cfg->Int("pdfImportDPI"));
  opts.lossy = cfg->Bool("pdfImportLossy");
  opts.budget = &budget;
  opts.onProgress = [&](int pageNum, int numPages) {
    progress.update(fstring(_("Importing %s (page %d of %d)..."), fileName.c_str(), pageNum + 1, numPages));
    return true;
  };
  std::string err;
  Document pdfdoc;
  int numPages = PdfImport::importPdf(&pdfdoc, pdfPath.c_str(), opts, &err);
  if(numPages <= 0 || activeDoc()->insertPagesFrom(&pdfdoc) <= 0) {
    progress.close();
    messageBox(Warning, _("Insert PDF"),
        fstring(_("Error importing %s: %s"), fileName.c_str(), err.c_str()));
    return false;
  }
  progress.close();
  reportReducedPages(budget, _("Insert PDF"));
  return true;
}

// Insert Document ("+" menu): one page of a PDF as an image on the current page - content, not a page of its
//  own (Insert PDF adds pages).  A PDF of several pages asks which one, first page suggested.  Rendered like
//  Import PDF (pdfImportDPI, JPEG when pdfImportLossy) and placed by ScribbleArea::insertImage at the PDF
//  page's real size, shrunk to fit the page, so it is one undoable paste that selects it for moving.
void ScribbleApp::insertPdfAsImage()
{
  if(!PdfImport::isAvailable()) {
    messageBox(Warning, _("Insert Document"), _("This build of Sumi does not include PDF support."));
    return;
  }
  FilePicker::openFile(_("Insert Document"), "pdf", [this](const std::string& filename){
    if(!filename.empty())
      insertPdfPageAsImage(filename);
  });
}

bool ScribbleApp::insertPdfPageAsImage(const std::string& pdfPath)
{
  std::string fileName = FSPath(pdfPath).fileName();
  PdfImport::MemoryBudget budget(importMemoryLimit());
  PdfImport::Renderer renderer;
  if(!renderer.open(pdfPath.c_str(), NULL, &budget) || renderer.numPages() <= 0) {
    messageBox(Warning, _("Insert Document"),
        fstring(_("Error importing %s: %s"), fileName.c_str(), renderer.error()));
    return false;
  }
  int pageCount = renderer.numPages();
  int pageIndex = 0;
  if(pageCount > 1) {
    TagNameDialog dialog(fstring(_("Insert page (1-%d)"), pageCount).c_str(), "1");
    if(Application::execDialog(&dialog) != Dialog::ACCEPTED)
      return false;
    pageIndex = std::min(std::max(atoi(dialog.getName().c_str()), 1), pageCount) - 1;
  }
  Dim widthPt = 0, heightPt = 0;
  Image pageImage = renderer.render(pageIndex, std::max(72, cfg->Int("pdfImportDPI")),
      cfg->Bool("pdfImportLossy") ? Image::JPEG : Image::PNG, &widthPt, &heightPt);
  if(pageImage.isNull()) {
    messageBox(Warning, _("Insert Document"),
        fstring(_("Error importing %s: %s"), fileName.c_str(), renderer.error()));
    return false;
  }
  activeArea()->insertImage(std::move(pageImage), Rect(), widthPt*PdfImport::UNITS_PER_POINT,
      heightPt*PdfImport::UNITS_PER_POINT);
  reportReducedPages(budget, _("Insert Document"));
  return true;
}

// Does the actual work of doImportPdf(), without touching the UI beyond a status notification, and
// returns the path of the document it created (empty on failure).  Kept separate so the command line
// can import a PDF and then follow exactly the same path as any other document it is given.
std::string ScribbleApp::importPdfToDocFile(const std::string& pdfPath, std::string* errorOut)
{
  if(!PdfImport::isAvailable()) {
    if(errorOut) *errorOut = _("This build of Sumi does not include PDF support.");
    return std::string();
  }
  FSPath pdfinfo(pdfPath);
  // into the library; a command line conversion (--out) only needs a scratch copy, and without a library
  //  the document goes next to the PDF as it always did
  FSPath outDir = pdfinfo.parent();
  if(!outDoc.empty()) {
    outDir = FSPath(tempPath);
    createPath(outDir);
  }
  else if(libraryManaged())
    outDir = FSPath(libraryRoot);
  // don't overwrite an existing document
  FSPath outinfo = DocLibrary::uniquePath(outDir, pdfinfo.baseName(), "svgz");

  // rendering is synchronous and can take a while for a long PDF
  std::string fileName = pdfinfo.fileName();
  ProgressBox progress(_("Import PDF"));
  progress.update(fstring(_("Importing %s..."), fileName.c_str()));

  PdfImport::MemoryBudget budget(importMemoryLimit());
  PdfImport::Options opts;
  opts.dpi = std::max(72, cfg->Int("pdfImportDPI"));
  opts.lossy = cfg->Bool("pdfImportLossy");
  opts.budget = &budget;
  opts.onProgress = [&](int pageNum, int numPages) {
    progress.update(fstring(_("Importing %s (page %d of %d)..."), fileName.c_str(), pageNum + 1, numPages));
    return true;
  };

  std::string err;
  int numPages = -1;
  {
    Document pdfdoc;
    // each page is written as soon as it is rendered, so a long PDF is never in memory all at once
    PdfImport::ImportSaver saver(&pdfdoc, outinfo.path, &budget, cfg->Int("compressLevel", 2));
    opts.saver = &saver;
    numPages = PdfImport::importPdf(&pdfdoc, pdfPath.c_str(), opts, &err);
    if(numPages > 0 && !saver.finish()) {
      numPages = -1;
      err = _("The imported document could not be saved.");
    }
  }
  if(numPages <= 0) {
    // nothing half-written stays behind (the document, which held the file open, is gone)
    if(outinfo.exists())
      removeFile(outinfo.path);
    if(errorOut) *errorOut = err;
    return std::string();
  }
  PLATFORM_LOG("Imported %d page(s) from %s to %s\n", numPages, pdfinfo.c_str(), outinfo.c_str());
  // a command line conversion (--out) must not stop to say so
  if(outDoc.empty()) {
    progress.close();
    reportReducedPages(budget, _("Import PDF"));
  }
  return outinfo.c_str();
}

// the Import menu's "Limit memory" choice in bytes, 0 when off
size_t ScribbleApp::importMemoryLimit()
{
  if(!cfg->Bool("importLimitMemory"))
    return 0;
  return size_t(std::max(64, cfg->Int("importMemoryLimitMB"))) << 20;
}

// Says how many pages an import rendered below the resolution asked for, to stay within the memory limit -
//  otherwise a coarser page would be a mystery
void ScribbleApp::reportReducedPages(const PdfImport::MemoryBudget& budget, const char* title)
{
  if(budget.reducedPages <= 0)
    return;
  messageBox(Info, title, fstring(_("To stay within the memory limit of %d MB, %d pages were imported at a lower"
      " resolution (down to %d DPI).  A higher limit keeps the full resolution."),
      int(budget.limit >> 20), budget.reducedPages, int(budget.lowestDpi + 0.5)));
}

// The browser's Import menu. The system picker answers later, from the event loop (on iOS and Android it
//  cannot do otherwise), by which time the caller has shown the browser again - so the result is handled
//  by browserImportDone() rather than returned.
void ScribbleApp::importFromBrowser(int kind)
{
  if(kind == TagDocList::IMPORT_PDF) {
    if(!PdfImport::isAvailable()) {
      messageBox(Warning, _("Import PDF"), _("This build of Sumi does not include PDF support."));
      return;
    }
    FilePicker::openFile(_("Import PDF"), "pdf", [this](const std::string& filename){
      if(filename.empty())
        return;
      std::string err, docPath = importPdfToDocFile(filename, &err);
      if(docPath.empty())
        messageBox(Warning, _("Import PDF"),
            fstring(_("Error importing %s: %s"), FSPath(filename).fileName().c_str(), err.c_str()));
      browserImportDone(docPath);
    });
  }
  else if(kind == TagDocList::IMPORT_NOTEFUL) {
    // a notebook, or a folder exported as .zip
    FilePicker::openFile(_("Import Noteful"), "noteful zip", [this](const std::string& filename){
      if(!filename.empty())
        browserImportDone(importNoteful(filename));
    });
  }
  else {
    // a Sumi/Write document from anywhere: opening it copies it into the library (doOpenDocument)
    FilePicker::openFile(_("Import Document"), "svgz svg html htm", [this](const std::string& filename){
      if(!filename.empty())
        browserImportDone(filename);
    });
  }
}

// Opens what an import produced as if it had been tapped in the browser (which ends it with EXISTING_DOC);
//  with nothing to open, the browser just relists the library, which now holds the imports.
void ScribbleApp::browserImportDone(const std::string& docPath)
{
  bool browserUp = tagDocList && tagDocList->isVisible();
  if(docPath.empty()) {
    if(browserUp)
      tagDocList->setRoot(tagDocList->root().c_str());
  }
  else if(browserUp) {
    tagDocList->selectedFile = docPath;
    tagDocList->selectedPage = -1;
    tagDocList->finish(TagDocList::EXISTING_DOC);
  }
  else if(maybeSave())
    doOpenDocument(docPath);
}

// imports a Noteful notebook or .zip export into the library; returns the document to open (when exactly
//  one was imported), else ""
std::string ScribbleApp::importNoteful(const std::string& filename)
{
  // into the library; without one, next to what was picked, as PDF import does
  FSPath outDir = libraryManaged() ? FSPath(libraryRoot) : FSPath(filename).parent();
  NotefulImport::ArchiveOptions opts;
  opts.dpi = std::max(72, cfg->Int("pdfImportDPI"));
  opts.folderTags = cfg->Bool("notefulFolderTags", true);
  PdfImport::MemoryBudget budget(importMemoryLimit());
  opts.budget = &budget;
  ProgressBox progress(_("Import Noteful"));
  progress.update(fstring(_("Importing %s..."), FSPath(filename).fileName().c_str()));
  // "Mathe (2 of 5)", then each page of it
  std::string notebookLabel;
  opts.onNotebook = [&](int index, int count, const std::string& path) {
    notebookLabel = count > 1 ? fstring(_("%s (%d of %d)"), FSPath(path).baseName().c_str(), index + 1, count)
        : FSPath(path).baseName();
    progress.update(fstring(_("Importing %s..."), notebookLabel.c_str()));
    return true;
  };
  opts.onProgress = [&](int pageNum, int numPages) {
    progress.update(fstring(_("Importing %s, page %d of %d..."), notebookLabel.c_str(), pageNum + 1, numPages));
    return true;
  };
  NotefulImport::ArchiveResult result;
  std::string err;
  int imported = NotefulImport::importArchive(filename.c_str(), outDir.c_str(), opts, &result, &err);
  // close it before any error box below
  progress.close();
  // the import rewrote the library's tag index; a browser still holding the old one would drop the new
  //  tags on its next save
  if(tagDocList)
    tagDocList->setRoot(tagDocList->root().c_str());

  if(result.failed > 0 || imported <= 0) {
    std::string failures;
    for(const NotefulImport::ArchiveEntry& entry : result.entries) {
      if(entry.docPath.empty())
        failures += fstring("\n%s: %s", FSPath(entry.archivePath).baseName().c_str(), entry.error.c_str());
    }
    messageBox(Warning, _("Import Noteful"), imported > 0 ?
        fstring(_("Imported %d notebooks; these could not be read:%s"), imported, failures.c_str()) :
        fstring(_("Error importing %s: %s"), FSPath(filename).fileName().c_str(),
            failures.empty() ? err.c_str() : failures.c_str()));
  }
  if(imported > 0)
    reportReducedPages(budget, _("Import Noteful"));
  PLATFORM_LOG("Imported %d notebook(s) from %s to %s\n", imported, filename.c_str(), outDir.c_str());
  // one notebook opens; several stay in the browser, which now lists them
  if(imported == 1 && result.entries.size() == 1)
    return result.entries.front().docPath;
  if(imported > 1)
    showNotify(fstring(_("Imported %d notebooks"), imported), 1);
  return "";
}

/// images

void ScribbleApp::insertImage()
{
  // a plain insert must clear any scan left pending by a cancelled picker, or it would be hijacked
  pendingScan = false;
  pickImage();
}

void ScribbleApp::pickImage()
{
#if PLATFORM_ANDROID
  AndroidHelper::getImage();  // will call insertImage(QImage) when user selects image
#elif PLATFORM_IOS
  showImagePicker();
#else
  FilePicker::openFile(_("Insert Image"), "jpg jpeg png", [this](const std::string& filename){
    if(!filename.empty())
      insertImage(filename);
  });
#endif
}

// Where the photo comes from depends on the platform, and pendingScan then diverts it into ScanDialog:
// - Android: insertImage()'s picker, which is already a chooser merging every camera app with the
//   gallery - no Play Services and no CAMERA permission, since the camera app does the capturing
// - iOS: Take Photo / Photo Library when the device has a camera, the library alone when it does not
// - desktop: CameraDialog when Camera::list() finds a camera, with the file picker one button away;
//   no camera means straight to the file picker, as before
void ScribbleApp::scanDocument(bool asPage)
{
  pendingScanAsPage = asPage;
  // a fresh scan, not the continuation of an earlier Add more run (which may have been abandoned in the
  //  picker, leaving these set)
  lastScanPage = -1;
  lastScanRect = Rect();
  captureScan();
}

// gets a photo and hands it to finishScan(), directly on desktop or via the picker's INSERT_IMAGE event on
//  mobile; separate from scanDocument() so Add more can run it again without losing its place
void ScribbleApp::captureScan()
{
  pendingScan = true;
#if PLATFORM_IOS
  showScanImagePicker();
#elif PLATFORM_ANDROID
  pickImage();
#else
  std::vector<CameraInfo> cameras = Camera::list();
  // SUMI_CAMERA=none skips the camera for the file picker, so a scripted run in the agent display never
  //  opens the webcam of the machine it runs on
  const char* cameraMode = getenv("SUMI_CAMERA");
  if(cameraMode && strcmp(cameraMode, "none") == 0)
    cameras.clear();
  if(cameras.empty()) {
    pickImage();
    return;
  }
  int res;
  Image photo(0, 0);
  {
    CameraDialog dialog(std::move(cameras));
    res = execDialog(&dialog);
    if(res == Dialog::ACCEPTED)
      photo = dialog.takePhoto();
  }  // closes the camera before the scan dialog opens, so its light goes off while cropping
  if(res == Dialog::ACCEPTED)
    finishScan(std::move(photo));
  else if(res == CameraDialog::CHOOSE_FILE)
    pickImage();
  else
    pendingScan = false;
#endif
  // if the user cancelled a picker the flag stays set, which is harmless: the next plain insert
  //  clears it and the next scan overwrites it
}

void ScribbleApp::finishScan(Image photo)
{
  bool asPage = pendingScanAsPage;
  pendingScan = false;
  if(photo.isNull())
    return;
  int res;
  Image scan(0, 0);
  {
    ScanDialog dialog(std::move(photo));
    res = execDialog(&dialog);
    if(res == Dialog::ACCEPTED || res == ScanDialog::ADD_MORE)
      scan = dialog.takeResult();
  }  // frees the full size photo before Add more goes and fetches another one
  if(scan.isNull() || scan.width <= 0 || scan.height <= 0)
    return;
  if(asPage)
    insertScanPage(std::move(scan));
  else
    lastScanRect = activeArea()->insertImage(std::move(scan), lastScanRect);
  // Add more: the dialog has already closed, so on desktop this nests one level per extra page, which is
  //  harmless; on mobile it just opens the picker and returns
  if(res == ScanDialog::ADD_MORE)
    captureScan();
}

void ScribbleApp::insertScanPage(Image scan)
{
  // As a page, the scan is the background, exactly as an imported PDF page is: in the rule layer, so it
  //  cannot be selected, dragged or erased while writing on top of it.  See pdfimport.cpp for why
  //  isCustomRuling and the two classes below are what make that survive a page resize and a save/load.
  // A scan carries no physical scale - we know its pixel count, not the size of the paper - so match the
  //  width of the current page and let the height follow the scan's aspect ratio.  That keeps scrolling
  //  and zoom consistent with the rest of the document, which matters more than a guessed DPI would.
  PageProperties refProps = activeArea()->getCurrPage()->getProperties();
  Dim width = refProps.width > 0 ? refProps.width : Dim(scan.width);
  Dim height = width*scan.height/scan.width;
  PageProperties props(width, height, 0, 0, 0, Color::WHITE, Color::BLUE);
  Page* page = new Page(props);
  SvgImage* background = new SvgImage(std::move(scan), Rect::ltwh(0, 0, width, height));
  page->ruleNode->addChild(background);
  page->ruleNode->removeClass("write-std-ruling");
  page->ruleNode->addClass("write-no-dup");
  page->isCustomRuling = true;
  // unlike PDF import, which writes a file and reopens it, this goes into the open document, so it has
  //  to go through ScribbleDoc to get undo and sync - which needs the start/endAction pair, or the
  //  insertion is not recorded as an undoable action
  // the first scan goes after the current page, each Add more scan after the one before it - clamped, in
  //  case a sync peer deleted pages while the picker was open
  ScribbleDoc* doc = activeDoc();
  int where = lastScanPage >= 0 ? std::min(lastScanPage + 1, doc->document->numPages())
      : activeArea()->getCurrPageNum() + 1;
  doc->startAction(where);
  doc->insertPage(page, where);
  doc->endAction();
  lastScanPage = where;
}

void ScribbleApp::insertImage(const std::string& filename)
{
  std::vector<unsigned char> buff;
  if(readFile(&buff, filename.c_str())) {
    Image img = Image::decodeBuffer(&buff[0], buff.size());
    if(!img.isNull()) {
      // the desktop picker hands back a filename and returns here directly, never going through the
      //  INSERT_IMAGE event that the Android and iOS pickers post, so a pending scan is caught here
      if(pendingScan)
        finishScan(std::move(img));
      else
        activeArea()->insertImage(std::move(img));
      return;
    }
  }
  messageBox(Warning, _("Error opening image"), fstring(_("Error reading \"%s\""), filename.c_str()));
}

void ScribbleApp::insertImage(Image image, bool fromintent)
{
  if(!fromintent && (documentList == NULL || !documentList->isVisible()))
    activeArea()->insertImage(std::move(image));
  else
    setClipboardToImage(std::move(image));
}

// callback for image insertion on Android ... since this may be called from a different thread, we push event
//  to ensure only main thread touches document (or clipboard)
void ScribbleApp::insertImageSync(Image image, bool fromintent)
{
  SvgGui::pushUserEvent(scribbleSDLEvent, INSERT_IMAGE, new Image(std::move(image)), fromintent ? (void*)0x1 : NULL);
  //PLATFORM_WakeEventLoop();
}

#if PLATFORM_IOS
// this is called from ioshelper.m
void imagePicked(const void* data, int len, int fromclip)
{
  if(fromclip)  // call directly so clipboard update check on paste works
    ScribbleApp::app->insertImage(Image::decodeBuffer((const unsigned char*)data, len), fromclip);
  else  // insertImageSync to ensure UI updated even though we should be on same thread
    ScribbleApp::insertImageSync(Image::decodeBuffer((const unsigned char*)data, len), fromclip);
}
#endif

// used for linux clipboard
extern "C" { void clipboardFromBuffer(const unsigned char* buff, size_t len, int is_image); }
void clipboardFromBuffer(const unsigned char* buff, size_t len, int is_image)
{
  if(is_image)
    ScribbleApp::app->insertImage(Image::decodeBuffer(buff, len), true);  // fromintent = true to set clipboard
  else
    ScribbleApp::app->loadClipboardText((const char*)buff, len);
}

/// PDF export ///

void ScribbleApp::writePDF(std::ostream& strm, const std::vector<int>& pages)
{
  Document* doc = activeDoc()->document;
  std::vector<int> pagenums;
  for(int pagenum : pages) {
    if(pagenum >= 0 && pagenum < doc->numPages())
      pagenums.push_back(pagenum);
  }
  if(pages.empty()) {
    for(int ii = 0; ii < doc->numPages(); ++ii)
      pagenums.push_back(ii);
  }
  PdfWriter pdf(pagenums.size());
  pdf.anyHref = true;  // hack to work around our hack for links
  //pdf.compressionLevel = 0;  // for debugging
  Dim ptsPerDim = 72.0/150;
  // a link names its target by the document's page number, which is not the PDF's when only some pages
  //  are written; a link to a page left out goes nowhere
  pdf.resolveLink = [doc, pagenums](const char* href, int* pgnum) -> SvgNode* {
    int docpage = doc->numPages() - 1;
    SvgNode* target = doc->findNamedNode(href, &docpage);
    auto it = std::find(pagenums.begin(), pagenums.end(), docpage);
    if(!target || it == pagenums.end())
      return NULL;
    *pgnum = int(it - pagenums.begin());
    return target;
  };
  // what Element::applyStyle() does when drawing; PdfWriter does not run it
  pdf.defaultCompOp = [](const SvgNode* node) {
    return Element::isLegacyMarker(node) ? int(Painter::CompOp_Multiply) : -1;
  };
  for(int pagenum : pagenums) {
    Page* page = doc->pages[pagenum];
    page->ensureLoaded();
    pdf.newPage(page->width(), page->height(), ptsPerDim);
    pdf.darkBackdrop = page->color().luma() < 128;  // as on screen (Page::drawsDark()), minus night mode
    pdf.drawNode(page->svgDoc.get());
  }
  pdf.write(strm);
}

bool ScribbleApp::writePDF(const std::string& filename, const std::vector<int>& pages)
{
  std::ofstream f(PLATFORM_STR(filename.c_str()), std::ios::out | std::ios::binary);
  if(!f)
    return false;
  writePDF(f, pages);
  f.close();
  return true;
}

#if PLATFORM_EMSCRIPTEN
void ScribbleApp::exportPDF()
{
  std::stringstream strm;
  activeDoc()->checkAndClearErrors(true);  // clear errors so document can be saved
  writePDF(strm);
  std::string str = strm.str();
  jsSaveFile("Untitled.pdf", str.data(), str.size());
}
#else
void ScribbleApp::exportPDF()
{
  std::string name = activeDoc()->fileName()[0] ? FSPath(activeDoc()->fileName()).baseName() : std::string("Untitled");
  FilePicker::saveFile(_("Export PDF"), name + ".pdf", "pdf", [this](const std::string& filename){
    if(writePDF(filename))
      return true;
    messageBox(Error, _("Export PDF"), _("Error saving document. Please try a different folder."));
    return false;
  });
}
#endif

/// page management exports (docs/agent/page-management.md) ///

std::string ScribbleApp::pagesExportName(const std::vector<int>& pages)
{
  std::string name = activeDoc()->fileName()[0] ? FSPath(activeDoc()->fileName()).baseName() : std::string("Untitled");
  return pages.size() == 1 ? fstring("%s-p%d", name.c_str(), pages.front() + 1) : name + "-pages";
}

#if PLATFORM_EMSCRIPTEN
// the browser has no file system the user can reach: every export is a download
static void downloadFile(const std::string& path)
{
  std::string contents = readFile(path.c_str());
  jsSaveFile(FSPath(path).fileName().c_str(), contents.data(), contents.size());
}
#endif

void ScribbleApp::exportPagesPDF(const std::vector<int>& pages)
{
  if(pages.empty())
    return;
#if PLATFORM_EMSCRIPTEN
  std::string path = FSPath(tempPath).childPath(pagesExportName(pages) + ".pdf");
  if(writePDF(path, pages))
    downloadFile(path);
#else
  FilePicker::saveFile(_("Export PDF"), pagesExportName(pages) + ".pdf", "pdf", [this, pages](const std::string& filename){
    if(writePDF(filename, pages))
      return true;
    messageBox(Error, _("Export PDF"), _("Error saving document. Please try a different folder."));
    return false;
  });
#endif
}

// A new document holding just these pages, for the share sheet.  The desktop has no share sheet, so
//  there it is saved through the save dialog instead, like Export PDF.
void ScribbleApp::sharePagesDocument(const std::vector<int>& pages)
{
  if(pages.empty())
    return;
  std::string name = pagesExportName(pages) + ".svgz";
#if PLATFORM_MOBILE || PLATFORM_EMSCRIPTEN
  std::string path = FSPath(tempPath).childPath(name);
  if(!activeDoc()->savePagesCopy(pages, path.c_str())) {
    messageBox(Error, _("Share Pages"), _("Error saving document."));
    return;
  }
#if PLATFORM_EMSCRIPTEN
  downloadFile(path);
#else
  sendFile(_("See attached file"), path);
#endif
#else
  FilePicker::saveFile(_("Share Pages"), name, "svgz", [this, pages](const std::string& filename){
    // the format follows the extension (Document::save), so a name typed without one still gets svgz
    std::string path = FSPath(filename).extension() == "svgz" ? filename : filename + ".svgz";
    if(activeDoc()->savePagesCopy(pages, path.c_str()))
      return true;
    messageBox(Error, _("Share Pages"), _("Error saving document. Please try a different folder."));
    return false;
  });
#endif
}

Image ScribbleApp::renderPageImage(Page* page)
{
  page->ensureLoaded();
  Rect dirty = page->rect();
  Dim scale = gui->paintScale;
  Image img(page->width()*scale, page->height()*scale, Image::PNG);
  Painter imgpaint(Painter::PAINT_SW | Painter::SRGB_AWARE, &img);
  imgpaint.beginFrame();
  imgpaint.scale(scale);
  imgpaint.setsRGBAdjAlpha(true);
  page->draw(&imgpaint, dirty, cfg->Bool("sendRuleLines"));
  imgpaint.endFrame();
  return img;
}

static bool writePng(const Image& img, const std::string& path)
{
  std::ofstream pngstrm(PLATFORM_STR(path.c_str()), std::ios::binary);
  if(!pngstrm)
    return false;
  auto pngenc = img.encodePNG();
  pngstrm.write((char*)pngenc.data(), pngenc.size());
  return bool(pngstrm);
}

// One image per page.  The desktop save dialog picks one file, so several pages are written beside it as
//  name-N.png, N the page number; on mobile they all go to one share sheet.
void ScribbleApp::exportPagesPNG(const std::vector<int>& pages)
{
  if(pages.empty())
    return;
  // the files for the chosen path: the path itself for one page, name-N.png beside it for several
  auto writeAll = [this, pages](const std::string& chosen, std::vector<std::string>* written){
    Document* doc = activeDoc()->document;
    std::string base = FSPath(chosen).extension() == "png" ? FSPath(chosen).basePath() : chosen;
    for(int pagenum : pages) {
      if(pagenum < 0 || pagenum >= doc->numPages())
        continue;
      std::string path = pages.size() == 1 ? base + ".png" : fstring("%s-%d.png", base.c_str(), pagenum + 1);
      if(!writePng(renderPageImage(doc->pages[pagenum]), path))
        return false;
      if(written)
        written->push_back(path);
    }
    return true;
  };
#if PLATFORM_MOBILE || PLATFORM_EMSCRIPTEN
  std::vector<std::string> written;
  std::string name = activeDoc()->fileName()[0] ? FSPath(activeDoc()->fileName()).baseName() : std::string("Untitled");
  if(!writeAll(FSPath(tempPath).childPath(name + ".png"), &written)) {
    messageBox(Error, _("Export PNG"), _("Error saving image."));
    return;
  }
#if PLATFORM_EMSCRIPTEN
  for(const std::string& path : written)
    downloadFile(path);
#else
  sendFiles(_("See attached image"), written);
#endif
#else
  std::string name = pages.size() == 1 ? pagesExportName(pages) : (activeDoc()->fileName()[0]
      ? FSPath(activeDoc()->fileName()).baseName() : std::string("Untitled"));
  FilePicker::saveFile(_("Export PNG"), name + ".png", "png", [this, writeAll](const std::string& filename){
    if(writeAll(filename, NULL))
      return true;
    messageBox(Error, _("Export PNG"), _("Error saving image. Please try a different folder."));
    return false;
  });
#endif
}

/// dialogs ///

void ScribbleApp::createLink()
{
  //LinkDialog dialog(activeDoc());
  //execDialog(&dialog);  // blocking
  asyncDialog(new LinkDialog(activeDoc()));
}

void ScribbleApp::showPageSetup()
{
  //RulingDialog rulingDialog(activeDoc());
  //execDialog(&rulingDialog);  // blocking
  asyncDialog(new RulingDialog(activeDoc()));
}

void ScribbleApp::showThemePicker()
{
  // works on an untitled document exactly as on a saved one - the per-document config exists either way
  asyncDialog(new ThemeDialog(activeDoc()));
}

void ScribbleApp::openPreferences()
{
  if(disableConfigSave)
    messageBox(Warning, _("Sumi"), _("Preferences will not be saved because some were set from command line."));
  //showSelToolbar(Point(NaN, NaN)); -- no longer possible to open prefs w/o sel toolbar being closed
  //ConfigDialog dialog(cfg);
  //int res = execDialog(&dialog);  // blocking
  std::string prevLibraryPath = cfg->String("libraryPath");
  asyncDialog(new ConfigDialog(cfg), [this, prevLibraryPath](int res) {
    if(res == Dialog::ACCEPTED) {
      reloadConfig();  // note we still save config file if res == 0 (rejected)
      activeDoc()->updateGhostPage();  // the default page size may have changed
      // a new library location moves the documents there; on refusal or failure the old one stands
      std::string newLibraryPath = StringRef(cfg->String("libraryPath")).trimmed().toString();
      if(libraryManaged() && newLibraryPath != prevLibraryPath) {
        cfg->set("libraryPath", prevLibraryPath.c_str());
        if(!newLibraryPath.empty())
          relocateLibrary(newLibraryPath);
      }
    }
    else if(res == -1)
      return;
    writeConfigFile();
  });
}

// pick up config changes and persist them; the Preferences dialog defers its writes until OK, but the
//  tools' settings popups write each change as it is made, so they call this straight away
void ScribbleApp::applyConfigChanges()
{
  reloadConfig();
  writeConfigFile();
}

void ScribbleApp::reloadConfig()
{
  loadConfig();
  for(ScribbleDoc* doc : scribbleDocs)
    doc->loadConfig(true);
  bookmarkArea->loadConfig(cfg);
  // destroy doc list so that it will be recreated with new settings
  delete documentList;
  documentList = NULL;
}

void ScribbleApp::writeConfigFile()
{
  // write config file immediately to better support multiple instances
  // would be nice if could also reread before opening dialog!
  if(!disableConfigSave && !cfg->saveConfigFile(cfgFile.c_str(), true)) {
    messageBox(Warning, _("Error"), fstring(_("Error saving preferences to \"%s\""), cfgFile.c_str()));
  }
}

void ScribbleApp::resetDocPrefs()
{
  activeDoc()->resetDocPrefs();
}

// version: XXYYZZ where public version number is XX.YY.ZZ; no good reason for this to be in makefile
#define SCRIBBLE_VERSION_SERIAL 30000

void ScribbleApp::about()
{
  static const int ver = SCRIBBLE_VERSION_SERIAL;
  static const int maint = ver % 100;
  static const int minor = (ver/100) % 100;
  static const int major = (ver/10000) % 100;
  // Sumi is a modified version of Write; the AGPL (section 5) requires saying so and keeping the credit
  messageBox(Info, _("About Sumi"),
      //fstring("Sumi v%d.%d.%d\nBuild ID: ", major, minor, maint) + PPVALUE_TO_STRING(SCRIBBLE_REV_NUMBER)
      fstring("Sumi %d\nBuild ID: ", major) + PPVALUE_TO_STRING(SCRIBBLE_REV_NUMBER)
      + "; " + __DATE__ + (IS_DEBUG ? " DEBUG" : "") +
      "\nBased on Write by Stylus Labs\nhttp://www.styluslabs.com"
      "\nFree software under the GNU Affero General Public License v3"
      "\n\nSumi is a word processor for handwriting."
#if PLATFORM_IOS
      "\nPrivacy: Sumi does not collect any personal data."
#else
      "\nAvailable for iOS, Android, Windows, Mac, and Linux."
#endif
  );
}

/// send/share ///

void ScribbleApp::sendFile(const std::string& body, const std::string& attachfile)
{
#if PLATFORM_ANDROID
  FSPath fileinfo(attachfile);
  std::string ext = fileinfo.extension();
  std::string mimetype;
  if(ext == "html" || ext == "htm")
    mimetype = "text/html";
  else if(ext == "svg" || ext == "svgz")
    mimetype = "image/svg+xml";
  else if(ext == "png")
    mimetype = "image/png";
  else if(ext == "pdf")
    mimetype = "application/pdf";
  AndroidHelper::sendFile(attachfile.c_str(), mimetype.c_str(), FSPath(activeDoc()->fileName()).baseName().c_str());
#elif PLATFORM_IOS
  iosSendFile(attachfile.c_str());
#elif IS_DEBUG
  PLATFORM_LOG("Sharing %s\n", attachfile.c_str());
#endif
}

void ScribbleApp::sendFiles(const std::string& body, const std::vector<std::string>& attachfiles)
{
  if(attachfiles.size() == 1) {
    sendFile(body, attachfiles.front());
    return;
  }
#if PLATFORM_ANDROID
  AndroidHelper::sendFiles(attachfiles, "image/png", FSPath(activeDoc()->fileName()).baseName().c_str());
#elif PLATFORM_IOS
  std::vector<const char*> paths;
  for(const std::string& file : attachfiles)
    paths.push_back(file.c_str());
  iosSendFiles(paths.data(), int(paths.size()));
#else
  for(const std::string& file : attachfiles)
    sendFile(body, file);
#endif
}

void ScribbleApp::sendPageImage()
{
  Image img = renderPageImage(activeArea()->getCurrPage());

  std::string basename = activeDoc()->fileName()[0] ?
      FSPath(activeDoc()->fileName()).baseName() : std::string("untitled");
  std::string pngfile = FSPath(tempPath).childPath(basename + ".png");
  std::ofstream pngstrm(PLATFORM_STR(pngfile.c_str()), std::ios::binary);
  auto pngenc = img.encodePNG();
  pngstrm.write((char*)pngenc.data(), pngenc.size());
  pngstrm.close();
  sendFile(_("See attached image"), pngfile);
}

void ScribbleApp::sendPDF()
{
  std::string basename = activeDoc()->fileName()[0] ?
      FSPath(activeDoc()->fileName()).baseName() : std::string("untitled");
  std::string pdffile = FSPath(tempPath).childPath(basename + ".pdf");
  if(writePDF(pdffile))
    sendFile(_("See attached PDF"), pdffile);
}

void ScribbleApp::sendDocument()
{
  if(maybeSave())
    sendFile(_("See attached file"), activeDoc()->fileName());
}

/// Update check

void ScribbleApp::updateCheck(bool userreq)
{
#if ENABLE_UPDATE
  if(updateSocket != -1)  // this may happen in syncClearUndone()
    return;  //SDL_WaitThread(updateThread, NULL);
  userUpdateReq = userreq;
  std::thread updateThread(updateThreadFn, (void*)this);  //updateThread = SDL_CreateThread(updateThreadFn, "Write_updateThread", (void*)this);
  updateThread.detach();  //SDL_DetachThread(updateThread);
#endif
}

#if ENABLE_UPDATE

int ScribbleApp::updateThreadFn(void* _self)
{
  ScribbleApp* self = static_cast<ScribbleApp*>(_self);
  constexpr int RECV_BUFF_LEN = 1<<20;
  char* recvBuff = new char[RECV_BUFF_LEN];
  char* recvPtr = recvBuff;
  self->updateSocket = unet_socket(UNET_TCP, UNET_CONNECT, UNET_NOBLOCK, "www.styluslabs.com", "80");
  if(self->updateSocket != -1) {
    if(unet_select(-1, self->updateSocket, 4) == UNET_RDY_WR) {
      std::string req = fstring("GET /write/versions.xml?force=%d&sc=%d HTTP/1.1\r\nHost: www.styluslabs.com\r\n"
          "Connection: close\r\nUser-Agent: %s\r\n\r\n",
          self->userUpdateReq ? 1 : 0, cfg->Int("strokeCounter"), self->httpUserAgent.c_str());
      unet_send(self->updateSocket, req.data(), req.size());
      while(unet_select(self->updateSocket, -1, 4) == UNET_RDY_RD) {
        int n = unet_recv(self->updateSocket, recvPtr, RECV_BUFF_LEN - (recvPtr - recvBuff) - 1);
        if(n <= 0)
          break;
        recvPtr += n;
      }
    }
    unet_close(self->updateSocket);
    self->updateSocket = -1;
  }

  *recvPtr = '\0';
  SvgGui::pushUserEvent(scribbleSDLEvent, UPDATE_CHECK, (void*)recvBuff);
  return 0;
}

void ScribbleApp::updateNotify(const char* msg, int level)
{
  if(userUpdateReq)
    messageBox(level == 0 ? Info : Warning, _("Update Check"), msg);
  //userUpdateReq = false;
#ifdef SCRIBBLE_TEST
  SCRIBBLE_LOG("Update Check: %s\n", msg);
#endif
}

void ScribbleApp::updateInfoReceived(char* updateData)
{
  pugi::xml_document updateInfo;
  pugi::xml_node node;
  StringRef updateStr(updateData);
  int contentpos = updateStr.find("\r\n\r\n");
  if(!updateData[0] || contentpos < 0)
    updateNotify(_("Error connecting to update server."), 1);
  else if(!updateInfo.load(updateData + contentpos + 4)
      || !(node = updateInfo.child("rss").child("channel").child("item")))
    updateNotify(_("Error parsing update information from server."), 1);
  else {
    cfg->set("strokeCounter", 0);
    cfg->set("lastUpdateCheck", int(mSecSinceEpoch()/1000));
    // see if a new version is available
    if(node.child("sl:version").text().as_int(0) <= SCRIBBLE_VERSION_SERIAL)
      updateNotify(_("You have the latest version of Sumi."), 0);
    else {
      // no automatic update for now - just prompt to go to download page
      auto choice = messageBox(Question, _("Sumi Update"),
          _("A new version of Sumi is available.  Would you like to open the download page?"), {_("Yes"), _("No")});
      if(choice == _("Yes"))
        openURL("http://www.styluslabs.com/download/");
    }
  }
  delete[] updateData;

#ifdef Q_OS_WIN
    QNetworkReply* dlreply;
    // download the update ... I guess for now we'll just write out to file when everything has been received
    // if we are too old, must download full install
    pugi::xml_node enclosure = node.find_child_by_attribute("enclosure", "class", "win_update");
    if(enclosure.attribute("sl:minversion").as_int(0) > SCRIBBLE_VERSION_SERIAL)
      dlreply = qNetMgr->get(QNetworkRequest(QUrl(
          node.find_child_by_attribute("enclosure", "class", "win_full").attribute("url").as_string())));
    else
      dlreply = qNetMgr->get(QNetworkRequest(QUrl(enclosure.attribute("url").as_string())));
    connect(dlreply, SIGNAL(finished()), this, SLOT(updateDownloaded()));
#endif
}
#endif  // ENABLE_UPDATE

/// Shared whiteboard
#include "syncdialog.h"

#define MD5_IMPLEMENTATION
#include "ulib/md5.h"

struct Url {
  std::string user;
  std::string pass;
  std::string host;
  std::string path;
  std::string query;
};

// SWB url string: [user[:pass]@][server[:port]/]whiteboard_id[?query params]
// we allow SWB login in HTTP credential format, but we don't actually send as HTTP credentials (insecure)
static Url parseWhiteboard(std::string sharename)
{
  Url url;
  if(sharename.compare(0, 6, "swb://") == 0)
    sharename = sharename.substr(6);
  size_t atsym = sharename.find('@');
  if(atsym != std::string::npos) {
    size_t colon = sharename.find(':');
    if(colon != std::string::npos && colon < atsym) {
      url.pass = sharename.substr(colon + 1, atsym - colon);
      url.user = sharename.substr(0, colon);
    }
    else
      url.user = sharename.substr(0, atsym);
    sharename.substr(atsym+1).swap(sharename);
  }
  size_t slash = sharename.find('/');
  if(slash != std::string::npos) {
    url.host = sharename.substr(0, slash);
    sharename.substr(slash+1).swap(sharename);
  }
  else
    url.host = ScribbleApp::cfg->String("syncServer");
  if(url.host.empty())
    url.host = "www.styluslabs.com";
  size_t qmark = sharename.find('?');
  if(qmark != std::string::npos) {
    url.query = sharename.substr(qmark+1);
    url.path = sharename.substr(0, qmark);
  }
  else
    url.path = sharename;
  return url;
}

void ScribbleApp::showSyncInfo()
{
  SyncInfoDialog dialog(activeDoc()->scribbleSync);
  execDialog(&dialog);
}

// check menu item + get sharing URL?
void ScribbleApp::shareDocument()
{
  if(activeDoc()->scribbleSync) {
    auto choice = messageBox(Question, _("Disconnect Whiteboard"),
        _("Disconnect from current whiteboard?"), {_("OK"), _("Cancel")});
    if(choice != _("OK"))
      return;
    delete activeDoc()->scribbleSync;
    activeDoc()->scribbleSync = NULL;
  }
  if(!maybeSave())
    return;
  std::string titlein = FSPath(activeDoc()->fileName()).baseName();
  // show link to styluslabs.com/share if no server or user saved
  bool showLink = !cfg->String("syncServer")[0] && !cfg->String("syncUser", "")[0];
  SyncCreateDialog dialog(toLower(randomStr(6)).c_str(), titlein.c_str(), showLink);
  for(;;) {
    if(execDialog(&dialog) != Dialog::ACCEPTED)
      return;
    Url url = parseWhiteboard(trimStr(dialog.urlEdit->text()));
    if(url.path != urlEncode(url.path.c_str())) {
      dialog.setMessage(_("ID cannot contain special characters"));
      continue;
    }
    std::string title = trimStr(dialog.titleEdit->text());
    std::string query = "&title=" + urlEncode(title.c_str());
    if(dialog.lectureMode->isChecked())
      query += "&rxonly=1";
    if(!url.query.empty())
      query.append("&").append(url.query);
    if(!syncSignIn(url))
      return;
    if(doSharedDoc(url.host, syncAPICall(url, "/v1/createswb?name=" + url.path + query), true))
      return;
    dialog.setMessage(_("Error creating whiteboard"));  // try again
  }
}

bool ScribbleApp::openSharedDoc()
{
  if(!maybeSave())
    return false;
  bool showLink = !cfg->String("syncServer")[0] && !cfg->String("syncUser", "")[0];
  SyncOpenDialog dialog(showLink);
  for(;;) {
    if(execDialog(&dialog) != Dialog::ACCEPTED)
      return false;
    Url url = parseWhiteboard(dialog.urlEdit->text());
    if(!syncSignIn(url))
      return false;
    if(doSharedDoc(url.host, syncAPICall(url, "/v1/openswb?name=" + url.path), false))
      return true;
    dialog.setMessage(_("Error connecting to whiteboard"));  // try again
  }
}

bool ScribbleApp::openSharedDoc(std::string sharename)
{
  Url url = parseWhiteboard(sharename);
  return syncSignIn(url) && doSharedDoc(url.host, syncAPICall(url, "/v1/openswb?name=" + url.path), false);
}

bool ScribbleApp::doSharedDoc(std::string host, std::string reply, bool master)
{
  size_t contentpos = reply.find("\r\n\r\n");
  if(!StringRef(reply).startsWith("HTTP/1.1 200") || contentpos == std::string::npos)
    return false;
  contentpos += 4;
  pugi::xml_document doc;
  doc.load_buffer(reply.data() + contentpos, reply.size() - contentpos);
  pugi::xml_node swb = doc.child("swb");
  const char* name = swb.attribute("name").as_string(NULL);
  const char* token = swb.attribute("token").as_string(NULL);
  if(!name || !token)
    return false;
  win->actionSendImmed->setChecked(true);  // reflect state of ScribbleSync
  showNotify(_("Connecting..."), 0);
  activeDoc()->openSharedDoc(host.c_str(), swb, master);
  if(!master) {
    std::string title = swb.attribute("title").as_string();
    if(title.empty())
      title = name;
#if PLATFORM_IOS
    onLoadFile("");
    doSaveAs();
#else
    FSPath fileinfo = FSPath(cfg->String("currFolder", ".")).child(title + "." + fileExt);
#if PLATFORM_MOBILE
    if(fileinfo.exists())
      fileinfo = FSPath(createRecoveryName(fileinfo.c_str(), ""));
#endif
    if(fileinfo.exists() || !activeDoc()->saveDocument(fileinfo.c_str(), Document::SAVE_FORCE))
      doSaveAs();
    else
      onLoadFile(fileinfo.c_str());
#endif
  }
  return true;
}

bool ScribbleApp::syncSignIn(const Url& baseurl)
{
  char md5Temp[2*MD5_DIGEST_SIZE+1];
  bool saveuser = baseurl.user.empty() && baseurl.pass.empty() && baseurl.host == cfg->String("syncServer");
  std::string user = saveuser ? std::string(cfg->String("syncUser", "")) : baseurl.user;
  std::string pw = saveuser ? cfg->String("syncPass", "") : "";
  if(!baseurl.pass.empty())
    pw = MD5hex(("styluslabs" + baseurl.pass).c_str(), 0, md5Temp);
  std::string msg;
  syncSession.clear();
  for(;;) {
    bool savepw = false;
    if(user.empty() || pw.empty()) {  // we previously allowed empty username, so check here
      // "See styluslabs.com/faq"; //"<a href='http://www.styluslabs.com/account/'>Create or edit account</a>";
      std::string accountlink = _("Login to ") + baseurl.host;
      SyncLoginDialog dialog(user.c_str(), saveuser, msg.empty() ? accountlink.c_str() : msg.c_str());
      if(execDialog(&dialog) != Dialog::ACCEPTED)
        return false;
      // remove leading and trailing space, which can easily be added accidentally or automatically on Android
      user = trimStr(dialog.userEdit->text());
      if(saveuser)
        cfg->set("syncUser", user.c_str());
      std::string saltedpw = "styluslabs" + trimStr(dialog.passEdit->text());
      pw = MD5hex(saltedpw.c_str(), 0, md5Temp);
      savepw = saveuser && dialog.savePassword->isChecked();
    }

    // we'll let QNetworkAccessManager handle the session cookie returned by /auth
    std::string ts = fstring("%lld", mSecSinceEpoch());
    std::string sig = MD5hex((pw + ts).c_str(), 0, md5Temp);
    std::string replystr = syncAPICall(baseurl, "/v1/auth?user=" + user + "&timestamp=" + ts + "&signature=" + sig);
    StringRef reply(replystr);
    if(reply.startsWith("HTTP/1.1 200")) {
      if(savepw && saveuser)
        cfg->set("syncPass", pw.data());
      const char* setCookieStr = "Set-Cookie: session=";
      int c0 = reply.find(setCookieStr);
      if(c0 > 0) {
        c0 += strlen(setCookieStr);
        int c1 = reply.findFirstOf("\r;", c0);
        syncSession = reply.substr(c0, c1 - c0).toString();
      }
      return true;
    }
    if(reply.isEmpty())
      msg = _("Could not connect to ") + baseurl.host;
    else
      msg = StringRef(reply).startsWith("HTTP/1.1 401") ?
          std::string(_("Incorrect username or password.")) : (_("HTTP Error: ") + replystr.substr(9, 3));
    // try again
    if(saveuser)
      cfg->set("syncPass", "");
    pw.clear();
  }
}

// We want to redisplay login, create SWB, open SWB dialogs on failure because mistyped password or doc name
//  is expected to be fairly common, we have to show some kind of error info anyway and for the user to
//  manually reopen dialogs they have to renavigate to submenu of overflow menu (so 3 clicks)
// - but how to do it?
// - recursively call with retry = true? ... but we don't want to repeat save or sign in!
// - use a loop inside the fn? ... do we recreate the dialog inside the loop; if not, how do we add the
//  the error message when retrying?; if so, I suppose we need bool retry; variable

/*QNetworkReply* MainWindow::asyncAPICall(const QUrl& baseurl, const std::string& route, const QObject* obj, const char* method)
{
  if(!syncAPIMgr)
    syncAPIMgr = new QNetworkAccessManager(this);

  QUrl requrl(std::string("http://") + cfg->String("syncServer") + route);
  requrl.setPort(baseurl.port(7000));
  if(!baseurl.host().isEmpty())
    requrl.setHost(baseurl.host());
  QNetworkRequest request;
  request.setUrl(requrl);
  request.setRawHeader("User-Agent", httpUserAgent);
  QNetworkReply* reply = syncAPIMgr->get(request);
  if(obj)
    QObject::connect(reply, SIGNAL(finished()), obj, method);
  return reply;
}*/

// For sign-in and create/open SWB, I think blocking API call is actually preferrable (and easier)
std::string ScribbleApp::syncAPICall(const Url& baseurl, const std::string& route)
{
  // TODO: show "Please Wait..." message box
  constexpr int RECV_BUFF_LEN = 4096;
  char recvBuff[RECV_BUFF_LEN];
  std::string reply;
  int sock = unet_socket(UNET_TCP, UNET_CONNECT, UNET_NOBLOCK, baseurl.host.c_str(), "7000");
  if(sock != -1) {
    if(unet_select(-1, sock, 4) > 0) {
      std::string req = fstring("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nUser Agent: %s\r\n",
          route.c_str(), baseurl.host.c_str(), httpUserAgent.c_str());
      if(!syncSession.empty())
        req.append("Cookie: session=").append(syncSession).append("\r\n");
      req.append("\r\n");
      unet_send(sock, req.data(), req.size());
      while(unet_select(sock, -1, 4) > 0) {
        int n = unet_recv(sock, recvBuff, RECV_BUFF_LEN);
        if(n <= 0)
          break;
        reply.append(recvBuff, n);
      }
    }
    unet_shutdown(sock, UNET_SHUT_RDWR);
    unet_close(sock);
  }
  return reply;
}

// toggle sending edits
void ScribbleApp::syncSendImmed()
{
  win->actionSendImmed->setChecked(!win->actionSendImmed->isChecked());
  activeDoc()->scribbleSync->syncImmed = win->actionSendImmed->isChecked();
  if(activeDoc()->scribbleSync->syncImmed)
    activeDoc()->scribbleSync->sendHist(true);
}

// view sync button for SWB
void ScribbleApp::syncView()
{
  if(!activeDoc()->scribbleSync)
    return;
  activeDoc()->scribbleSync->syncViewBox = win->actionViewSync->isChecked() ?
      (win->actionViewSyncMaster->isChecked() ? ScribbleSync::SYNCVIEW_MASTER : ScribbleSync::SYNCVIEW_SLAVE)
      : ScribbleSync::SYNCVIEW_OFF;
}

// level: 0 <= info, 1 = warn, 2 = error; level < 0 concats to previous info message if still visible
void ScribbleApp::showNotify(const std::string& msg, int level)
{
  static const char* bg[] = {"#aaa", "#aa0", "#a00"};

  if(level < cfg->Int("syncMsgLevel")) return;
  if(!notifyBar) {
    notifyBar = createToolbar();
    notifyBar->node->removeClass("toolbar");  // otherwise CSS fill overrides ours
    notifyText = new TextBox(createTextNode(""));
    notifyText->setMargins(0, 0, 0, 16);
    Button* closeNotify = createToolbutton(SvgGui::useFile("icons/ic_menu_cancel.svg"), "");
    closeNotify->onClicked = [this](){ dismissNotify(); };

    notifyBar->addWidget(notifyText);
    notifyBar->addWidget(createStretch());
    notifyBar->addWidget(closeNotify);
    win->selectFirst("#notify-toolbar-container")->addWidget(notifyBar);
  }
  // pass level < 0 to concat to last info message; don't let message grow to more then 1/4 of window height
  if(notifyBar->isVisible() && level < 0 && notifyBar->node->bounds().height() < win->winBounds().height()/4)
    notifyText->setText((notifyText->text() + "\n" + msg).c_str());
  else
    notifyText->setText(msg.c_str());

  notifyBar->node->setAttribute("fill", bg[std::max(0, std::min(level, 2))]);
  notifyBar->setVisible(true);
  // we might want to see notifications longer when testing
#ifndef SCRIBBLE_TEST
  notifyTimer = gui->setTimer(5000, win, notifyTimer, [this]() {
    dismissNotify();
    return 0;
  });
#endif
}

void ScribbleApp::dismissNotify()
{
  if(notifyBar)
    notifyBar->setVisible(false);
}

// update check

#ifdef Q_OS_WIN
void MainWindow::updateDownloaded()
{
  QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
  if(reply->error() == QNetworkReply::NoError) {
    // assume this is a download request
    // is there any reason to let filename be variable?
    std::string updaterpath = QApplication::applicationDirPath() + "/write_update.exe";
    QFile file(updaterpath);
    if(!file.open(QIODevice::WriteOnly)) {
      updateNotify("Error saving update installer to " + updaterpath
            + ". You may need to run Sumi as an administrator to perform update.");
      goto cleanup;
    }
    file.write(reply->readAll());
    file.close();
    // don't bug user until next week if they cancel the update
    cfg->set("lastUpdateCheck", int(mSecSinceEpoch()/1000));
    // ask user if they want to install update
    pugi::xml_node node = updateInfo.child("rss").child("channel").child("item");
    QMessageBox msgbox(QMessageBox::Question, tr("Update Available"),
        tr("A new version of Sumi is available, would you like to update now?\n\n")
        + node.child_value("description"));
    QPushButton* updatebtn = msgbox.addButton(tr("Update Now"), QMessageBox::AcceptRole);
    msgbox.addButton(tr("Skip Update"), QMessageBox::RejectRole);
    msgbox.setDefaultButton(updatebtn);
    msgbox.exec();
    // quit() doesn't appear to generate call to closeEvent
    if(msgbox.clickedButton() != updatebtn || !maybeSave()) {
      QMessageBox::information(this, "Update Skipped", "To update later, select \"Check for Update\" from the "
          "Help menu.  Automatic update check can be disabled in Preferences.");
      goto cleanup;
    }
    // launch the updater and exit
    QProcess::startDetached(updaterpath, std::stringList());
    QApplication::quit();
  }
  else
    updateNotify("Error downloading update from " + reply->url().path());
cleanup:
  reply->deleteLater();
}
#endif

#ifdef SCRIBBLE_TEST
// an alternative to this is TESTSOURCES= in Makefile.common
#include "scribbletest/scribbletest.cpp"

void ScribbleApp::runTestUI(std::string runtype)
{
  if(!cfg->Int("glRender")) {
    messageBox(Error, "Cannot run tests", "Tests require GL renderer.");
    return;
  }
  messageBox(Info, "Test Results - " + runtype, runTest(runtype));
}

std::string ScribbleApp::runTest(std::string runtype)
{
  if(runtype == runType) { ScribbleTest::exitAfterTest = true; }
  if(runtype == "test") {
    ScribbleTest test(SCRIBBLE_TEST_PATH);
    test.runAll();
    return test.resultStr;
  }
  else if(runtype == "synctest") {
    ScribbleTest test(SCRIBBLE_TEST_PATH);
    // set server for synctest
    test.scribbleConfig->set("syncServer", cfg->String("syncServer"));
    test.runAll(true);
    return test.resultStr;
  }
  else if(runtype == "perftest") {
    ScribbleTest test(activeDoc(), bookmarkArea, scribbleMode);
    test.performanceTest();
    return test.resultStr;
  }
  else if(runtype == "inputtest") {
    // see 83a76eea88eb for TouchInputFilter::notifyTouchEvent test
    ScribbleTest test(activeDoc(), bookmarkArea, scribbleMode);
    test.inputTest();
    return test.resultStr;
  }
  return runtype + " is not a valid test mode.";
}

#endif

Color ScribbleApp::displayColor(Color c)
{
  ScribbleArea* area = app ? app->activeArea() : NULL;
  const ColorMap* colorMap = area ? area->nightColorMap() : NULL;
  return colorMap ? colorMap->map(c) : c;
}
