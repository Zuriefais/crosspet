#include "Fb2ReaderMenuActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

Fb2ReaderMenuActivity::Fb2ReaderMenuActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const std::string& title, const int currentPage, const int totalPages,
                                             const int bookProgressPercent, const uint8_t currentOrientation)
    : Activity("Fb2ReaderMenu", renderer, mappedInput), title(title), pendingOrientation(currentOrientation) {
  menuItems.reserve(6);
  menuItems.push_back({MenuAction::READER_OPTIONS, StrId::STR_READER_OPTIONS});
  menuItems.push_back({MenuAction::SELECT_SECTION, StrId::STR_SELECT_CHAPTER});
  menuItems.push_back({MenuAction::ROTATE_SCREEN, StrId::STR_ORIENTATION});
  menuItems.push_back({MenuAction::GO_HOME, StrId::STR_GO_HOME_BUTTON});
}

void Fb2ReaderMenuActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void Fb2ReaderMenuActivity::onExit() { Activity::onExit(); }

void Fb2ReaderMenuActivity::loop() {
  buttonNavigator.onNext([this] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, static_cast<int>(menuItems.size()));
    requestUpdate();
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto selectedAction = menuItems[selectedIndex].action;
    if (selectedAction == MenuAction::ROTATE_SCREEN) {
      pendingOrientation = (pendingOrientation + 1) % orientationLabels.size();
      requestUpdate();
      return;
    }

    if (selectedAction == MenuAction::READER_OPTIONS) {
      const auto beforeOrientation = SETTINGS.orientation;
      startActivityForResult(std::make_unique<ReaderOptionsActivity>(renderer, mappedInput),
                             [this, beforeOrientation](const ActivityResult&) {
                               settingsChanged = settingsChanged || SETTINGS.orientation != beforeOrientation;
                               pendingOrientation = SETTINGS.orientation;
                               requestUpdate();
                             });
      return;
    }

    setResult(MenuResult{static_cast<int>(selectedAction), pendingOrientation, settingsChanged});
    finish();
    return;
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    result.data = MenuResult{-1, pendingOrientation, settingsChanged};
    setResult(std::move(result));
    finish();
    return;
  }
}

void Fb2ReaderMenuActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 title.c_str(), nullptr, true);

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;

  GUI.drawList(
      renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, static_cast<int>(menuItems.size()),
      selectedIndex,
      [this](int index) { return I18N.get(menuItems[index].labelId); }, nullptr, nullptr,
      [this](int index) -> std::string {
        if (menuItems[index].action == MenuAction::ROTATE_SCREEN) {
          return I18N.get(orientationLabels[pendingOrientation]);
        }
        return "";
      },
      true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);

  renderer.displayBuffer();
}
