#pragma once

#include <Fb2.h>
#include <Epub/Page.h>

#include <memory>
#include <vector>

#include "activities/Activity.h"
#include "CrossPointSettings.h"

class Fb2ReaderActivity final : public Activity {
  std::unique_ptr<Fb2> fb2;

  int currentSection = 0;
  int currentPageInSection = 0;
  int totalPages = 1;
  int pagesUntilFullRefresh = 0;
  bool sideButtonLongPressHandled = false;
  bool frontButtonLongPressHandled = false;
  bool longPressMenuHandled = false;
  bool longPowerButtonHandled = false;

  // Cached pages for current section
  int currentSectionCached = -1;
  std::vector<std::unique_ptr<Page>> sectionPages;

  int viewportWidth = 0;
  int viewportHeight = 0;
  bool initialized = false;

  int fontId = 0;
  int cachedFontId = 0;
  float lineCompression = 1.0f;
  bool extraParagraphSpacing = false;
  bool forceParagraphIndents = false;
  uint8_t paragraphAlignment = 0;
  bool hyphenationEnabled = false;
  bool bionicReadingEnabled = false;
  bool guideReadingEnabled = false;
  int cachedOrientedMarginTop = 0;
  int cachedOrientedMarginRight = 0;
  int cachedOrientedMarginBottom = 0;
  int cachedOrientedMarginLeft = 0;

  void renderPage();
  void renderStatusBar() const;

  void initializeReader();
  void buildSectionPages(int sectionIndex);
  void loadPageFromCache(int pageIndex);
  void saveProgress() const;
  void loadProgress();

  void reindexCurrentSection();
  void applyOrientation(uint8_t orientation);
  void executeReaderQuickAction(CrossPointSettings::LONG_PRESS_MENU_ACTION action);
  void executeLongPressMenuAction();
  bool consumeLongPowerButtonRelease();
  bool consumeLongPowerButtonHold();
  bool executeShortPowerButtonAction();
  bool executeLongPowerButtonAction();

 public:
  explicit Fb2ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Fb2> fb2)
      : Activity("Fb2Reader", renderer, mappedInput), fb2(std::move(fb2)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool canSnapshotForSleepOverlay() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
  std::string getCurrentBookPath() const override { return fb2 ? fb2->getPath() : std::string{}; }
  ScreenshotInfo getScreenshotInfo() const override;
};
