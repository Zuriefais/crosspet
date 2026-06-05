#pragma once
#include <I18n.h>

#include <string>
#include <vector>

#include "ReaderOptionsActivity.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class Fb2ReaderMenuActivity final : public Activity {
 public:
  enum class MenuAction {
    READER_OPTIONS,
    SELECT_SECTION,
    ROTATE_SCREEN,
    GO_HOME,
  };

  explicit Fb2ReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 const std::string& title, const int currentPage, const int totalPages,
                                 const int bookProgressPercent, const uint8_t currentOrientation);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  struct MenuItem {
    MenuAction action;
    StrId labelId;
  };

  std::vector<MenuItem> menuItems;
  int selectedIndex = 0;
  ButtonNavigator buttonNavigator;
  std::string title;
  uint8_t pendingOrientation = 0;
  const std::vector<StrId> orientationLabels = {StrId::STR_PORTRAIT, StrId::STR_LANDSCAPE_CW, StrId::STR_INVERTED,
                                                 StrId::STR_LANDSCAPE_CCW};
  bool settingsChanged = false;
};
