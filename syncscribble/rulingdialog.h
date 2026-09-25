#pragma once

#include "ugui/widgets.h"
#include "page.h"

class ScribbleDoc;
class ColorEditBox;

class RulingDialog : public PopupDialog
{
public:
  // initProps non-NULL puts the dialog in "new page" mode: it edits a standalone set of page
  //  properties for a page that does not exist yet, so accept() applies nothing to the document and
  //  the caller reads the result from properties().  The apply-to-all/default checkboxes are hidden,
  //  since there is no page to apply to and the point of the mode is a one-off ruling.
  RulingDialog(ScribbleDoc* doc, const PageProperties* initProps = NULL);
  void accept();
  const PageProperties& properties() const { return props; }

  static int predefSizes[][2];
  static unsigned int predefRulings[][4];

private:
  void setPaperType(int index);
  void setRuleType(int index);
  void checkClipping();
  // redraws the page thumbnail from the current control values; the numbers in this dialog are
  //  meaningless without it - nobody knows what "Y Ruling 40" looks like until they see it
  void updatePreview();

  ScribbleDoc* scribbleDoc;
  bool newPageMode;

  SpinBox* spinWidth;
  SpinBox* spinHeight;
  SpinBox* spinXRuling;
  SpinBox* spinYRuling;
  SpinBox* spinLeftMargin;
  SpinBox* spinDotRadius;
  ColorEditBox* pageColorPicker;
  ColorEditBox* ruleColorPicker;
  CheckBox* cbApplyToAll;
  CheckBox* cbDocDefault;
  CheckBox* cbGlobalDefault;
  ComboBox* comboRuling;
  ComboBox* comboPaperSize;
  Widget* clipWarning;
  Widget* rulePreview;
  ScrollWidget* scrollWidget = NULL;

  PageProperties props;
};
