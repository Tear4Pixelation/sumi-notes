#pragma once

#include "scribbleconfig.h"
#include "ugui/widgets.h"
#include "scribbleapp.h"

// settings popup for one tool's options row, built from the same pref info as the Preferences
//  dialog; each control applies its change immediately (see configdialog.cpp)
ArrowPopup* createToolSettingsPopup(const char* title, const std::vector<const char*>& prefNames);
// extraRows are arbitrary widgets (e.g. a titled row holding a combo box) for settings that are not
//  config prefs - the pen tip is a flag on the pen itself, not a pref, so it cannot come from prefNames
Button* createToolSettingsButton(const char* title, const std::vector<const char*>& prefNames,
    const std::vector<Button*>& extraItems = {}, const std::vector<Widget*>& extraRows = {});

class ConfigDialog : public PopupDialog
{
public:
  ConfigDialog(ScribbleConfig* _cfg);
  void accept();
  void resetPrefs();
  void toggleAdvPrefs(bool show);

private:
  void init();

  ScribbleConfig* cfg = NULL;
  Widget* toolStack;
  std::vector<Widget*> allprops;
  // the default page size is two config values (pageWidth, pageHeight), not one pref
  ComboBox* pageSizeCombo = NULL;
  std::vector<ScribbleApp::PageSizePreset> pageSizes;
  typedef std::map<std::string, Widget*> PropGroups_t;
  PropGroups_t propGroups;
};
