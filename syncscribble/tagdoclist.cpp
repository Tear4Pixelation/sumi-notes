#include "tagdoclist.h"
#include "cover.h"

#include "scribbleapp.h"
#include "scribbledoc.h"
#include "document.h"
#include "scribbleconfig.h"
#include "pdfimport.h"
#include "ulib/stringutil.h"
#include <algorithm>
#include <codecvt>
#include <locale>

// A thumbnail in the frame every preview shares (cover.h): corners rounded off and the outline on top,
//  as one group the size of frame.  cropToFrame cuts a notebook's thumbnail to the frame's proportions
//  from its top left - those saved before the frame (240 x 400, the view rather than the page) lose their
//  bottom, which is better than a frame taller than every other notebook's.
static SvgNode* createFramedThumbnail(Image thumbnail, const Rect& frame, bool cropToFrame)
{
  if(cropToFrame) {
    real cropWidth = std::min(real(thumbnail.width), thumbnail.height/Cover::PREVIEW_ASPECT);
    real cropHeight = std::min(real(thumbnail.height), thumbnail.width*Cover::PREVIEW_ASPECT);
    if(int(cropWidth) < thumbnail.width || int(cropHeight) < thumbnail.height)
      thumbnail = thumbnail.cropped(Rect::wh(cropWidth, cropHeight));
  }
  Cover::roundCorners(&thumbnail, Cover::previewRadius(frame.width())*thumbnail.width/frame.width());
  SvgG* group = new SvgG();
  group->addChild(new SvgImage(std::move(thumbnail), frame.toSize()));
  group->addChild(loadSVGFragment(Cover::outlineSVG(frame.width(), frame.height()).c_str()));
  return group;
}

TagNameDialog::TagNameDialog(const char* title, const char* initialName) : PopupDialog(createPopupDialogNode())
{
  nameEdit = createTextEdit();
  nameEdit->setText(initialName);
  setMinWidth(nameEdit, 260);
  Widget* dialogBody = selectFirst(".body-container");
  dialogBody->addWidget(nameEdit);
  dialogBody->setMargins(8, 8, 0, 8);
  focusedWidget = nameEdit;
  nameEdit->selectAll();

  setTitle(title);
  acceptBtn = addButton(_("OK"), [this](){ finish(ACCEPTED); });
  cancelBtn = addButton(_("Cancel"), [this](){ finish(CANCELLED); });
}

static const char* tagDocListWindowSVG = R"#(
<svg class="window tagdoclist" layout="box">
  <g id="main-layout" box-anchor="fill" layout="flex" flex-direction="row"></g>
</svg>
)#";

// Pixel values throughout createUI() (sidebar width 367 - see SIDEBAR_WIDTH below - padding 32/24/12/24, row height 50, icon
// size 24, FAB diameters 67/90, ...) are taken directly off the Penpot "Screen" board, which is the
// design's primary reference (COLORS/CLAUDE.md convention: match the design file, don't eyeball it).
// Sidebar geometry, shared by createUI() and elideTagRowTitle(), which derives a tag name's room from it.
//  The design's 367 never took effect (see the sizer rect in createUI()); the sidebar was ~222, sized
//  by its contents. 260 widens that a little. The tag list reaches TAG_LIST_BLEED_* further out on both
//  sides than the rest of the sidebar's content (a negative margin), so the tree gets almost the whole
//  width while the header, search box and bottom toolbar keep the design's padding. Rows indent 16 per
//  level (was 20), for the same reason: nested tags keep room for their name and chevron.
static constexpr int SIDEBAR_WIDTH = 260;
static constexpr int SIDEBAR_PAD_LEFT = 24, SIDEBAR_PAD_RIGHT = 24;
static constexpr int TAG_LIST_BLEED_LEFT = 12, TAG_LIST_BLEED_RIGHT = 18;
static constexpr int TAG_ROW_LEFT = 0, TAG_ROW_RIGHT = 4, TAG_ROW_INDENT = 16;

TagDocList::TagDocList(const char* root) : Window(createWindowNode(tagDocListWindowSVG)), tagStore("")
{
  docRoot = root;
  docFileExt = ScribbleApp::cfg->String("docFileExt");
  // Deliberately narrower than DocumentList's fileExts (which also matches html/htm): DocumentList
  // only ever lists one folder the user chose to open, so a stray non-Write html file there is the
  // user's own business. This view recurses the whole root unconditionally, so it stays to Write's
  // own native formats - otherwise it also picks up every unrelated html/svg file elsewhere in the
  // tree (browser downloads, node_modules, ...) and tries to parse them as documents.
  fileExts = docFileExt + " svgz svg";
  iconWidth = ScribbleApp::cfg->Int("thumbnailSize", 140) * ScribbleApp::getPreScale();

  // one tag index per document root, next to the documents themselves
  tagStore = TagStore(docRoot.child(".write-tags").c_str());
  tagStore.load();

  createUI();
  setTitle(_("Documents"));
}

Widget* TagDocList::createRoundedBg(real w, real h, real radius, const char* fillColor)
{
  SvgRect* rect = new SvgRect(Rect::wh(w, h), radius, radius);
  rect->setAttribute("box-anchor", "fill");
  rect->setAttribute("fill", fillColor);
  return new Widget(rect);
}

Widget* TagDocList::createFab(const char* iconPath, real diameter, bool primary)
{
  // The background rect's own (non-fill-anchored) geometry is what actually drives this button's
  // size - explicit width/height attributes on the outer <g> were silently ignored by the box
  // layout, which instead auto-sized the whole button down to the icon's own small bounds (the only
  // child that wasn't box-anchor="fill" deferring to a parent size nothing was actually setting).
  Button* fab = new Button(new SvgG());
  fab->node->setAttribute("layout", "box");
  if(primary)
    fab->node->addClass("fab-primary");
  SvgRect* bg = new SvgRect(Rect::wh(diameter, diameter), 10, 10);
  bg->addClass("fab-bg");  // colors are theme variables (.tagdoclist .fab-bg in theme.cpp)
  fab->containerNode()->addChild(bg);

  real iconSize = primary ? 27 : 18;
  SvgUse* icon = new SvgUse(Rect::wh(iconSize, iconSize), "", SvgGui::useFile(iconPath));
  icon->addClass("icon");
  fab->containerNode()->addChild(icon);
  return fab;
}

// A button of select mode's bar: a FAB-sized slot with no background of its own, since the bar's
//  container is the background; only hovering/pressing lights up its rounded square (theme.cpp).
Button* TagDocList::createSelectBarButton(const char* iconPath, const char* tooltip)
{
  Button* btn = new Button(new SvgG());
  btn->node->addClass("selectbar-btn");
  btn->node->setAttribute("layout", "box");
  SvgRect* bg = new SvgRect(Rect::wh(44, 44), 10, 10);
  bg->addClass("selectbar-btn-bg");
  btn->containerNode()->addChild(bg);
  SvgUse* icon = new SvgUse(Rect::wh(20, 20), "", SvgGui::useFile(iconPath));
  icon->addClass("icon");
  btn->containerNode()->addChild(icon);
  setupTooltip(btn, tooltip);
  return btn;
}

// A plain left-aligned icon+label row, used for "All Documents" - the standard toolbutton chrome
// (centered content, fixed 36x42 background, its own internal margins) doesn't match the tight,
// hand-tuned padding of the tag rows next to it, so it read as having far more padding than them.
Widget* TagDocList::createNavRow(const char* iconPath, const char* title)
{
  static const char* rowProtoSVG = R"(
    <g class="listitem" layout="box" box-anchor="hfill">
      <rect box-anchor="fill" width="20" height="50"/>
      <g layout="flex" flex-direction="row" box-anchor="hfill">
        <g class="icon-container" margin="0 12 0 0"></g>
        <text class="title" box-anchor="left"></text>
        <g class="spacer" box-anchor="fill" layout="box"></g>
      </g>
    </g>
  )";
  std::unique_ptr<SvgNode> proto(loadSVGFragment(rowProtoSVG));
  Button* row = new Button(proto->clone());
  row->node->addClass("tag-row");
  row->node->setAttribute("margin", "14 12 14 12");
  row->selectFirst(".title")->setText(title);
  SvgUse* icon = new SvgUse(Rect::wh(24, 24), "", SvgGui::useFile(iconPath));
  icon->addClass("icon");
  row->selectFirst(".icon-container")->containerNode()->addChild(icon);
  return row;
}

// Shrinks a toolbutton's ".background" rect (36x42 by default, see #toolbutton in theme.cpp) down to
// a tag row's own content height (~28px, icon/checkbox + text). setAttribute("height", ...) does not
// work for this: SvgRect caches its geometry in m_rect, separate from the generic XML attrs list that
// setAttribute() writes to (m_rect is only ever populated from width/height/x/y at parse time), so
// only SvgRect::setRect() actually moves it.
static void shrinkChevronButton(Button* chevron)
{
  SvgRect* bg = static_cast<SvgRect*>(chevron->selectFirst(".background")->node);
  Rect r = bg->getRect();
  bg->setRect(Rect::ltwh(r.left, r.top, r.width(), 28));
}

Widget* TagDocList::createTagRow(const std::string& tagId, int depth)
{
  const TagNode* tagNode = tagStore.tag(tagId);
  if(!tagNode)
    return NULL;

  static const char* rowProtoSVG = R"(
    <g class="listitem" layout="box" box-anchor="hfill">
      <rect box-anchor="fill" width="20" height="50"/>
      <g layout="flex" flex-direction="row" box-anchor="hfill">
        <g class="icon-container" margin="0 12 0 0"></g>
        <text class="title" box-anchor="left"></text>
        <g class="spacer" box-anchor="fill" layout="box"></g>
        <g class="chevron-container" layout="box" box-anchor="vfill" margin="0 0 0 12"></g>
      </g>
    </g>
  )";
  std::unique_ptr<SvgNode> proto(loadSVGFragment(rowProtoSVG));

  Button* row = new Button(proto->clone());
  row->node->addClass("tag-row");
  row->node->setAttribute("margin", fstring("6 %d 6 %d", TAG_ROW_RIGHT, TAG_ROW_LEFT + depth*TAG_ROW_INDENT).c_str());
  row->selectFirst(".title")->setText(tagNode->name.c_str());
  row->setChecked(activeTags.count(tagId) > 0);

  SvgUse* tagIcon = new SvgUse(Rect::wh(24, 24), "", SvgGui::useFile("icons/ic_tag.svg"));
  tagIcon->addClass("icon");
  row->selectFirst(".icon-container")->containerNode()->addChild(tagIcon);

  if(!tagNode->childIds.empty() && tagSearchQuery.empty()) {
    bool expanded = expandedTags.count(tagId) > 0;
    Button* chevron = createToolbutton(
        SvgGui::useFile(expanded ? "icons/chevron_up.svg" : "icons/ic_menu_expanddown.svg"), "");
    // The stock toolbutton chrome's own ".background" rect is 36x42 - a full toolbar hit target,
    // taller than this row's own content (~28px, icon+text) - and unlike the row's own sizing rect,
    // this one only carries box-anchor="hfill" (see #toolbutton in theme.cpp), so its declared height
    // *does* count towards the toolbutton's natural size (Widget::prepareLayout() only zeroes out the
    // dimension(s) that are actually "fill"). Left at that native 42px, the chevron was the tallest
    // thing in the row and dragged the *row's* own height up with it - rows with subtags rendered
    // visibly taller than rows without (measured: 34px row spacing against 27px for childless rows).
    // Shrinking its background rect to match the row's own content height fixes that at the source,
    // instead of growing every row (chevron or not) up to the toolbutton's size. setAttribute() alone
    // does not work here: SvgRect caches its geometry in m_rect, separate from the generic XML attrs
    // list that setAttribute() writes to, so only SvgRect::setRect() actually moves it.
    shrinkChevronButton(chevron);
    chevron->node->setAttribute("box-anchor", "vfill");
    chevron->onClicked = [this, tagId](){
      if(expandedTags.count(tagId))
        expandedTags.erase(tagId);
      else
        expandedTags.insert(tagId);
      rebuildTagTree();
    };
    row->selectFirst(".chevron-container")->addWidget(chevron);
  }

  row->onClicked = [this, tagId](){ toggleTagFilter(tagId); };
  SvgGui::setupRightClick(row, [this, tagId, row](SvgGui* gui, Widget* w, Point p){
    showTagMenu(tagId, row);
  });
  auto key = dragTagKeys.emplace(tagId, int(dragTagIds.size()));
  if(key.second)
    dragTagIds.push_back(tagId);
  tagDrag->addRow(row, key.first->second);
  return row;
}

bool TagDocList::isTagDescendant(const std::string& tagId, const std::string& ancestorId) const
{
  return tagStore.isDescendant(tagId, ancestorId);
}

// parentId empty = make it a root tag.  A tag's documents are untouched: they carry the tag's id, and
//  moving a tag in the tree changes nothing about which documents have it.
void TagDocList::moveTagUnder(const std::string& tagId, const std::string& parentId, const std::string& afterId)
{
  const TagNode* tagNode = tagStore.tag(tagId);
  if(!tagNode || tagNode->parentId == parentId || tagId == parentId || isTagDescendant(parentId, tagId))
    return;
  tagStore.reparentTag(tagId, parentId, afterId);
  // show the tag where it went rather than folding it away under a collapsed parent
  if(!parentId.empty())
    expandedTags.insert(parentId);
  tagStore.save();
  rebuildTagTree();
}

// A long tag name would otherwise push the chevron off the sidebar's edge, leaving a parent tag that
//  cannot be expanded. The row's width is fixed by the sidebar, so the title's share is known up front:
//  everything else in the row is subtracted and the name is ellipsized to fit what is left. Called
//  once the row is in the tree, since elideText() measures with the font the theme's CSS gives it.
//  The chevron's slot is reserved on every row so names line up whether or not a tag has children.
// Shortens text to fit maxWidth by cutting out its middle ("Semester 3...Analysis"), since tag names
//  that share a start - "Semester 3 Linear Algebra", "Semester 3 Analysis" - differ at the end. Assumes
//  one glyph per codepoint, as SvgPainter::elideText() does.
static void elideTextMiddle(SvgText* textnode, real maxWidth)
{
  SvgDocument* root = textnode->rootDocument();
  SvgPainter* bounder = root && root->boundsCalculator ? root->boundsCalculator : SvgDocument::sharedBoundsCalc;
  std::string text = textnode->text();
  textnode->addText("...");
  std::vector<GlyphPosition> glyphs = bounder->glyphPositions(textnode);
  textnode->clearText();
  std::u32string codepoints = std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t>().from_bytes(text);
  size_t count = codepoints.size();
  if(glyphs.size() != count + 3 || count == 0 || glyphs[count-1].right - glyphs[0].left <= maxWidth) {
    textnode->addText(text.c_str());
    return;
  }
  real ellipsisWidth = glyphs[count+2].right - glyphs[count].left;
  real budget = maxWidth - ellipsisWidth;
  // the head gets half the room, the tail whatever the head leaves
  size_t headLen = 0;
  while(headLen < count && glyphs[headLen].right - glyphs[0].left <= budget/2)
    ++headLen;
  real headWidth = headLen > 0 ? glyphs[headLen-1].right - glyphs[0].left : 0;
  size_t tailLen = 0;
  while(tailLen < count - headLen && glyphs[count-1].right - glyphs[count-1-tailLen].left <= budget - headWidth)
    ++tailLen;
  std::u32string head = codepoints.substr(0, headLen), tail = codepoints.substr(count - tailLen);
  // a space beside the ellipsis only widens the gap it already marks
  while(!head.empty() && head.back() == U' ') head.pop_back();
  while(!tail.empty() && tail.front() == U' ') tail.erase(0, 1);
  std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t> converter;
  textnode->addText((converter.to_bytes(head) + "..." + converter.to_bytes(tail)).c_str());
}

// A long tag name would otherwise push the chevron off the sidebar's edge, leaving a parent tag that
//  cannot be expanded. The row's width is fixed by the sidebar, so the title's share is known up front:
//  everything else in the row is subtracted and the name is shortened to fit what is left. Called
//  once the row is in the tree, since the measurement needs the font the theme's CSS gives it.
//  The chevron's slot is reserved on every row so names line up whether or not a tag has children.
void TagDocList::elideTagRowTitle(Widget* row, int depth)
{
  SvgText* title = static_cast<SvgText*>(row->containerNode()->selectFirst(".title"));
  if(!title)
    return;
  // CSS is otherwise applied lazily, at the next layout; unstyled, the title has no font to measure with
  row->node->restyle();
  real rowWidth = SIDEBAR_WIDTH - SIDEBAR_PAD_LEFT - SIDEBAR_PAD_RIGHT + TAG_LIST_BLEED_LEFT + TAG_LIST_BLEED_RIGHT
      - (TAG_ROW_LEFT + depth*TAG_ROW_INDENT) - TAG_ROW_RIGHT;
  // icon 24 + its 12 margin; chevron's 36 wide toolbutton + its 12 margin
  real maxWidth = rowWidth - (24 + 12) - (36 + 12);
  elideTextMiddle(title, std::max(maxWidth, real(40)));
}

void TagDocList::rebuildTagTree()
{
  // tagContextPopup is reparented onto whichever row triggered it (see showTagMenu()'s comment) and
  // stays there even while closed - deleteContents() below would otherwise destroy the popup along
  // with the row still holding it, which is exactly what crashed on "Add Subtag": that item's own
  // callback calls this function, while the popup showing the item is still mid-click and its host
  // row about to be torn out from under it. Detaching first makes the popup parentless (safe - the
  // next show() call reparents it again) instead of destroyed.
  closeAutoClosePopup(tagContextPopup);
  tagContextPopup->removeFromParent();
  tagDrag->clear();
  if(gui())
    gui()->deleteContents(tagTreeView, ".listitem");

  std::function<void(const std::string&, int)> appendRecursive = [&](const std::string& id, int depth){
    Widget* row = createTagRow(id, depth);
    if(!row)
      return;
    tagTreeView->addWidget(row);
    elideTagRowTitle(row, depth);
    const TagNode* tagNode = tagStore.tag(id);
    if(tagNode && expandedTags.count(id))
      for(const std::string& childId : tagNode->childIds)
        appendRecursive(childId, depth + 1);
  };

  if(tagSearchQuery.empty()) {
    for(const std::string& id : tagStore.rootTagIds())
      appendRecursive(id, 0);
  }
  else {
    std::string query = toLower(tagSearchQuery);
    for(const auto& pair : tagStore.allTags()) {
      if(toLower(pair.second.name).find(query) != std::string::npos) {
        Widget* row = createTagRow(pair.first, 0);
        if(row) {
          tagTreeView->addWidget(row);
          elideTagRowTitle(row, 0);
        }
      }
    }
  }
}

std::vector<TagDocList::DocEntry> TagDocList::collectDocuments()
{
  std::vector<DocEntry> docs;
  std::vector<FSPath> stack = {docRoot};
  while(!stack.empty()) {
    FSPath dir = stack.back();
    stack.pop_back();
    for(const std::string& file : lsDirectory(dir)) {
      if(file.empty() || file.front() == '.')
        continue;
      FSPath info = dir.child(file);
      if(info.isDir()) {
        stack.push_back(info);
        continue;
      }
      if(info.extension() == "svg" && StringRef(info.baseName()).chop(3).endsWith("_page"))
        continue;
      if(!containsWord(fileExts.c_str(), info.extension().c_str()))
        continue;

      time_t mtime = (time_t)getFileMTime(info);
      std::vector<std::string> tagIds;
      std::string pageTags;
      if(!tagStore.cachedDocTags(info.c_str(), mtime, &tagIds, &pageTags)) {
        tagIds = ScribbleDoc::extractDocTags(info.c_str());
        pageTags = ScribbleDoc::extractDocConfigValue(info.c_str(), "pagetags");
        tagStore.setDocTags(info.c_str(), mtime, tagIds, pageTags);
      }
      docs.push_back({info, tagIds, TagStore::parsePageTags(pageTags.c_str())});
    }
  }
  return docs;
}

std::vector<FSPath> TagDocList::docsWithPageTag(const std::vector<std::string>& tagIds)
{
  std::vector<FSPath> tagged;
  for(const DocEntry& doc : collectDocuments()) {
    bool found = std::any_of(doc.pageTags.begin(), doc.pageTags.end(), [&](const TagStore::PageTags& page){
      return std::any_of(page.tagIds.begin(), page.tagIds.end(), [&](const std::string& id){
        return std::find(tagIds.begin(), tagIds.end(), id) != tagIds.end();
      });
    });
    if(found)
      tagged.push_back(doc.path);
  }
  return tagged;
}

static Widget* createDocRowGroup()
{
  Widget* group = new Widget(new SvgG());
  group->node->addClass("doc-row-group");
  group->node->setAttribute("box-anchor", "hfill");
  group->node->setAttribute("layout", "flex");
  group->node->setAttribute("flex-direction", "row");
  group->node->setAttribute("flex-wrap", "wrap");
  group->node->setAttribute("justify-content", "flex-start");
  return group;
}

// width of a text node's laid-out text; the node must already be in the window's document, or its font
//  is not resolved (the same bounds calculator elideText() uses)
static real laidOutTextWidth(SvgText* textnode)
{
  SvgDocument* root = textnode->rootDocument();
  SvgPainter* bounder = root && root->boundsCalculator ? root->boundsCalculator : SvgDocument::sharedBoundsCalc;
  std::vector<GlyphPosition> glyphs = bounder->glyphPositions(textnode);
  return glyphs.empty() ? 0 : glyphs.back().right;
}

// The notebook's own tags under its title, so two notebooks with near-identical names can be told apart:
//  a tag glyph and the names on one line, as many as fit in maxWidth, the rest as "+N". Builds nothing for
//  an untagged notebook.  `column` must be in the document already (text is measured).
static void addDocTagsLine(SvgContainerNode* column, const std::vector<std::string>& tagNames, real maxWidth)
{
  if(tagNames.empty())
    return;
  static const real glyphSize = 12, glyphGap = 4;
  SvgG* line = new SvgG();
  line->addClass("doc-cell-tags");
  line->setAttribute("layout", "flex");
  line->setAttribute("flex-direction", "row");
  line->setAttribute("margin", "0 0 4 0");
  SvgUse* glyph = new SvgUse(Rect::wh(glyphSize, glyphSize), "", SvgGui::useFile("icons/ic_tag.svg"));
  glyph->addClass("icon");
  glyph->setAttribute("margin", fstring("0 %g 0 0", glyphGap).c_str());
  line->addChild(glyph);
  SvgText* textnode = createTextNode("");
  textnode->addClass("weak");  // text.weak: var(--text-weak), the subdued text color of the theme
  textnode->setAttribute("font-size", "12");
  line->addChild(textnode);
  column->addChild(line);

  real textWidth = maxWidth - glyphSize - glyphGap;
  auto summary = [&](size_t shown){
    std::string text = joinStr(std::vector<std::string>(tagNames.begin(), tagNames.begin() + shown), ", ");
    if(shown < tagNames.size())
      text += fstring("  +%d", int(tagNames.size() - shown));
    return text;
  };
  // the most leading names that fit whole, then the count of the rest
  for(size_t shown = tagNames.size(); shown > 0; --shown) {
    textnode->setText(summary(shown).c_str());
    if(laidOutTextWidth(textnode) <= textWidth)
      return;
  }
  // not even the first name fits: cut it short, keeping room for the count
  std::string countText = tagNames.size() > 1 ? fstring("  +%d", int(tagNames.size() - 1)) : "";
  textnode->setText(countText.c_str());
  real countWidth = countText.empty() ? 0 : laidOutTextWidth(textnode);
  textnode->setText(tagNames.front().c_str());
  SvgPainter::elideText(textnode, textWidth - countWidth);
  if(!countText.empty())
    textnode->addText(countText.c_str());
}

void TagDocList::rebuildDocGrid()
{
  // Same reasoning as rebuildTagTree()'s detach: docContextPopup/docTagsPopup are reparented onto
  // whichever cell triggered them and would otherwise be destroyed along with it here.
  closeAutoClosePopup(docContextPopup);
  docContextPopup->removeFromParent();
  closeAutoClosePopup(docTagsPopup);
  docTagsPopup->removeFromParent();
  if(gui())
    gui()->deleteContents(docGrid);
  docCells.clear();
  shownDocs.clear();

  // the frame every notebook's preview is drawn in (cover.h) - the cell is exactly that tall
  Rect iconSize = Rect::wh(iconWidth, std::round(iconWidth*Cover::PREVIEW_ASPECT));
  Rect symbolSize = Rect::wh(iconSize.width()/2, iconSize.height()/2);
  fileUseNode->setViewport(symbolSize);

  std::string query = toLower(docSearchQuery);
  std::vector<DocEntry> allDocs;
  // the partial matches, each with how it met the filter, for ordering them (partialMatchBefore)
  std::vector<std::pair<TagStore::FilterMatch, DocEntry>> someDocs;
  // a tagged page, shown as its own card beside (not instead of) its notebook
  // matched: the page's own tags that the filter picked it for - shown blinking when it opens
  struct PageCard { FSPath path; TagStore::PageTags page; std::vector<std::string> matched; };
  std::vector<PageCard> allPages;
  std::vector<std::pair<TagStore::FilterMatch, PageCard>> somePages;
  // A tag also matches documents carrying any of its subtags. Only here, not in the tree: selecting
  //  a supertag does not check its subtags. So with a supertag and one of its subtags both active,
  //  documents with that subtag satisfy both and sort above those with only a sibling subtag.
  auto carries = [this](const std::vector<std::string>& ids, const std::string& t){
    return std::any_of(ids.begin(), ids.end(), [&](const std::string& id){
      return id == t || isTagDescendant(id, t);
    });
  };
  for(const DocEntry& doc : collectDocuments()) {
    if(!query.empty() && toLower(doc.path.baseName()).find(query) == std::string::npos)
      continue;
    if(activeTags.empty()) {
      allDocs.push_back(doc);  // pages are only listed when filtering by a tag
      continue;
    }
    TagStore::FilterMatch docMatch = tagStore.matchFilter(doc.tagIds, activeTags);
    if(docMatch.matched == activeTags.size())
      allDocs.push_back(doc);
    else if(docMatch.matched > 0)
      someDocs.emplace_back(docMatch, doc);

    // A page inherits its notebook's tags, so #math + #homework finds the homework page in a math
    //  notebook.  It is listed only for a tag of its own, though - otherwise filtering by #math would
    //  list every tagged page of every math notebook.
    for(const TagStore::PageTags& page : doc.pageTags) {
      if(!std::any_of(activeTags.begin(), activeTags.end(), [&](const std::string& t){ return carries(page.tagIds, t); }))
        continue;
      // the page's own tags plus the ones it inherits from its notebook
      std::vector<std::string> pageAndDocTags = page.tagIds;
      pageAndDocTags.insert(pageAndDocTags.end(), doc.tagIds.begin(), doc.tagIds.end());
      TagStore::FilterMatch pageMatch = tagStore.matchFilter(pageAndDocTags, activeTags);
      std::vector<std::string> matched;
      for(const std::string& id : page.tagIds) {
        if(std::any_of(activeTags.begin(), activeTags.end(), [&](const std::string& t){ return carries({id}, t); }))
          matched.push_back(id);
      }
      if(pageMatch.matched == activeTags.size())
        allPages.push_back({doc.path, page, matched});
      else
        somePages.emplace_back(pageMatch, PageCard{doc.path, page, matched});
    }
  }
  // below the separator, a document carrying a filter tag itself before one that only reaches it
  //  through a subtag (an exact match before a top-level one); stable, so equals keep folder order
  auto partialBefore = [](const auto& lhs, const auto& rhs){
    return TagStore::partialMatchBefore(lhs.first, rhs.first);
  };
  std::stable_sort(someDocs.begin(), someDocs.end(), partialBefore);
  std::stable_sort(somePages.begin(), somePages.end(), partialBefore);

  auto addCell = [this, &iconSize, &symbolSize](Widget* group, const DocEntry& doc){
    Button* item = new Button(gridItemProto->clone());
    item->node->addClass("doc-cell");
    std::string pathStr = doc.path.c_str();
    item->onClicked = [this, doc, pathStr](){
      if(selectMode) {
        toggleDocSelected(pathStr);
        return;
      }
      selectedFile = pathStr;
      selectedPage = -1;
      finish(EXISTING_DOC);
    };
    SvgGui::setupRightClick(item, [this, doc, item](SvgGui* gui, Widget* w, Point p){
      if(!selectMode)
        showDocMenu(doc.path, item);
    });
    docCells.emplace(pathStr, item);
    if(std::find(shownDocs.begin(), shownDocs.end(), pathStr) == shownDocs.end())
      shownDocs.push_back(pathStr);
    item->setChecked(selectedDocs.count(pathStr) > 0);

    SvgContainerNode* container = item->selectFirst(".image-container")->containerNode();
    real itemWidth = iconSize.width();
    bool isNativeFormat = containsWord("svg svgz html htm", doc.path.extension().c_str());
    // a notebook with a cover shows its cover rather than its first page (CreateNotebookDialog, cover.h)
    int coverArgb = isNativeFormat
        ? atoi(ScribbleDoc::extractDocConfigValue(doc.path.c_str(), "coverColor").c_str()) : 0;
    Image thumbnail = !coverArgb && isNativeFormat && ScribbleApp::cfg->Bool("showThumbnail")
        ? ScribbleDoc::extractThumbnail(doc.path.c_str()) : Image(0, 0);
    // spacer is the full preview size (not the smaller icon size) so an icon-only cell takes up
    // exactly as much room as a thumbnail cell would - the glyph itself stays small and centered
    SvgRect* spacer = new SvgRect(iconSize);
    spacer->setAttribute("fill", "none");
    container->addChild(spacer);
    // a cover and a thumbnail fill the same frame, so a notebook with a cover is not a different shape
    if(coverArgb)
      container->addChild(loadSVGFragment(Cover::coverSVG(Color::fromArgb(coverArgb),
          iconSize.width(), iconSize.height()).c_str()));
    else if(!thumbnail.isNull())
      container->addChild(createFramedThumbnail(std::move(thumbnail), iconSize, true));
    else
      container->addChild(fileUseNode->clone());
    if(selectMode) {
      // a ring around the preview and a check badge in its corner; the badge is an empty circle until
      //  the cell is checked, so every selectable cell shows it can be picked (theme.cpp)
      SvgRect* ring = new SvgRect(Rect::wh(20, 20), 6, 6);
      ring->addClass("select-ring");
      ring->setAttribute("box-anchor", "fill");
      container->addChild(ring);
      SvgG* badge = new SvgG();
      badge->addClass("select-badge");
      badge->setAttribute("box-anchor", "top right");
      badge->setAttribute("layout", "box");
      badge->setAttribute("margin", "8 8 0 0");
      SvgRect* badgeBg = new SvgRect(Rect::wh(24, 24), 12, 12);
      badgeBg->addClass("select-badge-bg");
      badge->addChild(badgeBg);
      SvgUse* check = new SvgUse(Rect::wh(16, 16), "", SvgGui::useFile("icons/ic_menu_accept.svg"));
      check->addClass("icon");
      check->addClass("select-check");
      badge->addChild(check);
      container->addChild(badge);
    }

    group->addWidget(item);
    std::string name = doc.path.extension() == docFileExt ? doc.path.baseName() : doc.path.fileName();
    SvgText* textnode = static_cast<SvgText*>(item->containerNode()->selectFirst(".title-text"));
    textnode->addText(name.c_str());
    SvgPainter::elideText(textnode, itemWidth);

    // from the tag cache collectDocuments() already filled - never a file read per cell.  Tags not being
    //  filtered on come first: every notebook shown carries the active ones, so they tell nothing apart.
    std::vector<std::string> tagNames, filteredNames;
    for(const std::string& id : doc.tagIds) {
      const TagNode* tagNode = tagStore.tag(id);
      if(!tagNode)
        continue;
      bool filtered = activeTags.count(id) > 0;
      (filtered ? filteredNames : tagNames).push_back(tagNode->name);
    }
    tagNames.insert(tagNames.end(), filteredNames.begin(), filteredNames.end());
    addDocTagsLine(textnode->parent()->asContainerNode(), tagNames, itemWidth);
  };

  // the page's preview, its name (the outline title, if it has one), and which notebook it is in
  auto addPageCell = [this, &iconSize](Widget* group, const PageCard& card){
    Button* item = new Button(gridItemProto->clone());
    item->node->addClass("doc-cell");
    item->node->addClass("page-cell");
    int pagenum = card.page.page;
    // not selectable (see selectMode), so dimmed while selecting rather than opening on a stray tap
    if(selectMode)
      item->node->setAttribute("opacity", "0.35");
    item->onClicked = [this, card, pagenum](){
      if(selectMode)
        return;
      selectedFile = card.path.c_str();
      selectedPage = pagenum;
      selectedPageTags = card.matched;
      finish(EXISTING_DOC);
    };

    SvgContainerNode* container = item->selectFirst(".image-container")->containerNode();
    real itemWidth = iconSize.width();
    Image thumbnail = ScribbleDoc::extractPageThumbnail(card.path.c_str(), pagenum);
    if(!thumbnail.isNull()) {
      // the page's own proportions, fitted inside the cell a notebook's thumbnail takes
      real scale = std::min(iconSize.width()/thumbnail.width, iconSize.height()/thumbnail.height);
      Rect imageRect = Rect::wh(thumbnail.width*scale, thumbnail.height*scale);
      SvgRect* spacer = new SvgRect(iconSize);
      spacer->setAttribute("fill", "none");
      container->addChild(spacer);
      // the frame's corners and outline, at the page's own shape
      SvgNode* image = createFramedThumbnail(std::move(thumbnail), imageRect, false);
      image->setAttribute("box-anchor", "bottom");
      container->addChild(image);
    }
    else {
      SvgRect* spacer = new SvgRect(iconSize);
      spacer->setAttribute("fill", "none");
      container->addChild(spacer);
      container->addChild(fileUseNode->clone());
    }

    group->addWidget(item);
    std::string pageLabel = fstring(_("Page %d"), pagenum + 1);
    std::string notebook = card.path.extension() == docFileExt ? card.path.baseName() : card.path.fileName();
    SvgText* textnode = static_cast<SvgText*>(item->containerNode()->selectFirst(".title-text"));
    textnode->addText(card.page.title.empty() ? pageLabel.c_str() : card.page.title.c_str());
    SvgPainter::elideText(textnode, itemWidth);
    SvgText* subnode = createTextNode((card.page.title.empty() ? fstring(_("in %s"), notebook.c_str())
        : fstring(_("%s of %s"), pageLabel.c_str(), notebook.c_str())).c_str());
    subnode->addClass("page-cell-notebook");
    subnode->setAttribute("font-size", "12");
    subnode->setAttribute("margin", "0 0 4 0");
    subnode->setAttribute("opacity", "0.7");
    textnode->parent()->asContainerNode()->addChild(subnode);
    SvgPainter::elideText(subnode, itemWidth);
  };

  // a small caption over each group when the grid is split (two or more tags active, see below)
  auto addGroupLabel = [this](const char* text){
    Widget* label = new Widget(createTextNode(text));
    label->node->addClass("doc-group-label");
    label->node->addClass("weak");
    label->node->setAttribute("box-anchor", "left");
    label->node->setAttribute("font-size", "12");
    label->setMargins(8, 0, 4, 8);
    docGrid->addWidget(label);
  };
  bool splitGrid = !someDocs.empty() || !somePages.empty();
  if(splitGrid && (!allDocs.empty() || !allPages.empty()))
    addGroupLabel(_("Matches all selected tags"));

  Widget* allGroup = createDocRowGroup();
  docGrid->addWidget(allGroup);
  for(const DocEntry& doc : allDocs)
    addCell(allGroup, doc);
  for(const PageCard& card : allPages)
    addPageCell(allGroup, card);

  // Multi-select with 2+ tags active: documents matching every active tag are shown above this
  // separator, documents matching only some of them below it - see the header comment on
  // multiSelectMode for why this beats a strict all-or-nothing AND filter.
  if(splitGrid) {
    docGrid->addWidget(createHRule());
    addGroupLabel(_("Matches some selected tags"));
    Widget* someGroup = createDocRowGroup();
    docGrid->addWidget(someGroup);
    for(const auto& entry : someDocs)
      addCell(someGroup, entry.second);
    for(const auto& entry : somePages)
      addPageCell(someGroup, entry.second);
  }

  // documents the search or tag filter now hides drop out of the selection (see selectMode)
  std::set<std::string> stillShown;
  for(const std::string& path : selectedDocs) {
    if(docCells.count(path))
      stillShown.insert(path);
  }
  selectedDocs.swap(stillShown);
  updateSelectBar();
}

void TagDocList::setRoot(const char* root)
{
  docRoot = root;
  tagStore = TagStore(docRoot.child(".write-tags").c_str());
  tagStore.load();
  // tag ids are per root, so a filter from the old root would match nothing here
  activeTags.clear();
  expandedTags.clear();
  refresh();
}

void TagDocList::refresh()
{
  rebuildTagTree();
  rebuildDocGrid();
}

void TagDocList::toggleTagFilter(const std::string& tagId)
{
  if(multiSelectMode) {
    if(activeTags.count(tagId))
      activeTags.erase(tagId);
    else
      activeTags.insert(tagId);
  }
  else {
    bool wasActive = activeTags.count(tagId) > 0;
    activeTags.clear();
    if(!wasActive)
      activeTags.insert(tagId);
  }
  refresh();
}

void TagDocList::toggleMultiSelect()
{
  multiSelectMode = !multiSelectMode;
  multiSelectBtn->setChecked(multiSelectMode);
  if(!multiSelectMode && activeTags.size() > 1) {
    // dropping back to single-select with several tags active would otherwise leave a filter state
    // that single-select's own click handling (replace, not accumulate) could never have produced
    std::string keep = *activeTags.begin();
    activeTags.clear();
    activeTags.insert(keep);
  }
  refresh();
}

// Right-click/long-press menus in this app are ArrowPopups (see pentoolbar.cpp's palettePopup for the
// canonical example), never plain Menus with showContextMenu() - that's the established convention
// here, not a style choice made for this file. The practical difference: a Menu positions itself at a
// click *point* (gui()->showContextMenu(menu, pos)), but ArrowPopup has no point-based anchoring at
// all - ArrowPopup::calcOffset() computes its position purely from its own parent widget's bounds and
// points its arrow at that widget, so "anchoring a context popup near a row" means making the row
// itself the popup's parent, not passing coordinates.
// One popup instance is built once in createUI() (matching DocumentList's persistent contextMenu -
// building a fresh Menu per click crashed here previously, see git history) and reparented onto
// whichever row/cell triggered it immediately before each show: removeFromParent() first (Widget::
// addWidget() asserts a widget has no existing parent), then row->addWidget(popup), then
// openAutoClosePopup(popup) rather than popup->setVisible(true) directly - see setupAutoClosePopup()'s
// own comment in ugui/widgets.cpp for why the menu-stack path is required for outside-click/Escape to
// work. Unlike the old point-based version, it's safe to use the row widget directly here since it's
// only read synchronously within this call, never stored past it.
void TagDocList::showTagMenu(const std::string& tagId, Widget* row)
{
  contextMenuTagId = tagId;
  tagContextPopup->removeFromParent();
  row->addWidget(tagContextPopup);
  openAutoClosePopup(tagContextPopup);
}

// Opening a modal window does not proactively close a still-open ArrowPopup - Window::showWindow()
// only sends OUTSIDE_MODAL reactively, on the *next* pointer event that lands outside the modal (see
// SvgGui::sendEvent's `if(modalWidget && ...)` branch), never at the moment the modal itself appears.
// So a popup left open behind a freshly-opened dialog is still fully live - including still holding
// keyboard focus routing for one more event - and the dialog's very first keystroke was landing there
// instead of the new TextEdit, silently dropping it ("urgent" -> "rgent"). Every place that opens a
// TagNameDialog or messageBox from a context that might have a popup open calls this first.
void TagDocList::closeAllContextPopups()
{
  closeAutoClosePopup(tagContextPopup);
  closeAutoClosePopup(docContextPopup);
  closeAutoClosePopup(docTagsPopup);
}

void TagDocList::addTag(const std::string& parentId)
{
  closeAllContextPopups();
  TagNameDialog dialog(parentId.empty() ? _("New Tag") : _("New Subtag"), "");
  int res = Application::execDialog(&dialog);
  std::string name = dialog.getName();
  if(res != Dialog::ACCEPTED || name.empty())
    return;
  tagStore.addTag(name, parentId);
  if(!parentId.empty())
    expandedTags.insert(parentId);
  tagStore.save();
  rebuildTagTree();
}

void TagDocList::renameTag(const std::string& tagId)
{
  const TagNode* tagNode = tagStore.tag(tagId);
  if(!tagNode)
    return;
  closeAllContextPopups();
  TagNameDialog dialog(_("Rename Tag"), tagNode->name.c_str());
  int res = Application::execDialog(&dialog);
  std::string name = dialog.getName();
  if(res != Dialog::ACCEPTED || name.empty())
    return;
  tagStore.renameTag(tagId, name);
  tagStore.save();
  // a page's tag shows its name, so pages carrying it are relabelled; a document's own tag is shown
  //  only from the store, so it needs nothing
  std::vector<FSPath> tagged = docsWithPageTag({tagId});
  for(const FSPath& path : tagged) {
    rewriteDocument(path, [&](Document& doc, ScribbleConfig&){
      ScribbleDoc::renamePageTagElements(&doc, tagId, name);
    });
  }
  if(tagged.empty())
    rebuildTagTree();
  else
    refresh();
}

// Deletion only ever touches the TagStore's tree (and its cache) -- never the tag lists a document
// stores in its own config, except to add the supertag when the user asked for that (retagDocs). That's deliberate, not an oversight: it's what makes undo trivial (the
// document side never changes, so there's nothing to put back there) and it costs nothing, since a
// document referencing a since-deleted tag id simply stops matching anything in the tree - the id
// becomes inert rather than dangling. If the tag is undone, it's exactly as if it was never deleted.
// Documents carrying tagId (or, with subtree, any tag below it too) and not already carrying its parent,
//  i.e. those that would drop out of the parent's listing if the tag were deleted.
std::vector<FSPath> TagDocList::docsWithTag(const std::string& tagId, bool subtree)
{
  const TagNode* tagNode = tagStore.tag(tagId);
  std::vector<FSPath> taggedDocs;
  if(!tagNode)
    return taggedDocs;
  std::string parentId = tagNode->parentId;
  for(const DocEntry& doc : collectDocuments()) {
    const auto& ids = doc.tagIds;
    if(std::find(ids.begin(), ids.end(), parentId) != ids.end())
      continue;
    if(std::any_of(ids.begin(), ids.end(), [&](const std::string& id){
        return id == tagId || (subtree && isTagDescendant(id, tagId)); }))
      taggedDocs.push_back(doc.path);
  }
  return taggedDocs;
}

void TagDocList::deleteTagWithUndo(const std::string& tagId, bool deleteChildren,
    const std::vector<FSPath>& retagDocs)
{
  const TagNode* nodePtr = tagStore.tag(tagId);
  if(!nodePtr)
    return;
  TagNode tagNode = *nodePtr;  // copy before mutating the store

  // the one case where deletion does write documents: they take the parent tag, which undo takes back
  //  off; the deleted id itself stays in them, inert, as below
  for(const FSPath& path : retagDocs) {
    std::vector<std::string> tags = ScribbleDoc::extractDocTags(path.c_str());
    tags.push_back(tagNode.parentId);
    setDocumentTags(path, tags);
  }

  DeleteSnapshot snapshot;
  snapshot.label = tagNode.name;
  snapshot.parentId = tagNode.parentId;
  snapshot.parentAddedTo = retagDocs;
  if(deleteChildren) {
    std::vector<std::string> queue = {tagId};
    while(!queue.empty()) {
      std::string id = queue.front();
      queue.erase(queue.begin());
      const TagNode* n = tagStore.tag(id);
      if(!n)
        continue;
      snapshot.removedInOrder.push_back(*n);
      for(const std::string& childId : n->childIds)
        queue.push_back(childId);
    }
  }
  else {
    snapshot.removedInOrder.push_back(tagNode);
    snapshot.reparentedBack = tagNode.childIds;
  }

  // Unlike a document's own tags, a page's tags are taken off the page: they are visible content, and
  //  one left behind would show a name no tag has any more.  Nor do they fall back to the supertag - a
  //  page tag is a mark on one page, and a broader one is not what was put there.  Undo restores the
  //  files as they were, so each is kept before it is rewritten.
  std::vector<std::string> removedIds;
  for(const TagNode& removed : snapshot.removedInOrder)
    removedIds.push_back(removed.id);
  for(const FSPath& path : docsWithPageTag(removedIds)) {
    DeleteSnapshot::FileBackup backup;
    backup.path = path;
    if(!readFile(&backup.contents, path.c_str()))
      continue;
    if(rewriteDocument(path, [&](Document& doc, ScribbleConfig&){
        ScribbleDoc::removePageTagElements(&doc, removedIds); })) {
      backup.mtime = (time_t)getFileMTime(path);
      snapshot.pageTagFiles.push_back(std::move(backup));
    }
  }

  tagStore.deleteTag(tagId, deleteChildren);
  activeTags.erase(tagId);
  tagStore.save();

  lastDelete = snapshot;
  lastDelete.valid = true;
  undoButton->setEnabled(true);
  setupTooltip(undoButton, fstring(_("Undo delete \"%s\""), snapshot.label.c_str()).c_str());
  refresh();
}

void TagDocList::undoTagDelete()
{
  if(!lastDelete.valid)
    return;
  for(const FSPath& path : lastDelete.parentAddedTo) {
    std::vector<std::string> tags = ScribbleDoc::extractDocTags(path.c_str());
    tags.erase(std::remove(tags.begin(), tags.end(), lastDelete.parentId), tags.end());
    setDocumentTags(path, tags);
  }
  // a file edited since the delete keeps its edits, and so stays without the page tags
  for(const DeleteSnapshot::FileBackup& backup : lastDelete.pageTagFiles) {
    if((time_t)getFileMTime(backup.path) != backup.mtime)
      continue;
    FileStream file(backup.path.c_str(), "wb");
    if(file.is_open())
      file.write(backup.contents.data(), backup.contents.size());
    file.close();
    tagStore.removeDoc(backup.path.c_str());  // reread on the refresh below
  }
  for(const TagNode& tagNode : lastDelete.removedInOrder)
    tagStore.restoreTag(tagNode);
  if(!lastDelete.removedInOrder.empty()) {
    const std::string& restoredId = lastDelete.removedInOrder[0].id;
    for(const std::string& childId : lastDelete.reparentedBack)
      tagStore.reparentTag(childId, restoredId);
  }
  tagStore.save();
  hideUndo();
  refresh();
}

void TagDocList::hideUndo()
{
  lastDelete = DeleteSnapshot();
  undoButton->setEnabled(false);
}

void TagDocList::newDoc()
{
  time_t rawtime;
  struct tm* timeinfo;
  char buffer[80];
  time(&rawtime);
  timeinfo = localtime(&rawtime);
  strftime(buffer, 80, ScribbleApp::cfg->String("newDocTitleFmt", "%b %e %Hh%M"), timeinfo);

  FSPath docinfo = docRoot.child(&buffer[0] + ("." + docFileExt));
  for(int ii = 2; docinfo.exists(); ii++)
    docinfo = docRoot.child(fstring(_("New Document %d.%s"), ii, docFileExt.c_str()));

  // the tags being browsed are the likeliest ones for a notebook made from here
  std::vector<std::string> initialTags(activeTags.begin(), activeTags.end());
  CreateNotebookDialog dialog(docinfo.baseName().c_str(), &tagStore, initialTags);
  int res = Application::execDialog(&dialog);
  std::string name = dialog.name();
  // the dialog may have made tags even if it was then cancelled
  rebuildTagTree();
  if(res != Dialog::ACCEPTED || name.empty())
    return;
  docinfo = docRoot.child(name + "." + docFileExt);
  if(!FSPath(docinfo.c_str()).exists("wb"))
    return;
  newDocChoices.coverColor = dialog.hasCover() ? dialog.coverColor() : Color(0);
  newDocChoices.layout = dialog.pageLayout();
  newDocChoices.tagIds = dialog.tagIds();
  selectedFile = docinfo.c_str();
  finish(NEW_DOC);
}

// Document long-press menu: Manage Tags / Change Cover / Rename / Delete - see showTagMenu()'s comment
// for why this is an ArrowPopup reparented onto the cell rather than a Menu shown at a point.
void TagDocList::showDocMenu(const FSPath& path, Widget* cell)
{
  contextMenuDocPath = path;
  docContextPopup->removeFromParent();
  cell->addWidget(docContextPopup);
  openAutoClosePopup(docContextPopup);
}

void TagDocList::showDocTagsPopup(const FSPath& path, Widget* cell)
{
  contextMenuDocPath = path;
  docTagsSearchQuery.clear();
  docTagsSearchEdit->setText("");
  // attach to the cell *before* rebuilding: rebuildDocGrid() (every reopen of the browser, every
  // search keystroke) leaves docTagsPopup parentless with the last document's rows still inside, and
  // rebuildDocTagsList() deleting rows of a detached popup crashed - see its comment
  docTagsPopup->removeFromParent();
  cell->addWidget(docTagsPopup);
  rebuildDocTagsList();
  openAutoClosePopup(docTagsPopup);
}

// A checkbox row for one tag in the Manage Tags popup, mirroring createTagRow()'s tree chrome
// (indentation by depth, a chevron for any tag with children) but with a checkbox in place of the
// plain click-to-filter row - this list assigns tags rather than filtering by them.
Widget* TagDocList::createDocTagRow(const std::string& tagId, int depth,
    const std::vector<std::string>& currentTags)
{
  const TagNode* tagNode = tagStore.tag(tagId);
  if(!tagNode)
    return NULL;

  // Same rowProtoSVG shape as createTagRow(); see that function's comment on the chevron for why its
  // background/checkmark rects get shrunk below instead of left at the stock toolbutton's 36x42 - a
  // row's own height comes from its tallest actual child, so an unshrunk chevron would make a row
  // with subtags visibly taller than a row without one.
  static const char* rowProtoSVG = R"(
    <g class="listitem" layout="box" box-anchor="hfill">
      <rect box-anchor="fill" width="20" height="50"/>
      <g layout="flex" flex-direction="row" box-anchor="hfill">
        <g class="checkbox-container" box-anchor="vfill" layout="box"></g>
        <g class="spacer" box-anchor="fill" layout="box"></g>
        <g class="chevron-container" layout="box" box-anchor="vfill" margin="0 0 0 12"></g>
      </g>
    </g>
  )";
  std::unique_ptr<SvgNode> proto(loadSVGFragment(rowProtoSVG));
  Widget* row = new Widget(proto->clone());
  row->node->addClass("doctag-row");
  row->node->setAttribute("margin", fstring("6 12 6 %d", 12 + depth*20).c_str());

  bool checked = std::find(currentTags.begin(), currentTags.end(), tagId) != currentTags.end();
  CheckBox* checkbox = createCheckBox(tagNode->name.c_str(), checked);
  checkbox->node->setAttribute("box-anchor", "left");
  std::string id = tagId;
  checkbox->onToggled = [this, id](bool on){
    std::vector<std::string> tags = ScribbleDoc::extractDocTags(contextMenuDocPath.c_str());
    if(on) {
      if(std::find(tags.begin(), tags.end(), id) == tags.end())
        tags.push_back(id);
    }
    else
      tags.erase(std::remove(tags.begin(), tags.end(), id), tags.end());
    setDocumentTags(contextMenuDocPath, tags);
    // deliberately not rebuildDocGrid(): that destroys every cell (see its own comment on why it
    // must close/detach docTagsPopup first) including the one this popup is currently open on,
    // which would slam a "multi-select, Done to close" popup shut on the first checkbox click. A
    // stale active tag-filter count is an acceptable trade-off until the next real refresh().
  };
  row->selectFirst(".checkbox-container")->addWidget(checkbox);

  if(!tagNode->childIds.empty() && docTagsSearchQuery.empty()) {
    bool expanded = docTagsExpandedTags.count(tagId) > 0;
    Button* chevron = createToolbutton(
        SvgGui::useFile(expanded ? "icons/chevron_up.svg" : "icons/ic_menu_expanddown.svg"), "");
    // see createTagRow()'s comment on why this gets shrunk instead of left at the toolbutton default
    shrinkChevronButton(chevron);
    chevron->node->setAttribute("box-anchor", "vfill");
    chevron->onClicked = [this, tagId](){
      if(docTagsExpandedTags.count(tagId))
        docTagsExpandedTags.erase(tagId);
      else
        docTagsExpandedTags.insert(tagId);
      rebuildDocTagsList();
    };
    row->selectFirst(".chevron-container")->addWidget(chevron);
  }
  return row;
}

// A tree of checkboxes, one per tag, mirroring rebuildTagTree()'s structure: nested under their
// parents and collapsible while docTagsSearchQuery is empty, flattened to matches (depth 0, no
// chevrons) once a search is typed - same as the sidebar's own tag tree. Checked against
// contextMenuDocPath's own current tags. Toggling a checkbox writes straight through to the
// document - see setDocumentTags()'s comment for why there's no separate "apply" step - so the
// popup's own Done button only ever needs to close it, never commit anything.
// The popup has no ancestor to hand it real screen bounds the way the sidebar's own tag tree does
// (see rebuildTagTree() and its ScrollWidget below in createUI()), so unlike that tree, this list
// cannot simply grow and let a ScrollWidget clip it - a popup taller than the window pushed its own
// Done button and search box off-screen and broke hit-testing for outside-click-to-close, which read
// as a crash once a project grew past a handful of tags (every click then landed "outside" a popup
// whose own controls were no longer reachable). Capping the row count and pointing at search instead
// keeps the popup's own height bounded and predictable without needing a scrollable sub-region here.
static const int DOC_TAGS_MAX_ROWS = 10;

void TagDocList::rebuildDocTagsList()
{
  // SvgGui::deleteWidget() -> onHideWidget() dereferences widget->window() unconditionally, and a row
  // of a detached docTagsPopup (rebuildDocGrid() detaches it) has no window: build 121 crashed on the
  // iPad at null+0x78 that way, on Manage Tags after the browser had been reopened. A detached popup
  // is closed and owns no hover/press/focus/menu state, so plain deletion is all it needs.
  if(gui() && docTagsList->window())
    gui()->deleteContents(docTagsList);
  else {
    for(Widget* row : docTagsList->select("*")) {
      row->removeFromParent();
      delete row->node;
    }
  }
  if(contextMenuDocPath.isEmpty())
    return;

  std::vector<std::string> currentTags = ScribbleDoc::extractDocTags(contextMenuDocPath.c_str());
  int shown = 0;
  bool truncated = false;

  if(docTagsSearchQuery.empty()) {
    std::function<void(const std::string&, int)> appendRecursive = [&](const std::string& id, int depth){
      if(shown >= DOC_TAGS_MAX_ROWS) {
        truncated = true;
        return;
      }
      Widget* row = createDocTagRow(id, depth, currentTags);
      if(!row)
        return;
      docTagsList->addWidget(row);
      ++shown;
      const TagNode* tagNode = tagStore.tag(id);
      if(tagNode && docTagsExpandedTags.count(id))
        for(const std::string& childId : tagNode->childIds)
          appendRecursive(childId, depth + 1);
    };
    for(const std::string& id : tagStore.rootTagIds())
      appendRecursive(id, 0);
  }
  else {
    std::string query = toLower(docTagsSearchQuery);
    for(const auto& pair : tagStore.allTags()) {
      if(toLower(pair.second.name).find(query) == std::string::npos)
        continue;
      if(shown >= DOC_TAGS_MAX_ROWS) {
        truncated = true;
        continue;
      }
      Widget* row = createDocTagRow(pair.first, 0, currentTags);
      if(row) {
        docTagsList->addWidget(row);
        ++shown;
      }
    }
  }

  if(truncated) {
    Widget* hint = new Widget(createTextNode(_("More tags - type to search")));
    hint->node->addClass("weak");
    hint->node->setAttribute("font-size", "12");
    hint->node->setAttribute("margin", "8 0 0 0");
    docTagsList->addWidget(hint);
  }
}

// The only place a document's own tags are written back to disk - everywhere else (extractDocTags())
// only ever reads them via the cheap partial-parse path. This one has to fully load and re-save the
// document, since there's no lightweight way to patch a single config attribute in place; that's fine
// here (a checkbox toggle, not a hot path) but is why this isn't also how the sidebar's own tag tree
// edits work - those never need to touch a document at all, see deleteTagWithUndo()'s comment.
//
// Document::save()'s thumb parameter is not "keep the existing thumbnail" - it is literally the PNG
// data to embed, and passing NULL means "write no thumbnail at all" (see document.cpp: `if(thumb)
// *outstrm << "<image id='thumbnail' ...`, both the plain-svg and bgz paths). There is no read-modify-
// write of just the config node, so the existing thumbnail has to be extracted before the load/save
// round-trip and re-encoded back in, or every document loses its preview the first time a tag is
// toggled on it - exactly what shipped here initially and was caught by testing.
void TagDocList::setDocumentTags(const FSPath& path, const std::vector<std::string>& tagIds)
{
  rewriteDocument(path, [&](Document&, ScribbleConfig& cfg){
    cfg.set("tags", TagStore::formatTagList(tagIds).c_str());
  });
}

bool TagDocList::rewriteDocument(const FSPath& path, const std::function<void(Document&, ScribbleConfig&)>& edit)
{
  Image thumbnail = ScribbleDoc::extractThumbnail(path.c_str());

  Document doc;
  if(doc.load(new FileStream(path.c_str()), false) != Document::LOAD_OK)
    return false;
  // .svgz's block-gzip format loads pages lazily regardless of the `delayload` argument above -
  // Document::loadBgzDoc() always just records each page's blockIdx and defers the actual per-page
  // read to Document::ensureLoaded()/loadBgzPage(), which reads through `doc`'s own blockStream (the
  // FileStream opened by load() above). The very next line below opens a *second*, independent
  // FileStream on the same path in "wb" mode for the save - which truncates the file on disk
  // immediately, before any not-yet-loaded page has been read. Any page still lazy at that point
  // silently fails to load (blockStream now reads an empty/truncated file), and since save() below
  // passes SAVE_FORCE, Document::saveBgz() writes that page out anyway - as blank content - rather
  // than aborting. This is how toggling a tag was observed to silently blank a document's other
  // pages: only the pages already touched (e.g. the one last viewed) survived. Forcing every page to
  // load here, while the original read stream is still intact, is what makes the round-trip safe.
  doc.ensurePagesLoaded();
  ScribbleConfig cfg(ScribbleApp::cfg);
  cfg.loadConfig(doc.getConfigNode());
  edit(doc, cfg);
  // every page is loaded, so this also renders any tagged page whose thumbnail the file did not carry
  ScribbleDoc::updatePageTagSummary(&doc, &cfg);
  cfg.saveConfig(doc.resetConfigNode());

  std::string thumbBuff;
  const char* thumbArg = NULL;
  if(!thumbnail.isNull()) {
    thumbBuff = base64_encode(thumbnail.encode(Image::PNG));
    thumbArg = thumbBuff.c_str();
  }
  if(!doc.save(new FileStream(path.c_str(), "wb"), thumbArg, Document::SAVE_FORCE))
    return false;
  tagStore.setDocTags(path.c_str(), (time_t)getFileMTime(path),
      TagStore::parseTagList(cfg.String("tags", "")), cfg.String("pagetags", ""));
  tagStore.save();
  return true;
}

// The cover is the document's own config, like its tags, so changing it is the same load and save
void TagDocList::changeCover(const FSPath& path)
{
  closeAllContextPopups();
  int coverArgb = atoi(ScribbleDoc::extractDocConfigValue(path.c_str(), "coverColor").c_str());
  ChangeCoverDialog dialog(Color::fromArgb(coverArgb));
  if(Application::execDialog(&dialog) != Dialog::ACCEPTED)
    return;
  int chosenArgb = int(dialog.coverColor().argb());
  if(chosenArgb == coverArgb)
    return;
  if(!rewriteDocument(path, [&](Document&, ScribbleConfig& cfg){ cfg.set("coverColor", chosenArgb); })) {
    ScribbleApp::messageBox(ScribbleApp::Error, _("Change Cover"), _("The document could not be saved."));
    return;
  }
  rebuildDocGrid();
}

void TagDocList::renameDoc(const FSPath& path)
{
  closeAllContextPopups();
  TagNameDialog dialog(_("Rename Document"), path.baseName().c_str());
  int res = Application::execDialog(&dialog);
  std::string name = dialog.getName();
  if(res != Dialog::ACCEPTED || name.empty())
    return;
  FSPath newPath = path.parent().child(name + "." + path.extension());
  if(newPath.exists()) {
    ScribbleApp::messageBox(ScribbleApp::Error, _("Rename Document"),
        fstring(_("\"%s\" already exists."), newPath.fileName().c_str()));
    return;
  }
  ScribbleApp::app->closeDocs(path);
  if(!moveFile(path, newPath)) {
    ScribbleApp::messageBox(ScribbleApp::Error, _("Rename Document"),
        fstring(_("Unable to rename \"%s\"."), path.baseName().c_str()));
    return;
  }
  tagStore.removeDoc(path.c_str());
  tagStore.save();
  refresh();
}

// A plain confirm-then-permanently-delete, unlike DocumentList's own delete (documentlist.cpp),
// which moves the file to a temp trash path and offers an undo bar - that machinery is a lot to
// duplicate for a first cut of tag assignment's "and so on" menu; the confirmation dialog is the
// only safety net here.
void TagDocList::deleteDoc(const FSPath& path)
{
  closeAllContextPopups();
  auto res = ScribbleApp::messageBox(ScribbleApp::Warning, _("Delete Document"),
      fstring(_("Delete \"%s\"? This cannot be undone."), path.baseName().c_str()),
      {_("Delete"), _("Cancel")});
  if(res != _("Delete"))
    return;
  ScribbleApp::app->closeDocs(path);
  removeFile(path.c_str());
  tagStore.removeDoc(path.c_str());
  tagStore.save();
  refresh();
}

// The grid is rebuilt rather than restyled, since only select mode's cells carry the ring and badge.
void TagDocList::setSelectMode(bool on)
{
  if(selectMode == on)
    return;
  closeAllContextPopups();
  selectMode = on;
  selectedDocs.clear();
  fabRow->setVisible(!on);
  selectBar->setVisible(on);
  rebuildDocGrid();
}

// restyles the cell(s) in place: a rebuild would reload every thumbnail on each tap
void TagDocList::toggleDocSelected(const std::string& path)
{
  bool selected = !selectedDocs.count(path);
  if(selected)
    selectedDocs.insert(path);
  else
    selectedDocs.erase(path);
  auto cells = docCells.equal_range(path);
  for(auto it = cells.first; it != cells.second; ++it)
    it->second->setChecked(selected);
  updateSelectBar();
}

void TagDocList::setAllSelected(bool selected)
{
  selectedDocs.clear();
  if(selected)
    selectedDocs.insert(shownDocs.begin(), shownDocs.end());
  for(auto& cell : docCells)
    cell.second->setChecked(selected);
  updateSelectBar();
}

void TagDocList::updateSelectBar()
{
  size_t count = selectedDocs.size();
  selectCountText->setText(count ? fstring(_("%d selected"), int(count)).c_str() : _("Select documents"));
  deleteSelectedBtn->setEnabled(count > 0);
  selectNoneBtn->setEnabled(count > 0);
  selectAllBtn->setEnabled(count < shownDocs.size());
}

// Permanent, like deleteDoc(): one confirmation for the lot, naming the document when there is just one.
void TagDocList::deleteSelectedDocs()
{
  if(selectedDocs.empty())
    return;
  std::string message = selectedDocs.size() == 1
      ? fstring(_("Delete \"%s\"? This cannot be undone."), FSPath(*selectedDocs.begin()).baseName().c_str())
      : fstring(_("Delete %d documents? This cannot be undone."), int(selectedDocs.size()));
  auto res = ScribbleApp::messageBox(ScribbleApp::Warning, _("Delete Documents"), message,
      {_("Delete"), _("Cancel")});
  if(res != _("Delete"))
    return;
  std::vector<std::string> failed;
  for(const std::string& path : selectedDocs) {
    ScribbleApp::app->closeDocs(FSPath(path));
    if(removeFile(path.c_str()))
      tagStore.removeDoc(path.c_str());
    else
      failed.push_back(FSPath(path).baseName());
  }
  tagStore.save();
  selectedDocs.clear();
  refresh();
  if(!failed.empty()) {
    ScribbleApp::messageBox(ScribbleApp::Error, _("Delete Documents"),
        fstring(_("Unable to delete: %s"), joinStr(failed, ", ").c_str()));
  }
}

void TagDocList::createUI()
{
  // ---- sidebar ----
  Widget* sidebar = new Widget(new SvgG());
  sidebar->node->addClass("sidebar");  // .tagdoclist .sidebar { fill: var(--dark); } - see theme.cpp
  sidebar->node->setAttribute("box-anchor", "vfill");
  sidebar->node->setAttribute("layout", "box");
  // a <g> paints nothing on its own; this fill rect (inheriting .sidebar's fill) is what actually
  // paints the background - without it the sidebar was reported as "not always rendering instantly"
  sidebar->addWidget(createFillRect());
  // The box layout ignores a width attribute on a <g>, and the fill rect above declares no size, so
  //  the sidebar used to be exactly as wide as its widest child. This vfill rect is what sets it.
  SvgRect* sidebarSizer = new SvgRect(Rect::wh(SIDEBAR_WIDTH, 0));
  sidebarSizer->setAttribute("box-anchor", "vfill");
  sidebarSizer->setAttribute("fill", "none");
  sidebar->containerNode()->addChild(sidebarSizer);

  Widget* sidebarContent = new Widget(new SvgG());
  sidebarContent->node->setAttribute("box-anchor", "fill");
  sidebarContent->node->setAttribute("layout", "flex");
  sidebarContent->node->setAttribute("flex-direction", "column");
  sidebarContent->setMargins(32, SIDEBAR_PAD_RIGHT, 12, SIDEBAR_PAD_LEFT);
  sidebar->addWidget(sidebarContent);

  SvgText* titleNode = createTextNode("Sumi");
  titleNode->addClass("doclist-title");
  Widget* titleWidget = new Widget(titleNode);
  titleWidget->node->setAttribute("box-anchor", "left");
  sidebarContent->addWidget(titleWidget);

  Widget* sep1 = createHRule();
  sep1->setMargins(12, 0, 0, 0);
  sidebarContent->addWidget(sep1);

  allDocumentsBtn = static_cast<Button*>(createNavRow("icons/ic_menu_doctext.svg", _("All Documents")));
  allDocumentsBtn->onClicked = [this](){ activeTags.clear(); refresh(); };
  sidebarContent->addWidget(allDocumentsBtn);

  tagDrag.reset(new RowDrag(this));
  // All Documents stands for "no parent": dropping a subtag there makes it a root tag again
  tagDrag->setRootTarget(allDocumentsBtn);
  tagDrag->canDrop = [this](int src, int dst){
    const std::string& srcId = dragTagIds[src];
    const TagNode* tagNode = tagStore.tag(srcId);
    if(!tagNode)
      return false;
    if(dst == RowDrag::ROOT)
      return !tagNode->parentId.empty();
    const std::string& dstId = dragTagIds[dst];
    // its own supertag is allowed: that is how a subtag is dragged back out (see onDrop)
    return dstId != srcId && !isTagDescendant(dstId, srcId);
  };
  tagDrag->onDrop = [this](int src, int dst){
    if(src < 0 || src >= int(dragTagIds.size()) || dst >= int(dragTagIds.size()))
      return;
    const TagNode* tagNode = tagStore.tag(dragTagIds[src]);
    if(!tagNode)
      return;
    std::string parentId = dst == RowDrag::ROOT ? std::string() : dragTagIds[dst];
    std::string afterId;
    // dropped on its own supertag: out of it, one level up, listed right below the supertag it left
    if(!parentId.empty() && parentId == tagNode->parentId) {
      const TagNode* parent = tagStore.tag(parentId);
      afterId = parentId;
      parentId = parent ? parent->parentId : std::string();
    }
    moveTagUnder(dragTagIds[src], parentId, afterId);
  };

  // no top margin here: allDocumentsBtn's own bottom margin (from createNavRow, matching its top
  // margin) already provides the gap, symmetric with the gap between sep1 and the row above it
  Widget* sep2 = createHRule();
  sidebarContent->addWidget(sep2);

  // The rounded, filled box *is* the TextEdit's own background (styled via .tagdoclist .inputbox-bg
  // in theme.cpp) - not a separate oversized rect behind it, which was rendering as a visibly bigger,
  // differently-proportioned box around a much smaller text field.
  Widget* tagSearchRow = new Widget(new SvgG());
  tagSearchRow->node->setAttribute("box-anchor", "hfill");
  tagSearchRow->node->setAttribute("layout", "box");
  tagSearchRow->setMargins(16, 0, 0, 0);
  tagSearchEdit = createTextEdit(-1);  // -1 = hfill, so it fills the row's width
  tagSearchEdit->setEmptyText(_("Search tags"));
  tagSearchEdit->onChanged = [this](const char* s){ tagSearchQuery = s; rebuildTagTree(); };
  tagSearchEdit->setMargins(8, 12, 8, 12);
  tagSearchRow->addWidget(tagSearchEdit);
  sidebarContent->addWidget(tagSearchRow);

  tagTreeView = new Widget(new SvgG());
  tagTreeView->node->setAttribute("box-anchor", "hfill");
  tagTreeView->node->setAttribute("layout", "flex");
  tagTreeView->node->setAttribute("flex-direction", "column");
  // small: each tag row already carries its own top margin (see createTagRow's "6 12 6 ..."), so
  // this only needs to cover the gap between the search box and the first row, not duplicate it
  ScrollWidget* tagScroll = new ScrollWidget(new SvgDocument(), tagTreeView);
  tagScroll->node->setAttribute("box-anchor", "fill");
  Widget* tagScrollContainer = new Widget(new SvgG());
  tagScrollContainer->node->setAttribute("box-anchor", "fill");
  tagScrollContainer->node->setAttribute("layout", "box");
  tagScrollContainer->addWidget(tagScroll);
  tagScrollContainer->setMargins(0, -TAG_LIST_BLEED_RIGHT, 0, -TAG_LIST_BLEED_LEFT);
  sidebarContent->addWidget(tagScrollContainer);

  // undo, search(tags), separator, filter-tick(multi-select) - the sidebar's own bottom toolbar
  // (Penpot's "Screen" reference: a plain icon row, not a text bar that appears/disappears like
  // DocumentList's delete-undo bar - undo just disables itself when there's nothing to undo).
  // "new tag" is folded in here too rather than beside the search box above, per feedback that a
  // plus there was redundant.
  Toolbar* sidebarBottom = createToolbar();
  undoButton = createToolbutton(SvgGui::useFile("icons/ic_menu_undo.svg"), "");
  undoButton->onClicked = [this](){ undoTagDelete(); };
  undoButton->setEnabled(false);
  newTagBtn = createToolbutton(SvgGui::useFile("icons/ic_menu_plus.svg"), _("New Tag"));
  newTagBtn->onClicked = [this](){ addTag(""); };
  multiSelectBtn = createToolbutton(SvgGui::useFile("icons/ic_menu_filter_tick.svg"), _("Multi-select Tags"));
  multiSelectBtn->onClicked = [this](){ toggleMultiSelect(); };
  sidebarBottom->addWidget(newTagBtn);
  sidebarBottom->addWidget(undoButton);
  sidebarBottom->addSeparator();
  sidebarBottom->addWidget(multiSelectBtn);
  sidebarContent->addWidget(sidebarBottom);

  // ---- main content ----
  Widget* content = new Widget(new SvgG());
  content->node->addClass("doclist-content");
  content->node->setAttribute("box-anchor", "fill");
  content->node->setAttribute("layout", "box");
  content->addWidget(createFillRect());

  Widget* contentInner = new Widget(new SvgG());
  contentInner->node->setAttribute("box-anchor", "fill");
  contentInner->node->setAttribute("layout", "flex");
  contentInner->node->setAttribute("flex-direction", "column");

  docSearchRow = new Widget(new SvgG());
  docSearchRow->node->setAttribute("box-anchor", "hfill");
  docSearchRow->node->setAttribute("layout", "flex");
  docSearchRow->node->setAttribute("flex-direction", "row");
  docSearchRow->node->setAttribute("align-items", "center");
  docSearchRow->setMargins(8, 24, 0, 24);

  // Same as the tag search box: the rounded fill *is* the TextEdit's own background, sized to the
  // text rather than a separate larger rect behind it.
  Widget* docSearchBox = new Widget(new SvgG());
  docSearchBox->node->setAttribute("box-anchor", "hfill");
  docSearchBox->node->setAttribute("layout", "box");
  docSearchEdit = createTextEdit(-1);
  docSearchEdit->setEmptyText(_("Search documents"));
  docSearchEdit->onChanged = [this](const char* s){ docSearchQuery = s; rebuildDocGrid(); };
  docSearchEdit->setMargins(8, 12, 8, 12);
  docSearchBox->addWidget(docSearchEdit);
  docSearchRow->addWidget(docSearchBox);

  Widget* docFilterBtn = new Widget(new SvgG());
  docFilterBtn->node->setAttribute("layout", "box");
  docFilterBtn->setMargins(0, 0, 0, 12);
  Button* filterIconBtn = createToolbutton(SvgGui::useFile("icons/ic_menu_filter.svg"), "");
  filterIconBtn->onClicked = [this](){
    // tag clicks already filter the search results once one is active (see toggleTagFilter()), so
    // this button's own job is just to draw attention to the tag sidebar rather than open anything
    focusedWidget = tagSearchEdit;
    gui()->setFocused(tagSearchEdit, SvgGui::REASON_TAB);
  };
  docFilterBtn->addWidget(filterIconBtn);
  docSearchRow->addWidget(docFilterBtn);

  contentInner->addWidget(docSearchRow);

  docGrid = new Widget(new SvgG());
  docGrid->node->setAttribute("box-anchor", "hfill");
  docGrid->node->setAttribute("layout", "flex");
  docGrid->node->setAttribute("flex-direction", "column");
  docGrid->node->setAttribute("justify-content", "flex-start");
  // no top margin: the cells' own margin already spaces the first row from the search box
  docGrid->setMargins(0, 16, 16, 16);

  docScrollWidget = new ScrollWidget(new SvgDocument(), docGrid);
  docScrollWidget->node->setAttribute("box-anchor", "fill");
  Widget* docScrollContainer = new Widget(new SvgG());
  docScrollContainer->node->setAttribute("box-anchor", "fill");
  docScrollContainer->node->setAttribute("layout", "box");
  docScrollContainer->addWidget(docScrollWidget);
  contentInner->addWidget(docScrollContainer);

  content->addWidget(contentInner);

  // Floating FABs, bottom-right of the content area, on top of the grid (added after it so they sit
  // in front - see the "Move z-order" convention noted throughout the codebase). Search was the
  // smaller/secondary FAB (the search fields are now always shown), Add Document the larger/accent-colored primary one, matching Penpot's
  // "Screen" reference. Open Whiteboard (not in the mockup) sits between them at the secondary size.
  // The back-to-note FAB is also new and sits to their left, shown only when there's a document open
  // behind this browser.
  fabRow = new Widget(new SvgG());
  fabRow->node->setAttribute("box-anchor", "bottom right");
  fabRow->node->setAttribute("layout", "flex");
  fabRow->node->setAttribute("flex-direction", "row");
  fabRow->setMargins(0, 48, 43, 0);
  backNoteFab = static_cast<Button*>(createFab("icons/ic_menu_back.svg", 44, false));
  backNoteFab->setMargins(0, 17, 0, 0);
  backNoteFab->setVisible(false);
  backNoteFab->onClicked = [this](){ finish(REJECTED); };
  fabRow->addWidget(backNoteFab);
  selectFab = static_cast<Button*>(createFab("icons/ic_menu_multiselect.svg", 44, false));
  selectFab->setMargins(0, 17, 0, 0);
  setupTooltip(selectFab, _("Select"));
  selectFab->onClicked = [this](){ setSelectMode(true); };
  fabRow->addWidget(selectFab);
  // DocumentList's "Open Whiteboard" toolbar button, as a secondary FAB; the connect dialog itself is
  //  ScribbleApp::openSharedDoc(), reached through the OPEN_WHITEBOARD result
  whiteboardFab = static_cast<Button*>(createFab("icons/ic_menu_people.svg", 44, false));
  whiteboardFab->setMargins(0, 17, 0, 0);
  whiteboardFab->onClicked = [this](){ selectedFile.clear(); finish(OPEN_WHITEBOARD); };
  fabRow->addWidget(whiteboardFab);
  // Import: a popup of formats, opening upwards from the bottom edge.  Picking one ends the browser with
  //  IMPORT_*, like Open Whiteboard, so the file picker and the import run in ScribbleApp.
  importFab = static_cast<Button*>(createFab("icons/ic_menu_import.svg", 44, false));
  importFab->setMargins(0, 17, 0, 0);
  ArrowPopup* importPopup = createArrowPopup(Menu::VERT_LEFT | Menu::ABOVE);
  importPopup->addItem(_("Document"), NULL, [this](){ selectedFile.clear(); finish(IMPORT_DOC); });
  Button* importPdfItem = importPopup->addItem(_("PDF"), NULL,
      [this](){ selectedFile.clear(); finish(IMPORT_PDF); });
  importPdfItem->setEnabled(PdfImport::isAvailable());
  importPopup->addItem(_("Noteful"), NULL, [this](){ selectedFile.clear(); finish(IMPORT_NOTEFUL); });
  importPopup->addSeparator();
  // chosen here rather than in Preferences, since whether folders mirror tags depends on the export at
  //  hand; added with addWidget(), not addItem(), so a toggle does not close the popup
  CheckBox* folderTagsBox = createCheckBox(_("Folders as tags"), ScribbleApp::cfg->Bool("notefulFolderTags", true));
  folderTagsBox->setMargins(6, 0);
  // in line with the menu items above, rather than centred in a popup the combo below may widen
  folderTagsBox->node->setAttribute("box-anchor", "left");
  folderTagsBox->onToggled = [](bool on){ ScribbleApp::cfg->set("notefulFolderTags", on); };
  importPopup->addWidget(folderTagsBox);
  // What a PDF or Noteful import may take on top of what Sumi already uses (PdfImport::MemoryBudget), for
  //  a device that would otherwise end the app mid-import; pages that would not fit come out at a lower
  //  resolution instead.  128 MB is about the least that still imports a page at 300 DPI, 2 GB about the
  //  most any iPad allows an app at all.
  static const int limitsMB[] = {128, 256, 512, 1024, 2048};
  std::vector<std::string> limitNames;
  // the choice nearest the configured limit, which is then what applies - the combo must not show one
  //  limit while another (set by hand, say) is in force
  int configuredMB = ScribbleApp::cfg->Int("importMemoryLimitMB");
  int limitIndex = 0;
  for(int ii = 0; ii < int(sizeof(limitsMB)/sizeof(limitsMB[0])); ++ii) {
    limitNames.push_back(limitsMB[ii] < 1024 ? fstring("%d MB", limitsMB[ii]) : fstring("%d GB", limitsMB[ii]/1024));
    if(std::abs(limitsMB[ii] - configuredMB) < std::abs(limitsMB[limitIndex] - configuredMB))
      limitIndex = ii;
  }
  if(limitsMB[limitIndex] != configuredMB)
    ScribbleApp::cfg->set("importMemoryLimitMB", limitsMB[limitIndex]);
  bool limitOn = ScribbleApp::cfg->Bool("importLimitMemory");
  CheckBox* limitBox = createCheckBox(_("Limit memory"), limitOn);
  limitBox->setMargins(6, 0);
  limitBox->node->setAttribute("box-anchor", "left");
  ComboBox* limitCombo = createComboBox(limitNames);
  limitCombo->setIndex(limitIndex);
  limitCombo->onChanged = [limitCombo](const char*){
    ScribbleApp::cfg->set("importMemoryLimitMB", limitsMB[limitCombo->index()]);
  };
  // indented under its checkbox, and only there while the box is checked
  Widget* limitRow = createRow({limitCombo}, "0 6 6 28", NULL, "left");
  limitRow->setVisible(limitOn);
  limitBox->onToggled = [limitRow](bool on){
    ScribbleApp::cfg->set("importLimitMemory", on);
    limitRow->setVisible(on);
  };
  importPopup->addWidget(limitBox);
  importPopup->addWidget(limitRow);
  setupPopupMenu(importFab, importPopup);
  fabRow->addWidget(importFab);
  addDocFab = static_cast<Button*>(createFab("icons/ic_menu_plus.svg", 56, true));
  addDocFab->onClicked = [this](){ newDoc(); };
  fabRow->addWidget(addDocFab);
  content->addWidget(fabRow);

  // Select mode's actions take the FABs' place: one rounded container holding the count and a row of
  //  FAB-sized buttons, the way the FABs would look grouped.  Done (X) is last, where Add Document was.
  selectBar = new Widget(new SvgG());
  selectBar->node->addClass("selectbar");
  selectBar->node->setAttribute("box-anchor", "bottom right");
  selectBar->node->setAttribute("layout", "box");
  selectBar->setMargins(0, 48, 43, 0);
  SvgRect* selectBarBg = new SvgRect(Rect::wh(20, 20), 14, 14);
  selectBarBg->addClass("selectbar-bg");
  selectBarBg->setAttribute("box-anchor", "fill");
  selectBar->containerNode()->addChild(selectBarBg);
  Widget* selectBarRow = new Widget(new SvgG());
  selectBarRow->node->setAttribute("layout", "flex");
  selectBarRow->node->setAttribute("flex-direction", "row");
  selectBarRow->node->setAttribute("align-items", "center");
  selectBarRow->setMargins(6);
  selectCountText = createTextNode(_("Select documents"));
  selectCountText->addClass("selectbar-count");
  Widget* selectCount = new Widget(selectCountText);
  selectCount->setMargins(0, 14, 0, 12);
  selectBarRow->addWidget(selectCount);
  selectAllBtn = createSelectBarButton("icons/ic_menu_select_all.svg", _("Select All"));
  selectAllBtn->onClicked = [this](){ setAllSelected(true); };
  selectBarRow->addWidget(selectAllBtn);
  selectNoneBtn = createSelectBarButton("icons/ic_menu_select_none.svg", _("Select None"));
  selectNoneBtn->onClicked = [this](){ setAllSelected(false); };
  selectBarRow->addWidget(selectNoneBtn);
  deleteSelectedBtn = createSelectBarButton("icons/ic_menu_discard.svg", _("Delete"));
  deleteSelectedBtn->node->addClass("selectbar-delete");
  deleteSelectedBtn->onClicked = [this](){ deleteSelectedDocs(); };
  selectBarRow->addWidget(deleteSelectedBtn);
  Widget* selectBarSep = new Widget(new SvgRect(Rect::wh(1, 28)));
  selectBarSep->node->addClass("selectbar-sep");
  selectBarSep->setMargins(0, 6);
  selectBarRow->addWidget(selectBarSep);
  Button* selectDoneBtn = createSelectBarButton("icons/ic_menu_cancel.svg", _("Done"));
  selectDoneBtn->onClicked = [this](){ setSelectMode(false); };
  selectBarRow->addWidget(selectDoneBtn);
  selectBar->addWidget(selectBarRow);
  selectBar->setVisible(false);
  content->addWidget(selectBar);

  Widget* mainLayout = selectFirst("#main-layout");
  mainLayout->addWidget(sidebar);
  mainLayout->addWidget(content);

  // grid item prototype - identical to DocumentList's, kept as a local copy since the two views are
  // deliberately independent (see tagdoclist.h).  Anchored to the top of its grid row: cells differ in
  //  height (a notebook's tag line, a page card's notebook line), and unanchored a shorter one is
  //  centred, so its preview sits lower than its neighbours'.
  static const char* gridItemProtoSVG = R"(
    <g class="listitem" margin="14 16" layout="box" box-anchor="top">
      <rect box-anchor="fill" width="48" height="48"/>
      <g layout="flex" flex-direction="column">
        <g class="image-container" box-anchor="hfill" layout="box"></g>
        <text class="title-text" margin="8 0"></text>
      </g>
    </g>
  )";
  gridItemProto.reset(loadSVGFragment(gridItemProtoSVG));
  fileUseNode.reset(new SvgUse(Rect::wh(100, 100), "", SvgGui::useFile("icons/ic_file_fill.svg")));
  fileUseNode->addClass("icon");

  // one persistent context popup for every tag row - see showTagMenu()'s comment for why this is an
  // ArrowPopup (the app's standard for right-click/long-press) rather than a plain Menu
  tagContextPopup = createArrowPopup(Menu::VERT_RIGHT);
  tagContextPopup->addItem(_("Add Subtag"), SvgGui::useFile("icons/ic_tag.svg"), [this](){ addTag(contextMenuTagId); });
  tagContextPopup->addItem(_("Rename"), SvgGui::useFile("icons/ic_tag.svg"), [this](){ renameTag(contextMenuTagId); });
  tagContextPopup->addItem(_("Delete"), SvgGui::useFile("icons/ic_tag.svg"), [this](){
    const TagNode* n = tagStore.tag(contextMenuTagId);
    if(!n)
      return;
    closeAllContextPopups();
    bool deleteChildren = false;
    if(!n->childIds.empty()) {
      auto res = ScribbleApp::messageBox(ScribbleApp::Question, _("Delete Tag"),
          fstring(_("\"%s\" has subtags. Delete just this tag and keep its subtags, "
              "or delete it along with all its subtags?"), n->name.c_str()),
          {_("Keep Subtags"), _("Delete All"), _("Cancel")});
      if(res == _("Delete All"))
        deleteChildren = true;
      else if(res != _("Keep Subtags"))
        return;
    }
    // a subtag's documents can fall back to its supertag rather than losing the tag altogether
    std::vector<FSPath> retagDocs;
    const TagNode* parent = tagStore.tag(n->parentId);
    if(parent) {
      std::vector<FSPath> tagged = docsWithTag(contextMenuTagId, deleteChildren);
      if(!tagged.empty()) {
        auto res = ScribbleApp::messageBox(ScribbleApp::Question, _("Delete Tag"),
            fstring(_("%d document(s) are tagged \"%s\". Tag them with \"%s\" instead, "
                "or remove the tag from them?"), int(tagged.size()), n->name.c_str(), parent->name.c_str()),
            {fstring(_("Tag \"%s\""), parent->name.c_str()), _("Remove Tag"), _("Cancel")});
        if(res == _("Cancel") || res.empty())
          return;
        if(res != _("Remove Tag"))
          retagDocs = tagged;
      }
    }
    deleteTagWithUndo(contextMenuTagId, deleteChildren, retagDocs);
  });
  setupAutoClosePopup(tagContextPopup);

  // Document long-press menu: Manage Tags (opens docTagsPopup below), Change Cover, Rename, Delete.
  docContextPopup = createArrowPopup(Menu::VERT_RIGHT);
  docContextPopup->addItem(_("Manage Tags..."), NULL, [this](){
    // showDocTagsPopup() reparents docTagsPopup onto the same cell docContextPopup is currently
    // parented to - closeAutoClosePopup() first, or that cell would end up with two popup children
    // fighting over the same menu-stack slot.
    Widget* cell = docContextPopup->parent();
    closeAutoClosePopup(docContextPopup);
    if(cell)
      showDocTagsPopup(contextMenuDocPath, cell);
  });
  docContextPopup->addItem(_("Change Cover..."), NULL, [this](){ changeCover(contextMenuDocPath); });
  docContextPopup->addItem(_("Rename"), NULL, [this](){ renameDoc(contextMenuDocPath); });
  docContextPopup->addItem(_("Delete"), NULL, [this](){ deleteDoc(contextMenuDocPath); });
  setupAutoClosePopup(docContextPopup);

  // Tag-assignment popup: search box + a checkbox per tag (multi-select, toggled live - see
  // rebuildDocTagsList()) + a Done button that just closes it, top-right per the design.
  docTagsPopup = createArrowPopup(Menu::VERT_RIGHT);
  Widget* docTagsHeader = createRow();
  docTagsHeader->setMargins(0, 8, 8, 8);
  Widget* docTagsTitle = new Widget(createTextNode(_("Manage Tags")));
  docTagsTitle->node->addClass("arrowpopup-title");
  docTagsHeader->addWidget(docTagsTitle);
  docTagsHeader->addWidget(createStretch());
  // New Tag is reachable here too, not just the sidebar's own "+" - creating a tag while assigning
  // one to a document is the more common path in practice, and forcing a trip out to the sidebar
  // and back defeats the point of this popup existing at all.
  Button* docTagsNewBtn = createToolbutton(SvgGui::useFile("icons/ic_menu_plus.svg"), _("New Tag"));
  docTagsNewBtn->onClicked = [this](){
    // captured before closeAllContextPopups()/the dialog's nested event loop, not read fresh
    // afterward - contextMenuDocPath is a member and this is cheap insurance against it changing
    // out from under a still-running callback
    FSPath docPath = contextMenuDocPath;
    // captured before closeAllContextPopups() detaches it - closeAutoClosePopup() only hides the
    // popup, it doesn't reparent it, so this is still the right cell to reopen on afterward
    Widget* cell = docTagsPopup->parent();
    closeAllContextPopups();
    TagNameDialog dialog(_("New Tag"), "");
    int res = Application::execDialog(&dialog);
    std::string name = dialog.getName();
    if(res != Dialog::ACCEPTED || name.empty())
      return;
    std::string newTagId = tagStore.addTag(name, "");
    tagStore.save();
    // auto-assign the freshly created tag to the document this popup is open for, rather than
    // leaving it created-but-unchecked - the whole point of reaching "new tag" from here is to tag
    // *this* document with it
    std::vector<std::string> tags = ScribbleDoc::extractDocTags(docPath.c_str());
    tags.push_back(newTagId);
    setDocumentTags(docPath, tags);
    // reopen (rather than leave it closed by closeAllContextPopups() above) so the user sees the new
    // tag land checked and can keep going - this is a multi-select popup, one tag shouldn't end it
    if(cell)
      showDocTagsPopup(docPath, cell);
  };
  docTagsHeader->addWidget(docTagsNewBtn);
  Button* docTagsDoneBtn = createPushbutton(_("Done"));
  // The pushbutton prototype is box-anchor="fill" so it can be dropped into a fixed-size slot and
  // stretch to it, but here it sits next to createStretch() in a flex row - two fill-anchored items
  // along the flex direction both get squeezed when the row runs out of room (see the warning in
  // svggui.cpp's setLayoutBounds() about exactly this), leaving the button's rect background narrower
  // than the text it should contain: the background shrinks (it's hfill) but the text inside can't,
  // so it overflows the button. Restricting it to vfill keeps it full row height but leaves it sized
  // to its own text, so createStretch() is the only thing that actually gives way.
  docTagsDoneBtn->node->setAttribute("box-anchor", "vfill");
  // Blue like the primary FAB (see createFab's primary color) so Done reads as the affirmative action.
  docTagsDoneBtn->selectFirst(".pushbtn-bg")->node->setAttribute("fill", "#2EA3CF");
  docTagsDoneBtn->onClicked = [this](){ closeAutoClosePopup(docTagsPopup); };
  docTagsHeader->addWidget(docTagsDoneBtn);
  docTagsPopup->addWidget(docTagsHeader);

  docTagsSearchEdit = createTextEdit(-1);
  docTagsSearchEdit->setEmptyText(_("Search tags"));
  docTagsSearchEdit->onChanged = [this](const char* s){ docTagsSearchQuery = s; rebuildDocTagsList(); };
  docTagsSearchEdit->setMargins(8, 12, 8, 12);
  docTagsPopup->addWidget(docTagsSearchEdit);

  docTagsList = new Widget(new SvgG());
  docTagsList->node->setAttribute("box-anchor", "hfill");
  docTagsList->node->setAttribute("layout", "flex");
  docTagsList->node->setAttribute("flex-direction", "column");
  docTagsList->node->setAttribute("width", "280");
  docTagsList->setMargins(8, 12, 0, 12);
  docTagsPopup->addWidget(docTagsList);
  setupAutoClosePopup(docTagsPopup);

  // Click-outside-to-defocus for the search boxes. An unhandled press bubbles from the hit widget up
  // through its ancestors (see SvgGui::sendEvent's `while(widget && !widget->sdlEvent(...))` walk) -
  // it does NOT fall sideways to a sibling background rect, which is why an earlier attempt at this,
  // hung off a fill-rect behind the content, never actually fired: whatever plain <g> was in front of
  // it (not a sibling) always intercepted the hit test first. The window itself is an ancestor of
  // everything, so this only ever runs when nothing more specific (a button, a row, the text edit
  // itself) already consumed the press.
  addHandler([this](SvgGui* gui, SDL_Event* event){
    if(event->type == SDL_KEYDOWN && event->key.keysym.sym == SDLK_ESCAPE && selectMode) {
      setSelectMode(false);
      return true;
    }
    // SvgGui::setFocused() dereferences its argument unconditionally (widget->window()), so it has
    // no way to express "clear focus" - clearing focusedWidget directly and sending FOCUS_LOST
    // ourselves is the same thing setFocused() itself does internally before assigning a new one.
    if(event->type == SDL_FINGERDOWN && focusedWidget) {
      Widget* prev = focusedWidget;
      focusedWidget = NULL;
      prev->sdlUserEvent(gui, SvgGui::FOCUS_LOST, SvgGui::REASON_PRESSED);
    }
    return false;
  });
}

void TagDocList::setup(Window* parent, bool hasCurrentNote)
{
  setWinBounds(parent->winBounds());
  backNoteFab->setVisible(hasCurrentNote);
  focusedWidget = docScrollWidget;
  // TagDocList is cached and reused across separate opens (ScribbleApp::tagDocList), so a delete from
  // a previous visit must not leave undo looking available on this one.
  hideUndo();
  selectedPage = -1;
  // cached across opens like undo above: always back to the plain browser
  selectMode = false;
  selectedDocs.clear();
  fabRow->setVisible(true);
  selectBar->setVisible(false);
  // clear of the status bar, which the window extends under (ScribbleApp::topInset)
  docSearchRow->setMargins(8 + ScribbleApp::topInset, 24, 0, 24);
  // the editor's Tag Page dialog adds tags to the same index file; every edit here is saved as it is
  //  made, so reading the file back loses nothing
  tagStore.load();
  refresh();
}

void TagDocList::finish(Result_t res)
{
  hideUndo();
  gui()->closeWindow(this);
  result = res;
  if(onFinished)
    onFinished(res);
}
