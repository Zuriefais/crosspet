#include "ProfileSelectActivity.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <cstring>
#include <cctype>
#include <variant>

#include "../util/ConfirmationActivity.h"
#include "../util/KeyboardEntryActivity.h"
#include "../ActivityResult.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ProfileStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

void ProfileSelectActivity::onEnter() {
  Activity::onEnter();

  PROFILE_STORE.ensureDefaultProfile();
  rebuildList();
  currentMode = Mode::LIST;

  for (size_t i = 0; i < profileList.size(); i++) {
    if (profileList[i]->id == PROFILE_STORE.getActiveProfileId()) {
      selectedIndex = static_cast<int>(i);
      break;
    }
  }

  requestUpdate();
}

void ProfileSelectActivity::onExit() {
  Activity::onExit();
}

void ProfileSelectActivity::rebuildList() {
  profileList.clear();
  const auto& profiles = PROFILE_STORE.getProfiles();
  for (const auto& p : profiles) {
    profileList.push_back(&p);
  }
}

void ProfileSelectActivity::loop() {
  if (currentMode == Mode::ADD) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      auto handler = [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          if (auto* kr = std::get_if<KeyboardResult>(&result.data)) {
            if (!kr->text.empty()) {
              PROFILE_STORE.addProfile(kr->text);
              rebuildList();
              selectedIndex = static_cast<int>(profileList.size()) - 1;
            }
          }
        }
        currentMode = Mode::LIST;
        requestUpdate();
      };
      startActivityForResult(std::make_unique<KeyboardEntryActivity>(
                                renderer, mappedInput, std::string(tr(STR_PROFILE_ENTER_NAME)), "", 32),
                            handler);
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      currentMode = Mode::LIST;
      requestUpdate();
      return;
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (currentMode != Mode::LIST) {
      currentMode = Mode::LIST;
      rebuildList();
      requestUpdate();
      return;
    }
    SETTINGS.saveToFile();
    finish();
    return;
  }

  if (currentMode == Mode::LIST) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      if (selectedIndex == static_cast<int>(profileList.size())) {
        currentMode = Mode::ADD;
        requestUpdate();
        return;
      }

      const Profile* sel = profileList[selectedIndex];
      if (sel->id == PROFILE_STORE.getActiveProfileId()) {
        currentMode = Mode::RENAME;
        strncpy(renameBuffer, sel->name.c_str(), sizeof(renameBuffer) - 1);
        renameBuffer[sizeof(renameBuffer) - 1] = '\0';
        renameCursor = static_cast<int>(strlen(renameBuffer));
        requestUpdate();
        return;
      }

      PROFILE_STORE.setActiveProfile(sel->id);
      finish();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (selectedIndex > 0 && selectedIndex < static_cast<int>(profileList.size())) {
        const Profile* sel = profileList[selectedIndex];
        std::string profileName = sel->name;
        std::string profileId = sel->id;
        startActivityForResult(
            std::make_unique<ConfirmationActivity>(renderer, mappedInput,
                                                    std::string(tr(STR_PROFILE_CONFIRM_DELETE)) + " " + profileName + "?",
                                                    std::string(tr(STR_PROFILE_DATA_WARNING))),
            [this, profileId](const ActivityResult& result) {
              if (!result.isCancelled) {
                PROFILE_STORE.removeProfile(profileId);
                rebuildList();
                if (selectedIndex >= static_cast<int>(profileList.size())) {
                  selectedIndex = static_cast<int>(profileList.size()) - 1;
                }
              }
              requestUpdate();
            });
        return;
      }
    }
  } else if (currentMode == Mode::RENAME) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      const Profile* sel = profileList[selectedIndex];
      if (renameBuffer[0] != '\0' && sel->id == PROFILE_STORE.getActiveProfileId()) {
        PROFILE_STORE.renameProfile(sel->id, renameBuffer);
        rebuildList();
      }
      currentMode = Mode::LIST;
      requestUpdate();
      return;
    }

    if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
      if (renameCursor > 0) renameCursor--;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
      if (renameCursor < static_cast<int>(strlen(renameBuffer))) renameCursor++;
      requestUpdate();
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
      if (renameCursor < static_cast<int>(strlen(renameBuffer))) {
        renameBuffer[renameCursor] = toupper(renameBuffer[renameCursor]);
        requestUpdate();
      }
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
      if (renameCursor < static_cast<int>(strlen(renameBuffer))) {
        renameBuffer[renameCursor] = tolower(renameBuffer[renameCursor]);
        requestUpdate();
      }
    }
  }

  buttonNavigator.onNextRelease([this] {
    int maxIndex = (currentMode == Mode::LIST) ? static_cast<int>(profileList.size() + 1)
                                               : static_cast<int>(profileList.size());
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, maxIndex);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this] {
    int maxIndex = (currentMode == Mode::LIST) ? static_cast<int>(profileList.size() + 1)
                                               : static_cast<int>(profileList.size());
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, maxIndex);
    requestUpdate();
  });
}

void ProfileSelectActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_PROFILES));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (currentMode == Mode::LIST) {
    int totalItems = static_cast<int>(profileList.size() + 1);
    GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight}, totalItems, selectedIndex,
                 [this](int index) {
                   if (index < static_cast<int>(profileList.size())) {
                     return std::string(profileList[index]->name);
                   }
                   return std::string(tr(STR_PROFILE_ADD));
                 },
                 nullptr, nullptr,
                 [this](int index) {
                   if (index < static_cast<int>(profileList.size()) &&
                       profileList[index]->id == PROFILE_STORE.getActiveProfileId()) {
                     return std::string(tr(STR_SELECTED));
                   }
                   return std::string("");
                 },
                 true);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DELETE));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (currentMode == Mode::ADD) {
    renderer.drawText(UI_10_FONT_ID, 20, contentTop + 20, tr(STR_PROFILE_ENTER_NAME));

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (currentMode == Mode::DELETE) {
    renderer.drawText(UI_10_FONT_ID, 20, contentTop + 20, tr(STR_PROFILE_CONFIRM_DELETE));
    renderer.drawText(SMALL_FONT_ID, 20, contentTop + 50, tr(STR_PROFILE_DATA_WARNING));

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_DELETE), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (currentMode == Mode::RENAME) {
    renderer.drawText(UI_10_FONT_ID, 20, contentTop + 20, tr(STR_PROFILE_RENAME));

    char displayBuf[64] = {};
    strncpy(displayBuf, renameBuffer, sizeof(displayBuf) - 1);
    if (renameCursor < static_cast<int>(strlen(displayBuf))) {
      memmove(displayBuf + renameCursor + 1, displayBuf + renameCursor,
              strlen(displayBuf + renameCursor) + 1);
      displayBuf[renameCursor] = '|';
    } else {
      strncat(displayBuf, "|", sizeof(displayBuf) - strlen(displayBuf) - 1);
    }
    renderer.drawText(UI_10_FONT_ID, 20, contentTop + 50, displayBuf);

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CONFIRM), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
