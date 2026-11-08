#pragma once

#include "ugui/widgets.h"
#include "touchwidgets.h"
#include "document.h"

class ScribbleDoc;
class TextEdit;
class ColorEditBox;

// Create Link: what a link points at is chosen by type - an outline entry of this document, any page of
//  any document, or a URL.  See docs/agent/links.md for the href formats and why they are what they are.
class LinkDialog : public PopupDialog
{
public:
  enum TargetType { TARGET_OUTLINE = 0, TARGET_PAGE = 1, TARGET_URL = 2 };

  LinkDialog(ScribbleDoc* sd);
  void accept();

private:
  void setType(int type);
  // path "" is this document; returns false if the document could not be read
  bool setTargetDoc(const std::string& path, int pagenum = 0);
  void parseHref(const char* href);

  ScribbleDoc* scribbleDoc;
  ColorEditBox* penColorPicker;
  ComboBox* typeCombo;
  Widget* outlineRow;
  ComboBox* outlineCombo = NULL;
  Widget* docRow;
  ComboBox* docCombo;
  Widget* pageRow;
  SpinBox* pageSpin;
  Widget* pageCountLabel;
  Widget* urlRow;
  TextEdit* hrefEdit;

  std::vector<OutlineEntry> outline;
  // the path of each docCombo item, in order: "" for this document, OTHER_DOC for "Other Document..."
  std::vector<std::string> docChoices;
  std::string targetDoc;  // "" = this document
  int targetPageCount = 0;
  int docComboIndex = 0;  // to restore the combo when "Other Document..." is chosen
  // the file picker may answer after the dialog is gone
  std::shared_ptr<bool> aliveToken = std::make_shared<bool>(true);

  // the link being edited, and what the dialog made of it: OK without a change keeps it as it was, so
  //  e.g. a link to an old bookmark is not turned into a link to the top of its page
  std::string origHref;
  int origType = TARGET_URL, origOutline = -1, origPage = -1;
  std::string origDoc;
};
