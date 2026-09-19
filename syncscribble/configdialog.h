#pragma once

#include "scribbleconfig.h"
#include "ugui/widgets.h"

// settings popup for one tool's options row, built from the same pref info as the Preferences
//  dialog; each control applies its change immediately (see configdialog.cpp)
ArrowPopup* createToolSettingsPopup(const char* title, const std::vector<const char*>& prefNames);
Button* createToolSettingsButton(const char* title, const std::vector<const char*>& prefNames,
    const std::vector<Button*>& extraItems = {});

class ConfigDialog : public Dialog
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
  typedef std::map<std::string, Widget*> PropGroups_t;
  PropGroups_t propGroups;
};
