#include "linkdialog.h"
#include "ugui/textedit.h"
#include "ugui/colorwidgets.h"
#include "ulib/fileutil.h"
#include "mainwindow.h"
#include "scribbledoc.h"
#include "scribbleapp.h"
#include "filepicker.h"

// href formats (docs/agent/links.md):
//  "#pg-xxxxxxxxxx"         a page of this document, by the page's anchor (Page::anchorId)
//  "<path>#page=N"          page N (1-based) of another document, path relative to this one when possible
//  "#b-xxxxxxxxxx"          a bookmark (older documents; still followed, no longer created)
//  anything else            a URL

static const char* OTHER_DOC = "\x01";

// path of `target` relative to the folder of `from`, "./"-prefixed so ScribbleDoc::openURL() takes it for
//  a local file; absolute if `from` is empty (an unsaved document has no folder to be relative to)
static std::string relativeDocPath(const std::string& from, const std::string& target)
{
  if(from.empty())
    return target;
  std::string fromdir = canonicalPath(FSPath(from).parent());
  std::vector<std::string> fromparts = splitStr<std::vector>(fromdir.c_str(), '/', true);
  std::vector<std::string> targetparts = splitStr<std::vector>(target.c_str(), '/', true);
  size_t common = 0;
  while(common < fromparts.size() && common + 1 < targetparts.size()
      && fromparts[common] == targetparts[common])
    ++common;
  // nothing in common but the root: an absolute path says the same thing more plainly
  if(common == 0)
    return target;
  std::string rel;
  for(size_t ii = common; ii < fromparts.size(); ++ii)
    rel += "../";
  if(rel.empty())
    rel = "./";
  for(size_t ii = common; ii < targetparts.size(); ++ii)
    rel += targetparts[ii] + (ii + 1 < targetparts.size() ? "/" : "");
  return rel;
}

// the document file a link's path part names, resolved against the linking document; "" if not a file
static std::string resolveDocPath(const std::string& from, const std::string& path)
{
  std::string local = path;
  if(local.compare(0, 7, "file://") == 0)
    local = local.substr(7);
  if(local.empty() || (local[0] != '.' && local[0] != '/'))
    return "";
  FSPath file = local[0] == '.' ? FSPath(from).parent().child(local) : FSPath(local);
  return file.exists() ? canonicalPath(file) : "";
}

LinkDialog::LinkDialog(ScribbleDoc* sd) : PopupDialog(createPopupDialogNode()), scribbleDoc(sd)
{
  ScribbleArea* scribbleArea = scribbleDoc->activeArea;
  StrokeProperties props(Color::INVALID_COLOR, -1);
  Selection* sel = scribbleArea->currSelection;
  if(sel)
    props = sel->getStrokeProperties();
  // NULL if not a hyperref
  const char* href = scribbleArea->getHyperRef();
  origHref = href ? href : "";

  Rect pbbox = ScribbleApp::win->winBounds();
  setWinBounds(Rect::centerwh(pbbox.center(), std::min(pbbox.width(), Dim(500)), std::min(pbbox.height() - 60, Dim(340))));

  // linkColor might make sense as per-doc option, but there's no clear way in UI to set global vs. per-doc
  Color linkcolor = Color::fromArgb(ScribbleApp::cfg->Int("linkColor"));
  penColorPicker = createColorEditBox();

  typeCombo = createComboBox({_("Outline"), _("Page"), _("URL")});
  typeCombo->onChanged = [this](const char*){ setType(typeCombo->index()); };

  // Outline: the entries of this document, indented by level as in the sidebar
  outline = scribbleDoc->outline(true);
  std::vector<std::string> entries;
  for(const OutlineEntry& entry : outline)
    entries.push_back(std::string(2*entry.level, ' ') + entry.title);
  if(entries.empty())
    entries.push_back(_("No outline entries"));
  outlineCombo = createComboBox(entries);
  outlineCombo->setEnabled(!outline.empty());

  // Page: a document (this one, another open tab, or any file) and a page of it
  std::vector<std::string> docnames = {_("This Document")};
  docChoices = {""};
  std::string thisdoc = scribbleDoc->fileName()[0] ? canonicalPath(FSPath(scribbleDoc->fileName())) : "";
  for(const EditorTab& tab : ScribbleApp::app->tabs.tabs) {
    if(tab.path.empty() || !FSPath(tab.path).exists())
      continue;
    std::string path = canonicalPath(FSPath(tab.path));
    if(path == thisdoc || std::find(docChoices.begin(), docChoices.end(), path) != docChoices.end())
      continue;
    docChoices.push_back(path);
    docnames.push_back(FSPath(path).baseName());
  }
  docChoices.push_back(OTHER_DOC);
  docnames.push_back(_("Other Document..."));
  docCombo = createComboBox(docnames);
  docCombo->onChanged = [this](const char*){
    int idx = docCombo->index();
    if(idx < 0 || idx >= int(docChoices.size()))
      return;
    if(docChoices[idx] != OTHER_DOC) {
      if(!setTargetDoc(docChoices[idx]))
        docCombo->setIndex(docComboIndex);
      return;
    }
    // the choice stays what it was until a document is picked
    docCombo->setIndex(docComboIndex);
    std::weak_ptr<bool> alive = aliveToken;
    FilePicker::openFile(_("Link to Document"), "svgz svg html htm", [this, alive](const std::string& path){
      if(alive.expired() || path.empty())
        return;
      setTargetDoc(canonicalPath(FSPath(path)));
    });
  };

  pageSpin = createTextSpinBox(1, 1, 1, 1, "%.0f", 60);
  pageCountLabel = new TextBox(createTextNode(""));
  pageCountLabel->setMargins(0, 0, 0, 8);

  hrefEdit = createTextEdit();
  hrefEdit->node->setAttribute("box-anchor", "hfill");  // fill available width ...
  hrefEdit->setMargins(0, 0, 0, 16);  // ... save for a bit of a gap

  Widget* dialogBody = selectFirst(".body-container");
  dialogBody->setMargins(0, 8);
  dialogBody->addWidget(createTitledRow(_("Color"), penColorPicker));
  dialogBody->addWidget(createTitledRow(_("Link to"), typeCombo));
  dialogBody->addWidget(outlineRow = createTitledRow(_("Entry"), outlineCombo));
  dialogBody->addWidget(docRow = createTitledRow(_("Document"), docCombo));
  Widget* pagectrls = createRow({pageSpin, pageCountLabel});
  dialogBody->addWidget(pageRow = createTitledRow(_("Page"), pagectrls));
  dialogBody->addWidget(urlRow = createTitledRow(_("URL"), hrefEdit));
  dialogBody->addWidget(createStretch());

  // color picker must be added to document before setting color
  penColorPicker->setColor(linkcolor.isValid() ? linkcolor : props.color);

  // default for a new link: an outline entry if there are any, else a page; then whatever the link says
  setTargetDoc("", scribbleArea->getCurrPageNum());
  int type = outline.empty() ? TARGET_PAGE : TARGET_OUTLINE;
  if(!origHref.empty()) {
    parseHref(origHref.c_str());
    type = origType;
  }
  typeCombo->setIndex(type);
  setType(type);

  setTitle(_("Create Link"));
  acceptBtn = addButton(_("OK"), [this](){ accept(); });
  cancelBtn = addButton(_("Cancel"), [this](){ finish(CANCELLED); });
}

void LinkDialog::setType(int type)
{
  outlineRow->setVisible(type == TARGET_OUTLINE);
  docRow->setVisible(type == TARGET_PAGE);
  pageRow->setVisible(type == TARGET_PAGE);
  urlRow->setVisible(type == TARGET_URL);
}

bool LinkDialog::setTargetDoc(const std::string& path, int pagenum)
{
  std::string thisdoc = scribbleDoc->fileName()[0] ? canonicalPath(FSPath(scribbleDoc->fileName())) : "";
  int npages = 0;
  if(path.empty() || path == thisdoc)
    npages = scribbleDoc->document->numPages();
  else {
    // only the page count is needed: a .svgz loads its pages lazily, so this reads little more than the index
    Document otherdoc;
    if(otherdoc.load(new FileStream(path.c_str(), "rb"), true) != Document::LOAD_OK || otherdoc.numPages() <= 0) {
      ScribbleApp::messageBox(ScribbleApp::Warning, _("Create Link"), _("Unable to open the document."));
      return false;
    }
    npages = otherdoc.numPages();
  }
  targetDoc = path == thisdoc ? "" : path;
  targetPageCount = npages;
  // ComboBox can only append, so a picked document is listed after "Other Document..."
  auto it = std::find(docChoices.begin(), docChoices.end(), targetDoc);
  if(it == docChoices.end()) {
    docCombo->addItems({FSPath(targetDoc).baseName()});
    it = docChoices.insert(docChoices.end(), targetDoc);
  }
  docComboIndex = int(it - docChoices.begin());
  docCombo->setIndex(docComboIndex);
  pageSpin->setLimits(1, npages);
  pageSpin->setValue(std::min(std::max(0, pagenum), npages - 1) + 1);
  pageCountLabel->setText(fstring(_("of %d"), npages).c_str());
  return true;
}

void LinkDialog::parseHref(const char* href)
{
  Document* doc = scribbleDoc->document;
  std::string link(href);
  size_t hash = link.find('#');
  std::string path = hash == std::string::npos ? link : link.substr(0, hash);
  std::string frag = hash == std::string::npos ? "" : link.substr(hash);
  origType = TARGET_URL;
  if(path.empty() && !frag.empty()) {
    // within this document: a page anchor, a page number or an old bookmark
    int pagenum = doc->findPageTarget(frag.c_str());
    if(pagenum < 0) {
      pagenum = scribbleDoc->activeArea->getCurrPageNum();  // where the search starts
      if(!doc->findNamedNode(frag.c_str(), &pagenum))
        pagenum = -1;
    }
    if(pagenum >= 0) {
      origType = TARGET_PAGE;
      origPage = pagenum;
      // a page anchor on a page with an outline entry was made by picking that entry
      if(doc->findPageTarget(frag.c_str()) >= 0) {
        for(size_t ii = 0; ii < outline.size(); ++ii) {
          if(outline[ii].pagenum == pagenum) {
            origType = TARGET_OUTLINE;
            origOutline = int(ii);
            outlineCombo->setIndex(int(ii));
          }
        }
      }
      setTargetDoc("", pagenum);
    }
  }
  else if(!path.empty()) {
    std::string docpath = resolveDocPath(scribbleDoc->fileName(), path);
    if(!docpath.empty()) {
      int pagenum = 0;
      if(frag.compare(0, 6, "#page=") == 0)
        pagenum = std::max(0, atoi(frag.c_str() + 6) - 1);
      if(setTargetDoc(docpath, pagenum)) {
        origType = TARGET_PAGE;
        origDoc = targetDoc;
        origPage = int(pageSpin->value()) - 1;
      }
    }
  }
  if(origType == TARGET_URL)
    hrefEdit->setText(href);
  origDoc = targetDoc;
}

void LinkDialog::accept()
{
  ScribbleArea* scribbleArea = scribbleDoc->activeArea;
  Color c = penColorPicker->color();
  StrokeProperties props(c, -1);
  ScribbleApp::cfg->set("linkColor", c.argb());

  int type = typeCombo->index();
  int pagenum = int(pageSpin->value()) - 1;
  // unchanged: keep the link exactly as it was (it may point somewhere within a page)
  bool unchanged = !origHref.empty() && type == origType && ((type == TARGET_OUTLINE
      && outlineCombo->index() == origOutline) || (type == TARGET_PAGE && targetDoc == origDoc && pagenum == origPage));
  if(unchanged)
    scribbleArea->setSelProperties(&props, origHref.c_str());
  else if(type == TARGET_URL) {
    if(hrefEdit->text().empty())
      return;  // nothing to link to; the dialog stays open
    scribbleArea->setSelProperties(&props, hrefEdit->text().c_str());
  }
  else if(type == TARGET_OUTLINE) {
    int idx = outlineCombo->index();
    if(idx < 0 || idx >= int(outline.size()))
      return;
    scribbleArea->createPageLink(scribbleDoc->document->pages[outline[idx].pagenum], &props);
  }
  else if(targetDoc.empty()) {
    if(pagenum < 0 || pagenum >= scribbleDoc->document->numPages())
      return;
    scribbleArea->createPageLink(scribbleDoc->document->pages[pagenum], &props);
  }
  else {
    // another document is never written to, so its page is named by number
    std::string link = relativeDocPath(scribbleDoc->fileName(), targetDoc) + fstring("#page=%d", pagenum + 1);
    scribbleArea->setSelProperties(&props, link.c_str());
  }
  finish(ACCEPTED);
}
