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
  // layoutMode (with initProps) edits a page layout rather than a page: geometry only, so the color
  //  pickers are hidden - a layout takes its colors from the document's theme when it is used.
  //  Otherwise they are behind a collapsed "Advanced settings" checkbox, since the theme sets them too.
  RulingDialog(ScribbleDoc* doc, const PageProperties* initProps = NULL, bool layoutMode = false);
  void accept();
  const PageProperties& properties() const { return props; }

  static int predefSizes[][2];
  // {x ruling, y ruling, left margin}; colors are not part of a preset - see setRuleType()
  static unsigned int predefRulings[][3];
  // dot radius for each predefRulings entry (0 = lines); kept apart because radii are fractional
  static Dim predefDotRadii[];
  // index of the "Music staves" preset in predefRulings
  static const int STAFF_PRESET;

private:
  void setPaperType(int index);
  void setRuleType(int index);
  void checkClipping();
  // redraws the page thumbnail from the current control values; the numbers in this dialog are
  //  meaningless without it - nobody knows what "Y Ruling 40" looks like until they see it
  void updatePreview();
  bool staffActive() const;

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
  Dim previewW, previewH;
  ScrollWidget* scrollWidget = NULL;

  PageProperties props;
  bool initialStaff = false;
  bool staffPreset = false;
};
