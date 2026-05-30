#include "Fb2ReaderActivity.h"

#include <Fb2Parser.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "activities/boot_sleep/SleepCoverAssets.h"
#include "components/UITheme.h"
#include "fontIds.h"

#define TAG "FB2R"

namespace {
constexpr uint32_t PROGRESS_MAGIC = 0x46423250;  // "FB2P"
constexpr uint8_t PROGRESS_VERSION = 1;
constexpr int INVALID_SECTION = -1;
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
  auto fileName = filePath.substr(filePath.rfind('/') + 1);
  APP_STATE.openEpubPath = filePath;
  APP_STATE.saveToFile();
  SleepCoverAssets::prepareFb2(*fb2);
  const std::string coverBmpPath = Storage.exists(fb2->getCoverBmpPath().c_str()) ? fb2->getCoverBmpPath() : "";
  RECENT_BOOKS.addOrUpdateBook(filePath, fileName, "", coverBmpPath);

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
  fb2.reset();
}

void Fb2ReaderActivity::loop() {
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= ReaderUtils::GO_HOME_MS) {
    activityManager.goToFileBrowser(fb2 ? fb2->getPath() : "");
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back) &&
      mappedInput.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    onGoHome();
    return;
  }

  if (SETTINGS.sideButtonLongPress == CrossPointSettings::SIDE_LONG_PRESS::SIDE_LONG_ORIENTATION_CHANGE) {
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

    if (!sideButtonLongPressHandled && (topLongPressed || bottomLongPressed)) {
      sideButtonLongPressHandled = !(topReleased || bottomReleased);
      SETTINGS.orientation = ReaderUtils::rotatedOrientation(SETTINGS.orientation, bottomLongPressed);
      SETTINGS.saveToFile();
      {
        RenderLock lock(*this);
        ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
        sectionPages.clear();
        currentSectionCached = INVALID_SECTION;
        initialized = false;
      }
      requestUpdate();
      return;
    }
  }

  if (SETTINGS.longPressButtonBehavior == CrossPointSettings::ORIENTATION_CHANGE) {
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
      SETTINGS.orientation = ReaderUtils::rotatedOrientation(SETTINGS.orientation, prevLongPressed);
      SETTINGS.saveToFile();
      {
        RenderLock lock(*this);
        ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
        sectionPages.clear();
        currentSectionCached = INVALID_SECTION;
        initialized = false;
      }
      requestUpdate();
      return;
    }
  }

  auto [prevTriggered, nextTriggered, fromSideBtn, fromTilt] = ReaderUtils::detectPageTurn(mappedInput);
  (void)fromSideBtn;
  (void)fromTilt;
  if (!prevTriggered && !nextTriggered) {
    return;
  }

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
    const int pagesInSection = static_cast<int>(sectionPages.size());
    if (currentPageInSection < pagesInSection - 1) {
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

  Fb2Parser parser(sectionPath, renderer, cachedFontId, lineCompression, extraParagraphSpacing,
                   forceParagraphIndents, align, viewportWidth, viewportHeight, hyphenationEnabled,
                   bionicReadingEnabled, guideReadingEnabled,
                   [this](std::unique_ptr<Page> page, uint16_t, uint16_t) {
                     totalPages = static_cast<int>(sectionPages.size()) + 1;
                     sectionPages.push_back(std::move(page));
                   });

  if (!parser.parseAndBuildPages()) {
    LOG_ERR(TAG, "Failed to parse section %d", sectionIndex);
  }

  totalPages = static_cast<int>(sectionPages.size());
  if (totalPages < 1) totalPages = 1;
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

  if (currentSectionCached != currentSection || sectionPages.empty()) {
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
  if (sectionPages.empty()) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_EMPTY_FILE), true, EpdFontFamily::BOLD);
    renderStatusBar();
    GUI.drawTopStatusBarClock(renderer);
    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);
    return;
  }

  if (currentPageInSection >= static_cast<int>(sectionPages.size())) {
    currentPageInSection = static_cast<int>(sectionPages.size()) - 1;
  }

  renderer.clearScreen();

  // Use font prewarm for SD card fonts
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();

  // Scan pass — measure text without drawing
  sectionPages[currentPageInSection]->renderText(renderer, cachedFontId, cachedOrientedMarginLeft,
                                                  cachedOrientedMarginTop);
  scope.endScanAndPrewarm();

  // BW render pass
  sectionPages[currentPageInSection]->renderText(renderer, cachedFontId, cachedOrientedMarginLeft,
                                                  cachedOrientedMarginTop);

  renderStatusBar();
  GUI.drawTopStatusBarClock(renderer);

  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  if (SETTINGS.textAntiAliasing) {
    ReaderUtils::renderAntiAliased(renderer, [this]() {
      sectionPages[currentPageInSection]->renderText(renderer, cachedFontId, cachedOrientedMarginLeft,
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
