#pragma once

#include "pugixml.hpp"
#include "ulib/painter.h"
#include "ugui/widgets.h"
#include "basics.h"
#include "application.h"
#include "scribblepen.h"
#include "tablist.h"
#if PLATFORM_ANDROID
#include "android/androidhelper.h"
#elif PLATFORM_IOS
#include "ios/ioshelper.h"

// move this somewhere else
struct UIDocStream : public MemStream
{
  std::string url;
  void* uiDocument;

  UIDocStream(void* src, size_t len, size_t reserve, const char* _url, void* uidoc)  //, size_t reserve = 0)
    : url(_url), uiDocument(uidoc) { buffer = (char*)src; buffsize = len; capacity = reserve; }  //MemStream(src, len, reserve)
  // UIDocument will free buffer
  ~UIDocStream() override { if(uiDocument) { iosCloseDocument(uiDocument); buffer = NULL; } }

  bool flush() override { iosSaveDocument(uiDocument, data(), size()); return true; }
  const char* name() const override { return url.c_str(); }
  int type() const override { return MEMSTREAM | UIDOCSTREAM; }
};
#endif

// enable update on Android now that we can't update on Google Play
#define ENABLE_UPDATE !PLATFORM_IOS

class MainWindow;
namespace PdfImport { struct MemoryBudget; }
class ScribbleConfig;
class ScribbleDoc;
class ScribbleMode;
class ScribbleArea;
class BookmarkView;
class OverlayWidget;
class DocumentList;
class TagDocList;
struct NewDocChoices;
class PenToolbar;
class Selection;
class Clipboard;
class Page;
struct Url;

class ScribbleApp : public Application
{
public:
  void dropEvent(SDL_Event* event);
  bool keyPressEvent(SDL_Event* event);
  int sdlEventFilter(SDL_Event* event);
  bool sdlEventHandler(SDL_Event* event);

  void newDocument();
  bool openDocument();
  bool importDocument();
  bool saveDocument();
  bool doSave(ScribbleDoc* doc);
  bool doSaveAs();
  bool openHelp();
  void sendPageImage();
  void sendPDF();
  void sendDocument();
  void about();
  void revert();
  void clipboardChange();
  void pasteClipboard();
  void openPreferences();
  void applyConfigChanges();
  void reloadConfig();
  void writeConfigFile();
  void resetDocPrefs();
  void setMode(int mode);
  void showPageSetup();
  void showThemePicker();
  // shows TagDocList; openResult = false leaves acting on the result to the caller
  //  (openOrCreateDocTagged(), which also handles OPEN_WHITEBOARD)
  void execTagDocList(bool openResult = true);
  void askThemeForNewDoc();
  void applyNewDocChoices(const NewDocChoices& choices);
  void openRecentFile(const std::string& filename);
  void createLink();
  void exportPDF();
  // The sidebar's Pages view (docs/agent/page-management.md): the listed pages of the active document as a
  //  PDF (save dialog), as a new Sumi document handed to the share sheet (the save dialog on desktop,
  //  which has none), and as one PNG per page (save dialog, -N suffixes; the share sheet on mobile)
  void exportPagesPDF(const std::vector<int>& pages);
  void sharePagesDocument(const std::vector<int>& pages);
  void exportPagesPNG(const std::vector<int>& pages);
  // a page as an image at the size Send Page Image uses
  Image renderPageImage(Page* page);
  void importPDF();
  bool doImportPdf(const std::string& pdfPath);
  void insertPDF();
  bool insertPdfPages(const std::string& pdfPath);
  std::string importPdfToDocFile(const std::string& pdfPath, std::string* errorOut = NULL);
  // the document browser's import FAB: pick a file of the format and import it into the library;
  //  returns a document to open (one PDF or notebook), empty for an archive, a cancel or a failure
  void importFromBrowser(int kind);
  void browserImportDone(const std::string& docPath);
  std::string importNoteful(const std::string& filename);
  static size_t importMemoryLimit();
  void reportReducedPages(const PdfImport::MemoryBudget& budget, const char* title);
  void insertImage();
  void pickImage();
  void insertImage(const std::string& filename);
  void insertImage(Image image, bool fromintent = false);
  // document scanning: reuses the image picker, then runs the photo through ScanDialog.  asPage decides
  //  whether the result becomes a floating element or the background of a new page.
  void scanDocument(bool asPage);
  void captureScan();
  void finishScan(Image photo);
  void insertScanPage(Image scan);
  bool pendingScan = false;
  bool pendingScanAsPage = false;
  // where the previous scan of an Add more run went, so the next one follows it: the page number when
  //  scanning as pages (-1 = none yet), the element's bounds when scanning as images (invalid = none yet)
  int lastScanPage = -1;
  Rect lastScanRect;
  void showNotify(const std::string& msg, int level = 1);
  void dismissNotify();
  void appSuspending();
  void hideBookmarks();
  void insertDocument();
  void penSelected(int penindex);
  void closeDocs(const FSPath& path);
  void maybeQuit();
  // android callbacks
  bool openDocument(std::string filename);
  static void storagePermission(bool granted);
  static void insertImageSync(Image image, bool fromintent = false);
  // syncing
  bool openSharedDoc(std::string sharename);
  bool openSharedDoc();
  void shareDocument();
  void syncSendImmed();
  void syncView();
  void showSyncInfo();
  //bool syncSignIn(const QUrl& baseurl);
  //void syncMessage(std::string msg, int level);
  // update check
  void updateCheck(bool userreq = true);
#ifdef SCRIBBLE_TEST
  std::string runTest(std::string runtype);
  void runTestUI(std::string runtype);
#endif

  ScribbleApp(int argc, char* argv[]);
  void init();
  ~ScribbleApp();

  // public so that RulingDialog can access
  ScribbleMode* scribbleMode = NULL;
  //ScribbleDoc* scribbleDoc = NULL;
  ScribbleArea* mActiveArea = NULL;
  BookmarkView* bookmarkArea = NULL;
  OverlayWidget* overlayWidget = NULL;
  std::vector<ScribbleDoc*> scribbleDocs;
  std::vector<ScribbleArea*> scribbleAreas;

  std::unique_ptr<Clipboard> clipboard;
  bool newClipboard = false;
  bool clipboardExternal = true;
  const Page* clipboardPage = NULL;
  int clipboardFlags = 0;

  PenToolbar* penToolbar;
  ScribblePen currPen = {Color::BLACK, 1};
  Color bookmarkColor = Color::BLUE;

  std::string tempPath;
  std::string savedPath;
  //std::string backupPath;
  std::string docRoot;
  // The document library (see doclibrary.h): the one directory the tag document browser owns, and where
  //  every document opened from elsewhere is copied to. libraryTemporary means it is a fallback in app
  //  storage (Android without all-files access) that is deleted on uninstall; the documents are moved to
  //  the permanent location, libraryTarget, as soon as it becomes writable.
  std::string libraryRoot;
  std::string libraryTarget;
  bool libraryTemporary = false;
  // --library DIR: use DIR as the library for this run only (docs screenshots, demos); see initLibrary()
  std::string libraryOverride;
  std::string runType;
  std::string argDoc;
  std::string outDoc;
  bool isWindowActive = true;
  bool hasI18n = false;
  static Uint32 scribbleSDLEvent;
  enum scribbleSDLEventCode {INSERT_IMAGE=1, UPDATE_CHECK,
      STORAGE_PERMISSION, DISMISS_DIALOG, SIMULATE_PEN_BTN, IAP_COMPLETE, APP_SUSPEND};

  ScribbleArea* activeArea() const { return mActiveArea; }
  ScribbleDoc* activeDoc() const;
  void setActiveArea(ScribbleArea* area);
  void openSplit();
  bool openSplitDoc();
  bool closeSplit();

  // Editor tabs (docs/agent/editor-tabs.md, editortabs.cpp).  Every document with a file is a tab; a pane
  //  shows one tab's document.  scribbleDocs stays what it always was - the documents shown in a pane -
  //  and a background tab's document is owned by its tab alone, so nothing iterating scribbleDocs ever
  //  meets a document with no pane (doCancelAction, updateDocConfig and the thumbnail need one).
  TabList tabs;
  bool tabsEnabled() const;
  // the tab of the active pane's document, -1 for an untitled document
  int activeTabIndex() const;
  // show tab `idx` in `area` (the active pane by default), saving the document it replaces first
  bool switchToTab(int idx, ScribbleArea* area = NULL);
  // save the tab's document and close it; every pane showing it moves to a neighbouring tab, or to the
  //  library when it was the last
  bool closeTab(int idx);
  void moveTab(int from, int to);
  // a tab row dragged out of the sidebar and released over a pane at `pos` (window coordinates): onto
  //  the existing pane of a split, or - unsplit - into a new pane on the edge nearest the drop
  bool dropTabOnCanvas(int idx, Point pos);
  // feedback while dragging: highlight what dropTabOnCanvas would do at `pos`; NaN hides it
  void previewTabDrop(Point pos);
  // bring the list in line with the documents in the panes (new file, Save As, a document reset to
  //  untitled); called from onLoadFile
  void syncTabs();

  void repaintBookmarks(bool newdoc = false);
  void refreshUI(ScribbleDoc* doc, int reason);
  bool oneTimeTip(const char* id, Point pos = {}, const char* message = NULL);
  void showSelToolbar(Point pos);
  void doCommand(int cmd);
  void setClipboard(Clipboard* cb, const Page* srcpage, int flags);
  void loadClipboardText(const char* clipText, size_t len = 0);
  void loadClipboard();
  bool storeClipboard();
  void setClipboardToImage(Image img, bool lossy = false);
  void penChanged(int changed);
  void updatePenToolbar();
  void setPen(const ScribblePen& pen);
  void setDrawTool(int tool);
  const ScribblePen* getPen() const { return &currPen; }
  void loadConfig();

  static ScribbleApp* app;
  // a document color as the canvas currently draws it (dark mode maps it); for swatches and previews,
  //  never for comparing against or storing a pen color
  static Color displayColor(Color c);
  static MainWindow* win;
  static ScribbleConfig* cfg;
  static Dialog* currDialog;
  // UI units at the top of the window that system UI covers (the iOS status bar, the notch); the window
  //  draws edge to edge, so the toolbar and the browser's search row step down by this much
  static Dim topInset;

  enum MessageType {Info, Question, Warning, Error};
  static std::string messageBox(
      MessageType type, std::string title, std::string message, std::vector<std::string> buttons = {"OK"});
  static Dim getPreScale();
  static void getScreenPageDims(int* w, int* h);
  struct PageSizePreset { std::string title; int width, height; };
  static std::vector<PageSizePreset> pageSizePresets();
  static bool localeUsesLetter();
  static bool openURL(const char* url);
#if PLATFORM_ANDROID
  static bool hasAndroidPermission();
  static bool requestAndroidPermission();
#endif

  // the folder the tag document browser lists, whose .write-tags holds the tags page tags come from
  std::string tagBrowserRoot() const;

private:
  friend class ScribbleTest;

  DocumentList* documentList = NULL;
  TagDocList* tagDocList = NULL;
  std::string cfgFile;
  std::string fileExt;
  std::string httpUserAgent;
  std::string currFolder;
  std::vector<std::string> recentDocs;
  std::vector< std::unique_ptr<Action> > recentFileActions;
  //std::string backupFilename;
  int clipboardSerial = 0;  // iOS and Windows
  int currPenIndex;
  bool backToDocList = true;
  bool delayedShowDocList = false;
  bool disableConfigSave = false;
  size_t historyPos = 0;  // used in penChanged

  bool maybeSave(bool prompt = false);
  // editor tabs, see the public section
  bool showTabInArea(int idx, ScribbleArea* area);
  void attachDoc(ScribbleDoc* doc, ScribbleArea* area, const TabViewState* view);
  ScribbleDoc* detachDoc(ScribbleArea* area);
  void releaseDocIfOrphan(ScribbleDoc* doc);
  void rebuildShownDocs();
  void showUntitledInArea(ScribbleArea* area);
  bool saveTabDoc(ScribbleDoc* doc);
  void unloadIdleTabs();
  void restoreTabs();
  void updateSplitLabels();
  ScribbleArea* areaAt(Point pos) const;
  int tabInsertAnchor = -1;
  Timer* tabUnloadTimer = NULL;
  bool checkExtModified();
  bool checkExtModified(ScribbleDoc* doc);
  void doNewDocument();
  bool doOpenDocument(std::string filename);
  bool doOpenDocument(IOStream* filestrm);
  void populateRecentFiles();
  // pages: page numbers to write, in that order; empty = the whole document
  void writePDF(std::ostream& strm, const std::vector<int>& pages = {});
  bool writePDF(const std::string& filename, const std::vector<int>& pages = {});
  void saveConfig();
  void setWinTitle(const std::string& filename);
  void onLoadFile(const std::string& filename, bool addrecent = true);
  void sendFile(const std::string& body, const std::string& attachfile);
  // several files in one share sheet (mobile only)
  void sendFiles(const std::string& body, const std::vector<std::string>& attachfiles);
  // the suggested file name (no extension) for an export of `pages`
  std::string pagesExportName(const std::vector<int>& pages);
  bool openOrCreateDoc(bool cancelable = false);
  // the two document browsers behind openOrCreateDoc(), chosen by the useTagDocList pref
  bool openOrCreateDocClassic(bool cancelable = false);
  bool openOrCreateDocTagged(bool cancelable = false);
  void gotoSelectedPage();
  std::string execDocumentList(int mode, const char* exts = NULL, bool cancelable = true) ;
  std::string createRecoveryName(std::string filename, const char* toappend = " - recovered");
  std::string docDisplayName(std::string filename, int maxwidth=0);
  std::string docShortName(const std::string& filename, int maxwidth=0);
  Clipboard* importExternalDoc(SvgDocument* doc);
  bool openExternalDoc();
  // the library is in use whenever the tag document browser is (iOS keeps its own document picker)
  bool libraryManaged() const;
  bool isInLibrary(const std::string& filename) const;
  std::string defaultLibraryBase() const;
  void initLibrary();
  void adoptPermanentLibrary();
  bool relocateLibrary(const std::string& newBase);
  void offerLibraryMigration();
  void askDefaultPageSize();
  bool moveLibraryTo(const std::string& newRoot);
  bool importActiveDocToLibrary(const std::string& srcFile);
#if PLATFORM_ANDROID
  Semaphore saveSem;
#endif
  // update
#if ENABLE_UPDATE
  //std::thread* updateThread;
  bool userUpdateReq = false;
  int updateSocket = -1;
  void updateNotify(const char* msg, int level);
  void updateInfoReceived(char* updateData);
  static int updateThreadFn(void* _self);
#endif
  // sync
  bool syncSignIn(const Url& baseurl);
  std::string syncAPICall(const Url& baseurl, const std::string& route);
  bool doSharedDoc(std::string host, std::string reply, bool master);

  Toolbar* notifyBar = NULL;
  TextBox* notifyText = NULL;
  Timer* notifyTimer = NULL;
  Timer* autoSaveTimer = NULL;
  Timer* penSaveTimer = NULL;  // debounced config write after a pen or width preset change
  std::string syncSession;  // session cookie

  static const int volUpActions[];
  static const int volDnActions[];
};
