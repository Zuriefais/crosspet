#include "Fb2ReaderActivity.h"

#include <Fb2Parser.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Epub/Page.h>
#include <Epub/blocks/ImageBlock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "GlobalActions.h"
#include "MappedInputManager.h"
#include "Fb2ReaderChapterSelectionActivity.h"
#include "Fb2ReaderMenuActivity.h"
#include "ReaderOptionsActivity.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/boot_sleep/SleepCoverAssets.h"
#include "components/UITheme.h"
#include "fontIds.h"

#define TAG "FB2R"

namespace {
constexpr uint32_t PROGRESS_MAGIC = 0x46423250;  // "FB2P"
constexpr uint8_t PROGRESS_VERSION = 1;
constexpr int INVALID_SECTION = -1;
constexpr unsigned long longPressMenuMs = 600;
}  // namespace

void Fb2ReaderActivity::onEnter() {
  Activity::onEnter();

  if (!fb2) {
    return;
  }

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  mappedInput.setReaderMode(true);

  fb2->setupCacheDir();

  auto filePath = fb2->getPath();
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  SleepCoverAssets::prepareFb2(*fb2);
  const std::string coverBmpPath = Storage.exists(fb2->getCoverBmpPath().c_str()) ? fb2->getCoverBmpPath() : "";
  RECENT_BOOKS.addOrUpdateBook(filePath, fb2->getTitle(), fb2->getAuthor(), coverBmpPath);

  requestUpdate();
}

void Fb2ReaderActivity::onExit() {
  Activity::onExit();

  mappedInput.setReaderMode(false);

  renderer.setOrientation(GfxRenderer::Orientation::Portrait);

  sectionPages.clear();
  currentSectionCached = INVALID_SECTION;
  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  // Clean up page cache on SD before releasing the Fb2 object
  if (fb2) {
    const std::string pageCacheDir = fb2->getCachePath() + "/pages";
    Storage.removeDir(pageCacheDir.c_str());
  }

  fb2.reset();
}

void Fb2ReaderActivity::loop() {
  // Back button: long press goes to file browser
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(fb2 ? fb2->getPath() : "");
    return;
  }

  // Back button: short press goes home
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    onGoHome();
    return;
  }

  // --- Side button long press: font size / orientation change ---
  const bool sideLongPressChangesFont =
      SETTINGS.sideButtonLongPress == CrossPointSettings::SIDE_LONG_FONT_SIZE;
  const bool sideLongPressChangesOrientation =
      SETTINGS.sideButtonLongPress == CrossPointSettings::SIDE_LONG_ORIENTATION_CHANGE;
  if (sideLongPressChangesFont || sideLongPressChangesOrientation) {
    const bool topReleased = mappedInput.wasReleased(MappedInputManager::Button::Up);
    const bool bottomReleased = mappedInput.wasReleased(MappedInputManager::Button::Down);
    if (sideButtonLongPressHandled && (topReleased || bottomReleased)) {
      sideButtonLongPressHandled = false;
      return;
    }

    const bool longPressReady = mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;
    const bool topLongPressed =
        longPressReady && (mappedInput.isPressed(MappedInputManager::Button::Up) || topReleased);
    const bool bottomLongPressed =
        longPressReady && (mappedInput.isPressed(MappedInputManager::Button::Down) || bottomReleased);

    if (!sideButtonLongPressHandled && topLongPressed) {
      sideButtonLongPressHandled = !topReleased;
      if (sideLongPressChangesFont) {
        if (sdFontSystem.changeReaderFontSize(/*larger=*/true)) {
          reindexCurrentSection();
        }
      } else {
        applyOrientation(ReaderUtils::rotatedOrientation(SETTINGS.orientation, /*clockwise=*/false));
        requestUpdate();
      }
      return;
    }
    if (!sideButtonLongPressHandled && bottomLongPressed) {
      sideButtonLongPressHandled = !bottomReleased;
      if (sideLongPressChangesFont) {
        if (sdFontSystem.changeReaderFontSize(/*larger=*/false)) {
          reindexCurrentSection();
        }
      } else {
        applyOrientation(ReaderUtils::rotatedOrientation(SETTINGS.orientation, /*clockwise=*/true));
        requestUpdate();
      }
      return;
    }
  }

  // --- Front button long press: chapter skip / orientation change ---
  const bool frontLongPressAction = SETTINGS.longPressButtonBehavior == CrossPointSettings::CHAPTER_SKIP ||
                                    SETTINGS.longPressButtonBehavior == CrossPointSettings::ORIENTATION_CHANGE;
  if (frontLongPressAction) {
    const bool leftReleased = mappedInput.wasReleased(MappedInputManager::Button::Left);
    const bool rightReleased = mappedInput.wasReleased(MappedInputManager::Button::Right);
    if (frontButtonLongPressHandled && (leftReleased || rightReleased)) {
      frontButtonLongPressHandled = false;
      return;
    }

    const bool longPressReady = mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;
    const bool prevLongPressed = longPressReady && mappedInput.isPressed(MappedInputManager::Button::Left);
    const bool nextLongPressed = longPressReady && mappedInput.isPressed(MappedInputManager::Button::Right);
    if (!frontButtonLongPressHandled && (prevLongPressed || nextLongPressed)) {
      frontButtonLongPressHandled = true;
      if (SETTINGS.longPressButtonBehavior == CrossPointSettings::CHAPTER_SKIP) {
        if (nextLongPressed) {
          if (currentSection < static_cast<int>(fb2->getSectionCount()) - 1) {
            currentSection++;
            currentSectionCached = INVALID_SECTION;
            currentPageInSection = 0;
          }
        } else if (currentSection > 0) {
          currentSection--;
          currentSectionCached = INVALID_SECTION;
          currentPageInSection = 0;
        }
        requestUpdate();
        return;
      }
      applyOrientation(ReaderUtils::rotatedOrientation(SETTINGS.orientation, prevLongPressed));
      requestUpdate();
      return;
    }
  }

  // --- Confirm long press: execute quick action (before menu) ---
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (longPressMenuHandled) {
      longPressMenuHandled = false;
      return;
    }
    if (SETTINGS.longPressMenuAction != CrossPointSettings::LONG_MENU_OFF &&
        mappedInput.getHeldTime() >= longPressMenuMs) {
      executeLongPressMenuAction();
      return;
    }
  }
  if (SETTINGS.longPressMenuAction != CrossPointSettings::LONG_MENU_OFF && !longPressMenuHandled &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) && mappedInput.getHeldTime() >= longPressMenuMs) {
    longPressMenuHandled = true;
    executeLongPressMenuAction();
    return;
  }

  // --- Short press Confirm: open reader menu ---
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const int sectionCount = static_cast<int>(fb2->getSectionCount());
    const std::string bookTitle = fb2->getTitle().empty() ? fb2->getPath() : fb2->getTitle();
    startActivityForResult(
        std::make_unique<Fb2ReaderMenuActivity>(renderer, mappedInput, bookTitle, currentPageInSection, totalPages,
                                                0, SETTINGS.orientation),
        [this](const ActivityResult& result) {
          if (result.isCancelled) {
            if (const auto* menuResult = std::get_if<MenuResult>(&result.data)) {
              if (menuResult->settingsChanged) {
                initialized = false;
                currentSectionCached = INVALID_SECTION;
                requestUpdate();
              } else if (menuResult->orientation != SETTINGS.orientation) {
                applyOrientation(menuResult->orientation);
                requestUpdate();
              }
            }
            return;
          }
          if (const auto* menuResult = std::get_if<MenuResult>(&result.data)) {
            const auto action = static_cast<Fb2ReaderMenuActivity::MenuAction>(menuResult->action);
            switch (action) {
              case Fb2ReaderMenuActivity::MenuAction::SELECT_SECTION:
                startActivityForResult(
                    std::make_unique<Fb2ReaderChapterSelectionActivity>(renderer, mappedInput, fb2.get(),
                                                                        currentSection),
                    [this](const ActivityResult& chapterResult) {
                      if (chapterResult.isCancelled) return;
                      if (const auto* cr = std::get_if<ChapterResult>(&chapterResult.data)) {
                        currentSection = cr->spineIndex;
                        currentSectionCached = INVALID_SECTION;
                        currentPageInSection = 0;
                        requestUpdate();
                      }
                    });
                return;
              case Fb2ReaderMenuActivity::MenuAction::GO_HOME:
                onGoHome();
                return;
              default:
                break;
            }
            if (menuResult->settingsChanged) {
              initialized = false;
              currentSectionCached = INVALID_SECTION;
              requestUpdate();
            } else if (menuResult->orientation != SETTINGS.orientation) {
              applyOrientation(menuResult->orientation);
              requestUpdate();
            }
          }
        });
    return;
  }

  // --- Power button actions ---
  if (consumeLongPowerButtonRelease()) {
    return;
  }
  if (executeShortPowerButtonAction()) {
    return;
  }
  if (executeLongPowerButtonAction()) {
    return;
  }

  // --- Page turn detection & chapter skip ---
  auto [prevTriggered, nextTriggered, fromSideBtn, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  if (SETTINGS.longPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN && consumeLongPowerButtonHold()) {
    nextTriggered = true;
    fromSideBtn = false;
    fromTilt = false;
  }
  if (!prevTriggered && !nextTriggered) {
    return;
  }

  const bool longPress = !fromTilt && mappedInput.getHeldTime() > ReaderUtils::SKIP_HOLD_MS;
  const bool skipChapter =
      longPress &&
      (fromSideBtn ? SETTINGS.sideButtonLongPress == CrossPointSettings::SIDE_LONG_CHAPTER_SKIP
                   : SETTINGS.longPressButtonBehavior == CrossPointSettings::CHAPTER_SKIP);

  if (skipChapter) {
    if (nextTriggered) {
      if (currentSection < static_cast<int>(fb2->getSectionCount()) - 1) {
        currentSection++;
      }
    } else if (currentSection > 0) {
      currentSection--;
    }
    currentSectionCached = INVALID_SECTION;
    currentPageInSection = 0;
    requestUpdate();
    return;
  }

  if (longPress && !fromSideBtn && SETTINGS.longPressButtonBehavior == CrossPointSettings::ORIENTATION_CHANGE) {
    const uint8_t newOrientation =
        nextTriggered ? (SETTINGS.orientation - 1 + SETTINGS.ORIENTATION_COUNT) % SETTINGS.ORIENTATION_COUNT
                      : (SETTINGS.orientation + 1) % SETTINGS.ORIENTATION_COUNT;
    applyOrientation(newOrientation);
    requestUpdate();
    return;
  }

  // --- Normal page turn ---
  if (prevTriggered) {
    if (currentPageInSection > 0) {
      currentPageInSection--;
      requestUpdate();
    } else if (currentSection > 0) {
      currentSection--;
      currentSectionCached = INVALID_SECTION;
      currentPageInSection = 0;
      requestUpdate();
    }
  } else if (nextTriggered) {
    if (currentPageInSection < totalPages - 1) {
      currentPageInSection++;
      requestUpdate();
    } else if (currentSection < static_cast<int>(fb2->getSectionCount()) - 1) {
      currentSection++;
      currentSectionCached = INVALID_SECTION;
      currentPageInSection = 0;
      requestUpdate();
    } else {
      onGoHome();
    }
  }
}

void Fb2ReaderActivity::initializeReader() {
  if (initialized) {
    return;
  }

  cachedFontId = SETTINGS.getReaderFontId();
  const uint8_t cachedScreenMargin = SETTINGS.screenMargin;

  renderer.getOrientedViewableTRBL(&cachedOrientedMarginTop, &cachedOrientedMarginRight, &cachedOrientedMarginBottom,
                                   &cachedOrientedMarginLeft);
  cachedOrientedMarginLeft += cachedScreenMargin;
  cachedOrientedMarginRight += cachedScreenMargin;
  const int topStatusBarReservedHeight = ReaderUtils::getTopClockStatusBarReservedHeight();
  if (topStatusBarReservedHeight > 0) {
    cachedOrientedMarginTop += std::max(static_cast<int>(cachedScreenMargin),
                                        topStatusBarReservedHeight + ReaderUtils::STATUS_BAR_TEXT_PADDING);
  } else {
    cachedOrientedMarginTop += cachedScreenMargin;
  }
  cachedOrientedMarginBottom += std::max(
      cachedScreenMargin,
      static_cast<uint8_t>(UITheme::getInstance().getStatusBarHeight() + ReaderUtils::STATUS_BAR_TEXT_PADDING));

  viewportWidth = renderer.getScreenWidth() - cachedOrientedMarginLeft - cachedOrientedMarginRight;
  viewportHeight = renderer.getScreenHeight() - cachedOrientedMarginTop - cachedOrientedMarginBottom;
  if (viewportHeight < 1) viewportHeight = 1;
  if (viewportWidth < 1) viewportWidth = 1;

  lineCompression = SETTINGS.getReaderLineCompression();
  extraParagraphSpacing = SETTINGS.extraParagraphSpacing;
  forceParagraphIndents = SETTINGS.forceParagraphIndents;
  paragraphAlignment = SETTINGS.paragraphAlignment;
  hyphenationEnabled = SETTINGS.hyphenationEnabled;
  bionicReadingEnabled = SETTINGS.bionicReadingEnabled;
  guideReadingEnabled = SETTINGS.guideReadingEnabled;

  if (!fb2->isLoaded()) {
    if (!fb2->load()) {
      LOG_ERR(TAG, "Failed to load FB2 file");
    }
  }

  loadProgress();
  initialized = true;
}

void Fb2ReaderActivity::buildSectionPages(int sectionIndex) {
  sectionPages.clear();
  currentSectionCached = sectionIndex;
  currentPageInSection = 0;

  if (sectionIndex < 0 || sectionIndex >= static_cast<int>(fb2->getSectionCount())) {
    totalPages = 0;
    return;
  }

  const std::string sectionPath = fb2->getSectionPath(sectionIndex);
  if (!Storage.exists(sectionPath.c_str())) {
    LOG_ERR(TAG, "Section file not found: %s", sectionPath.c_str());
    totalPages = 0;
    return;
  }

  CssTextAlign align = CssTextAlign::Justify;
  switch (paragraphAlignment) {
    case CrossPointSettings::LEFT_ALIGN:
      align = CssTextAlign::Left;
      break;
    case CrossPointSettings::CENTER_ALIGN:
      align = CssTextAlign::Center;
      break;
    case CrossPointSettings::RIGHT_ALIGN:
      align = CssTextAlign::Right;
      break;
    case CrossPointSettings::JUSTIFIED:
    default:
      align = CssTextAlign::Justify;
      break;
  }

  // Clean old page cache and create fresh directory
  const std::string pageCacheDir = fb2->getCachePath() + "/pages";
  Storage.removeDir(pageCacheDir.c_str());
  Storage.mkdir(pageCacheDir.c_str());

  int pageCount = 0;

  // Add cover image as first page for section 0
  if (sectionIndex == 0) {
    std::string coverPath = fb2->getCoverBmpPath();
    if (!coverPath.empty() && Storage.exists(coverPath.c_str())) {
      auto coverPage = std::make_unique<Page>();
      auto imgBlock = std::make_shared<ImageBlock>(coverPath, renderer.getScreenWidth(),
                                                   renderer.getScreenHeight());
      coverPage->elements.push_back(std::make_shared<PageImage>(imgBlock, 0, 0));
      char path[128];
      snprintf(path, sizeof(path), "%s/page_%03d.dat", pageCacheDir.c_str(), 0);
      HalFile cacheFile;
      if (Storage.openFileForWrite("FB2_CACHE", path, cacheFile)) {
        coverPage->serialize(cacheFile);
        cacheFile.close();
      }
      pageCount = 1;
    }
  }

  Fb2Parser parser(sectionPath, renderer, cachedFontId, lineCompression, extraParagraphSpacing,
                   forceParagraphIndents, align, viewportWidth, viewportHeight, hyphenationEnabled,
                   bionicReadingEnabled, guideReadingEnabled,
                   [&pageCacheDir, &pageCount](std::unique_ptr<Page> page, uint16_t, uint16_t) {
                     char path[128];
                     snprintf(path, sizeof(path), "%s/page_%03d.dat", pageCacheDir.c_str(), pageCount);
                     HalFile cacheFile;
                     if (Storage.openFileForWrite("FB2_CACHE", path, cacheFile)) {
                       page->serialize(cacheFile);
                       cacheFile.close();
                     } else {
                       LOG_ERR(TAG, "Failed to write page cache: %s", path);
                     }
                     pageCount++;
                   });

  if (!parser.parseAndBuildPages()) {
    LOG_ERR(TAG, "Failed to parse section %d", sectionIndex);
  }

  totalPages = pageCount;
  if (totalPages < 1) totalPages = 1;
}

void Fb2ReaderActivity::loadPageFromCache(int pageIndex) {
  sectionPages.clear();
  if (pageIndex < 0 || pageIndex >= totalPages) return;

  const std::string pageCacheDir = fb2->getCachePath() + "/pages";
  char path[128];
  snprintf(path, sizeof(path), "%s/page_%03d.dat", pageCacheDir.c_str(), pageIndex);

  HalFile cacheFile;
  if (!Storage.openFileForRead("FB2_CACHE", path, cacheFile)) {
    LOG_ERR(TAG, "Failed to open page cache: %s", path);
    return;
  }

  auto page = Page::deserialize(cacheFile);
  cacheFile.close();

  if (!page) {
    LOG_ERR(TAG, "Failed to deserialize page %d", pageIndex);
    return;
  }

  sectionPages.push_back(std::move(page));
}

void Fb2ReaderActivity::render(RenderLock&&) {
  if (!fb2) {
    return;
  }

  if (!initialized) {
    initializeReader();
  }

  if (fb2->getSectionCount() == 0) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  if (currentSection >= static_cast<int>(fb2->getSectionCount())) {
    currentSection = fb2->getSectionCount() - 1;
  }
  if (currentSection < 0) currentSection = 0;

  if (currentSectionCached != currentSection) {
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
    buildSectionPages(currentSection);
  }

  if (currentPageInSection >= totalPages) {
    currentPageInSection = totalPages - 1;
  }
  if (currentPageInSection < 0) currentPageInSection = 0;

  renderPage();
  saveProgress();
}

void Fb2ReaderActivity::renderPage() {
  // Ensure the requested page is loaded into memory
  loadPageFromCache(currentPageInSection);

  if (sectionPages.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderStatusBar();
    GUI.drawTopStatusBarClock(renderer);
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
    return;
  }

  renderer.clearScreen();

  // Use font prewarm for SD card fonts
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();

  // Scan pass — measure text without drawing
  sectionPages[0]->renderText(renderer, cachedFontId, cachedOrientedMarginLeft,
                              cachedOrientedMarginTop);
  scope.endScanAndPrewarm();

  // BW render pass
  sectionPages[0]->renderText(renderer, cachedFontId, cachedOrientedMarginLeft,
                              cachedOrientedMarginTop);
  sectionPages[0]->renderImages(renderer, cachedFontId, cachedOrientedMarginLeft,
                                cachedOrientedMarginTop);

  renderStatusBar();
  GUI.drawTopStatusBarClock(renderer);

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::renderAntiAliased(renderer, [this]() {
      sectionPages[0]->renderText(renderer, cachedFontId, cachedOrientedMarginLeft,
                                  cachedOrientedMarginTop);
    });
  }
}

void Fb2ReaderActivity::renderStatusBar() const {
  const int sectionCount = static_cast<int>(fb2->getSectionCount());
  int globalPage = currentPageInSection;
  int globalTotal = totalPages;
  for (int s = 0; s < currentSection; s++) {
    // Rough estimate: use cached section count
    globalPage += 1;  // placeholder - just show section progress
    globalTotal += 1;
  }
  const float progress = sectionCount > 0 ? (currentSection * 100.0f / sectionCount) : 0;
  std::string title;
  if (SETTINGS.statusBarTitle != CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE) {
    title = fb2->getTitle();
  }
  GUI.drawStatusBar(renderer, progress, currentPageInSection + 1, totalPages, title);
}

void Fb2ReaderActivity::saveProgress() const {
  HalFile f;
  if (Storage.openFileForWrite("FB2R", fb2->getCachePath() + "/progress.bin", f)) {
    serialization::writePod(f, PROGRESS_MAGIC);
    serialization::writePod(f, PROGRESS_VERSION);
    serialization::writePod(f, static_cast<uint16_t>(currentSection));
    serialization::writePod(f, static_cast<uint16_t>(currentPageInSection));
    f.close();
  }
}

void Fb2ReaderActivity::loadProgress() {
  HalFile f;
  if (Storage.openFileForRead("FB2R", fb2->getCachePath() + "/progress.bin", f)) {
    uint32_t magic;
    serialization::readPod(f, magic);
    if (magic != PROGRESS_MAGIC) {
      f.close();
      return;
    }
    uint8_t version;
    serialization::readPod(f, version);
    if (version != PROGRESS_VERSION) {
      f.close();
      return;
    }
    uint16_t sec, page;
    serialization::readPod(f, sec);
    serialization::readPod(f, page);
    currentSection = sec;
    currentPageInSection = page;
    f.close();
    LOG_DBG(TAG, "Loaded progress: section %d, page %d", currentSection, currentPageInSection);
  }
}

void Fb2ReaderActivity::reindexCurrentSection() {
  currentSectionCached = INVALID_SECTION;
  initialized = false;
  requestUpdate();
}

void Fb2ReaderActivity::applyOrientation(uint8_t orientation) {
  if (SETTINGS.orientation == orientation) return;
  SETTINGS.orientation = orientation;
  SETTINGS.saveToFile();
  RenderLock lock(*this);
  ReaderUtils::applyOrientation(renderer, orientation);
  sectionPages.clear();
  currentSectionCached = INVALID_SECTION;
  initialized = false;
}

void Fb2ReaderActivity::executeReaderQuickAction(CrossPointSettings::LONG_PRESS_MENU_ACTION action) {
  switch (action) {
    case CrossPointSettings::LONG_MENU_SLEEP:
      enterDeepSleep();
      break;
    case CrossPointSettings::LONG_MENU_CHANGE_FONT:
      SETTINGS.fontFamily = (SETTINGS.fontFamily + 1) % CrossPointSettings::FONT_FAMILY_COUNT;
      reindexCurrentSection();
      break;
    case CrossPointSettings::LONG_MENU_TOGGLE_GUIDE_DOTS:
      SETTINGS.guideReadingEnabled = !SETTINGS.guideReadingEnabled;
      reindexCurrentSection();
      break;
    case CrossPointSettings::LONG_MENU_TOGGLE_BIONIC:
      SETTINGS.bionicReadingEnabled = !SETTINGS.bionicReadingEnabled;
      reindexCurrentSection();
      break;
    case CrossPointSettings::LONG_MENU_REFRESH_SCREEN:
      pagesUntilFullRefresh = 1;
      requestUpdate();
      break;
    case CrossPointSettings::LONG_MENU_TOGGLE_TILT_PAGE_TURN:
      SETTINGS.tiltPageTurn = SETTINGS.tiltPageTurn == CrossPointSettings::TILT_OFF
                                  ? CrossPointSettings::TILT_ON
                                  : CrossPointSettings::TILT_OFF;
      SETTINGS.saveToFile();
      requestUpdate();
      break;
    default:
      break;
  }
}

void Fb2ReaderActivity::executeLongPressMenuAction() {
  executeReaderQuickAction(static_cast<CrossPointSettings::LONG_PRESS_MENU_ACTION>(SETTINGS.longPressMenuAction));
}

bool Fb2ReaderActivity::consumeLongPowerButtonRelease() {
  if (!mappedInput.wasReleased(MappedInputManager::Button::Power) || !longPowerButtonHandled) {
    return false;
  }
  longPowerButtonHandled = false;
  return true;
}

bool Fb2ReaderActivity::consumeLongPowerButtonHold() {
  if (longPowerButtonHandled || !mappedInput.isPressed(MappedInputManager::Button::Power) ||
      mappedInput.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration()) {
    return false;
  }
  longPowerButtonHandled = true;
  return true;
}

bool Fb2ReaderActivity::executeShortPowerButtonAction() {
  if (!mappedInput.wasReleased(MappedInputManager::Button::Power) ||
      mappedInput.getHeldTime() >= SETTINGS.getPowerButtonLongPressDuration()) {
    return false;
  }
  switch (SETTINGS.shortPwrBtn) {
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FONT:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_CHANGE_FONT);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_GUIDE_DOTS:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_GUIDE_DOTS);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_BIONIC_READING:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_BIONIC);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_BOOKMARK:
    case CrossPointSettings::SHORT_PWRBTN::SYNC_PROGRESS:
    case CrossPointSettings::SHORT_PWRBTN::MARK_FINISHED:
    case CrossPointSettings::SHORT_PWRBTN::READING_STATS:
    case CrossPointSettings::SHORT_PWRBTN::SCREENSHOT:
    case CrossPointSettings::SHORT_PWRBTN::CYCLE_PAGE_TURN:
    case CrossPointSettings::SHORT_PWRBTN::FILE_TRANSFER:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TILT_PAGE_TURN:
      // Not yet supported in FB2 reader
      return false;
    default:
      return false;
  }
}

bool Fb2ReaderActivity::executeLongPowerButtonAction() {
  if (SETTINGS.longPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN || !consumeLongPowerButtonHold()) {
    return false;
  }
  switch (SETTINGS.longPwrBtn) {
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FONT:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_CHANGE_FONT);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_GUIDE_DOTS:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_GUIDE_DOTS);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_BIONIC_READING:
      executeReaderQuickAction(CrossPointSettings::LONG_MENU_TOGGLE_BIONIC);
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_BOOKMARK:
    case CrossPointSettings::SHORT_PWRBTN::SYNC_PROGRESS:
    case CrossPointSettings::SHORT_PWRBTN::MARK_FINISHED:
    case CrossPointSettings::SHORT_PWRBTN::READING_STATS:
    case CrossPointSettings::SHORT_PWRBTN::SCREENSHOT:
    case CrossPointSettings::SHORT_PWRBTN::CYCLE_PAGE_TURN:
    case CrossPointSettings::SHORT_PWRBTN::FILE_TRANSFER:
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TILT_PAGE_TURN:
      return false;
    default:
      return false;
  }
}

ScreenshotInfo Fb2ReaderActivity::getScreenshotInfo() const {
  ScreenshotInfo info;
  info.readerType = ScreenshotInfo::ReaderType::Fb2;
  std::string t = fb2->getTitle();
  size_t copyLen = std::min(t.size(), sizeof(info.title) - 1);
  memcpy(info.title, t.data(), copyLen);
  info.title[copyLen] = '\0';
  info.currentPage = currentPageInSection + 1;
  info.totalPages = totalPages;
  info.progressPercent = fb2->getSectionCount() > 0 ? (currentSection * 100 / fb2->getSectionCount()) : 0;
  return info;
}
