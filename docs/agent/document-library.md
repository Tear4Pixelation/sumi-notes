# Document library

The tag document browser owns **one directory, the library**, and every document opened from anywhere
else is copied into it - the library is the only place Write writes documents. Filesystem logic is in
`syncscribble/doclibrary.cpp` (no app dependencies, tested standalone by `scribbletest/librarytest.cpp`),
the policy in `ScribbleApp::initLibrary()` and its neighbours. Active whenever `useTagDocList` is on, on
every platform but wasm (`libraryManaged()`); with it off, everything behaves as before - on iOS that means
the system `UIDocumentBrowserViewController`.

- **Acquired, never assumed.** `DocLibrary::acquire(base)` takes `base`, `base 2`, ... - the first that is
  already a library, empty (OS litter like `.DS_Store` ignored), or creatable - and marks it with a
  `.write-library` file. The marker is what tells "our library from last run" from "a folder the user
  happens to call Write"; without it the browser would recurse into the user's folder and write its tag
  index there. The result is stored as `libraryPath` so later runs reuse it rather than re-searching.
- **Locations:** desktop `<documents>/Write` (XDG_DOCUMENTS_DIR on Linux, so it follows localized names),
  changeable under Preferences > Document List > Library folder (a text field; changing it *moves* the
  documents via `relocateLibrary()`, and a non-empty target gets a `Write` folder inside it). Android
  `/sdcard/Documents/Write`, i.e. shared storage that survives uninstall - **only once all-files access is
  granted**, because without it a reinstall cannot read the files it left there. Until then documents go
  to `Android/data/.../files/Write/` with a warning each time the browser opens, and
  `adoptPermanentLibrary()` moves them on the `STORAGE_PERMISSION` grant. `acquire()` rather than a plain
  create there is also how a reinstall finds the previous install's library again. iOS: `$HOME/Documents/Write`,
  which the Files app shows under Write (`UIFileSharingEnabled`), so the library stays reachable from outside.
- **An unreachable saved library is not forgotten.** A saved `libraryPath` whose parent is missing (an
  unmounted drive) puts the session in the fallback next to the config file without overwriting
  `libraryPath`; the next run that can reach it moves the fallback's documents back in. A library the user
  deleted (parent still there), or emptied marker and all, is simply recreated.
- **`--library DIR` (or `--library=DIR`) is a throwaway library for docs screenshots and demos.** DIR is
  created if missing and adopted even if it already holds documents (a prepared demo set; it gets a marker).
  It disables config saving outright (beating `--saveconfig`), so the real `libraryPath` and recent
  documents survive; it clears the recent list for the session and points `currFolder` at DIR, since Save As
  still opens the classic `DocumentList` there. There is no fallback and no migration offer: if DIR cannot be
  used the app logs and exits rather than showing the user's real library. Not the same as
  `--libraryPath=DIR`, which goes through the saved-library path and rejects a non-empty unmarked folder.
- **Import is one branch in `doOpenDocument(std::string)`**, which covers drag and drop, Android intents,
  Import Document... and recent files: open the original read-only, then `importActiveDocToLibrary()`
  saves it into the library as `docFileExt` and continues in the copy. Saving through the document rather
  than copying bytes is deliberate - it converts multi-file HTML, which the browser does not list. The
  original is dropped from recent documents, or picking it there would import a second copy. The command
  line opens via `activeDoc()->openDocument()` directly and does **not** import automatically: a file named
  there was picked deliberately from where it is, so a dialog offers Copy to Library or Edit Original, and
  declining edits the file in place. A `--out` conversion never asks or imports. PDF import writes straight into the library.
- **iOS runs in "library mode"** (`initLibraryMode()` in `ioshelper.m`): SDL's view controller stays the
  root instead of being presented over a `UIDocumentBrowserViewController`, and library documents are plain
  `FileStream`s rather than `UIDocument`s - so recents work there too (the title button's menu was hidden
  on iOS only because recents needed security-scoped bookmarks), and the browser is cancelable. Anything
  arriving from Files or another app's "Open in" still comes in as a `UIDocument`-backed `UIDocStream`;
  `dropEvent()` then imports it and never keeps editing through that `UIDocument`, which would write the
  original back. Import Document... and the browser's Import menu go through `FilePicker` instead (below).
  A file that is already in the library is reopened as a plain file instead of copied.
- **`/var` vs `/private/var`** (iOS, also macOS): `/var` is a symlink to `/private/var`, paths arrive in
  either spelling - the library's own included - and `canonicalPath()` does not resolve symlinks.
  `DocLibrary::contains()` strips the prefix from **both** sides. It used to be stripped from the file
  only, in `isInLibrary()`; with a library path that had the prefix, every document then looked external
  and every open from the browser imported another copy ("Copied ... into the document library", a new
  duplicate each time). `librarytest.cpp` pins both spellings.
- **The iOS container moves.** `.../Containers/Data/Application/<UUID>/` can change when the app is
  updated, while the saved `libraryPath` is absolute. `initLibrary()` rebases a saved path into another
  container onto the current `$HOME`, or every update would land in the fallback library.
- **The classic folder browser is gone as a browser**; its menu entry is now **Import Document...**, the
  system file picker whose result `doOpenDocument()` then copies in. The browser's Import FAB offers
  Document, PDF and Noteful on every platform (it used to be hidden on iOS).
- **Migration is a one-time offer, copying not moving** (`offerLibraryMigration()`, `libraryMigrated`):
  documents under the old `currFolder` (plus the legacy Android folders), depth 3 since that may be a home
  directory, excluding multi-file HTML. `.write-tags` is carried over so tag names survive; recent
  documents are remapped to the copies.
- Moving the library (`moveLibraryTo()`) saves and reopens open documents from their new path and
  repoints a live browser with `TagDocList::setRoot()` - deleting it could free a window still inside
  `execWindow`.

Known gaps: desktop Save As still writes wherever it is pointed and the document then lives there; the
library folder pref is a text field, not a folder picker; merging a fallback into a library that already
has a `.write-tags` keeps the destination's and leaves the other behind; verified on Linux only.

# System file picker (`filepicker.cpp`)

Every place a file is picked or saved uses the operating system's dialog - Import Document/PDF/Noteful,
the browser's Import menu, Insert Document, Insert Image (desktop; mobile already used the photo/camera
pickers), Save As and Export PDF. `DocumentList` is only the fallback (`FilePicker::setFallback()`), for
wasm and a Linux desktop with none of the dialogs below.

- **Linux:** the xdg-desktop-portal FileChooser over D-Bus (libdbus `dlopen`ed, so the binary still starts
  without it), then `zenity`, then `kdialog`. The wait pumps SDL so the window stays responsive, and drops
  the input queued meanwhile. `SUMI_FILE_PICKER=portal|zenity|kdialog|none` forces one;
  **`SUMI_FILE_PICKER=fixed:/path`** answers every pick with that path - use it in the agent display,
  where a portal dialog would open on the user's real desktop instead of inside cage.
- **Windows** `IFileOpenDialog`/`IFileSaveDialog`; **macOS** `NSOpenPanel`/`NSSavePanel` (`macoshelper.m`).
- **iOS** `UIDocumentPickerViewController` (`ioshelper.m`, `iosPickFile`), opened in place, not as a copy:
  a file under the app's own home is passed on by path (a copy of a library document would be imported
  as a duplicate); anything else is copied to `tmp/picked/<uuid>/` under security-scoped, coordinated
  access. Extensions without a system type (svgz, noteful) map to `public.data`.
- **Android** `ACTION_OPEN_DOCUMENT`; `MainActivity.copyToCache()` copies the content URI into
  `cache/picked/<n>/` on a worker thread and reports back through `jniFilePicked`.
- **Results are always asynchronous**: the callback runs from the event loop (`FilePicker::handleEvent`,
  first thing in `sdlEventHandler`), never inside `openFile()`, even on desktop where the dialog is modal.
  iOS and Android cannot do otherwise, and this way the desktop runs - and tests - the same path. The
  browser's Import therefore shows the browser again right after starting the picker; the result arrives
  while it is up, and `browserImportDone()` ends it with `EXISTING_DOC` on the imported document, as if
  it had been tapped.
- **Saving:** desktop asks for the path synchronously (`FilePicker::savePath()`), since Save As and
  `maybeSave()` need the answer at once. On Android and iOS `saveFile()` writes to `tmp/export/` and hands
  that file to `ACTION_CREATE_DOCUMENT` / the export picker - so mobile **Save As saves a copy** and the
  document stays in the library (iOS without the library keeps `iosSaveAs`). This also made Export PDF
  work on iOS, where it used to do nothing.

Gaps: only the Linux implementation has run; the Windows, macOS, Android and iOS code was written
without a compiler for those platforms. New strings are untranslated.

# Browser fonts

The browser's theme uses Raleway (the "Sumi" title) and Satoshi (everything else) with no fallback family
(`ugui/theme.cpp`). fontstash reads a font file only when it first draws with it, so `loadFont()` succeeds
for a file that does not exist and the text silently never appears: that is how the iOS browser lost its
title, "All Documents", both search placeholders, tag names and document names - the fonts were never
in `IOSRES`. Both now ship on every platform (`DISTRES`/`IOSRES`, Android assets extracted to `.saved/`,
the WiX installer), and `setupResources()` registers the UI sans font under a face's name when its file
is missing - checking existence, not `loadFont()`'s result.

# Create Notebook (new document dialog)

The tag browser's "+" opens `CreateNotebookDialog` (`newdocdialog.cpp`; `NewDocDialog` was taken, by
`documentlist.h`): a Cover tile, a Pages tile, the name, tag chips with "+ Add tags", and a Cover toggle.
Designed in the Penpot page "Create page" as a proposition, not followed to the pixel.

- **A cover is one stored color.** `coverColor` (ARGB, per-document config; 0 = no cover) holds only the
  seed; `Cover::bandColor()` (`cover.cpp`) derives the band by moving OKLab lightness towards the middle -
  darker on a light seed, lighter on a dark one, chroma clamped to what the new lightness holds - so black
  gets a gray band. Nothing derived is stored, so a cover cannot disagree with itself.
- **The grid draws the cover instead of the page thumbnail** when `coverColor` is set, read with
  `ScribbleDoc::extractDocConfigValue()` - `extractDocTags()` generalized to any config value.
- **Pages reuses the Add Page grid**: `AddPageMenu::createLayoutGrid()` is the same builder with a tile
  action passed in. The pick becomes the document default as well as the first page's ruling.
- **Tags**: the browser's own `TagStore` is passed in; tags made in the dialog are saved at once (so they
  survive Cancel), and the tags being filtered on when "+" was pressed are pre-selected.
- The dialog touches no files: `TagDocList::newDoc()` creates the file and carries `NewDocChoices` to
  `ScribbleApp::applyNewDocChoices()`, which applies them to the opened document and saves it right
  away - an untouched new document is otherwise never written, and the cover and tags live in it.
  The theme prompt still follows (`askThemeForNewDoc()`).
- The last cover/layout are remembered globally (`newDocCover`, `newDocCoverColor`, `newDocLayout`).
- Traps hit: `ColorEditBox::setColor()` segfaults before the widget is in a document (its slider
  gradients resolve by reference), so the custom color is set when the popup opens; chips and the
  "Create tag" row are removed by rebuilds their own clicks trigger, so those rebuilds go through a
  1 ms `setTimer`; in a flex column a child without `box-anchor` is centred, and an empty `hfill` row
  takes the whole width.

Known gaps: the classic `DocumentList` new-document path (non-library platforms) is unchanged; the Pages
popup opens downwards and runs off a 720 px screen's bottom; no automated test; new strings are untranslated.
