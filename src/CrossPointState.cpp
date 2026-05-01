#include "CrossPointState.h"

#include <HalStorage.h>
#include <JsonSettingsIO.h>
#include <Logging.h>
#include <Serialization.h>

#include "ProfileStore.h"

#include <algorithm>
#include <cstring>

namespace {
constexpr uint8_t STATE_FILE_VERSION = 5;

std::string getStateJsonPath() {
  return PROFILE_STORE.getProfileStatePath();
}

std::string getStateBinPath() {
  std::string jsonPath = getStateJsonPath();
  const size_t dotPos = jsonPath.rfind('.');
  return (dotPos != std::string::npos ? jsonPath.substr(0, dotPos) : jsonPath) + ".bin";
}

std::string getStateBakPath() {
  return getStateBinPath() + ".bak";
}
}  // namespace

CrossPointState CrossPointState::instance;

bool CrossPointState::isRecentSleep(uint16_t idx, uint8_t checkCount) const {
  const uint8_t effectiveCount = std::min(checkCount, recentSleepFill);
  for (uint8_t i = 0; i < effectiveCount; i++) {
    const uint8_t slot = (recentSleepPos + SLEEP_RECENT_COUNT - 1 - i) % SLEEP_RECENT_COUNT;
    if (recentSleepImages[slot] == idx) return true;
  }
  return false;
}

void CrossPointState::pushRecentSleep(uint16_t idx) {
  recentSleepImages[recentSleepPos] = idx;
  recentSleepPos = (recentSleepPos + 1) % SLEEP_RECENT_COUNT;
  if (recentSleepFill < SLEEP_RECENT_COUNT) recentSleepFill++;
}

bool CrossPointState::saveToFile() const {
  const std::string jsonPath = getStateJsonPath();
  const size_t slash = jsonPath.rfind('/');
  if (slash != std::string::npos) {
    Storage.mkdir(jsonPath.substr(0, slash).c_str());
  }
  return JsonSettingsIO::saveState(*this, jsonPath.c_str());
}

bool CrossPointState::loadFromFile() {
  const std::string jsonPath = getStateJsonPath();
  const std::string binPath = getStateBinPath();
  const std::string bakPath = getStateBakPath();

  // Try JSON first
  if (Storage.exists(jsonPath.c_str())) {
    String json = Storage.readFile(jsonPath.c_str());
    if (!json.isEmpty()) {
      return JsonSettingsIO::loadState(*this, json.c_str());
    }
  }

  // Fall back to binary migration
  if (Storage.exists(binPath.c_str())) {
    if (loadFromBinaryFile(binPath.c_str())) {
      if (saveToFile()) {
        Storage.rename(binPath.c_str(), bakPath.c_str());
        LOG_DBG("CPS", "Migrated state.bin to state.json");
        return true;
      } else {
        LOG_ERR("CPS", "Failed to save state during migration");
        return false;
      }
    }
  }

  return false;
}

bool CrossPointState::loadFromBinaryFile(const char* stateBin) {
  HalFile inputFile;
  if (!Storage.openFileForRead("CPS", stateBin, inputFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version > STATE_FILE_VERSION) {
    LOG_ERR("CPS", "Deserialization failed: Unknown version %u", version);
    return false;
  }

  serialization::readString(inputFile, openEpubPath);
  if (version >= 2) {
    uint8_t legacyLastSleep = UINT8_MAX;
    serialization::readPod(inputFile, legacyLastSleep);
    if (legacyLastSleep != UINT8_MAX) {
      pushRecentSleep(static_cast<uint16_t>(legacyLastSleep));
    }
  }

  if (version >= 3) {
    serialization::readPod(inputFile, readerActivityLoadCount);
  }

  if (version >= 4) {
    serialization::readPod(inputFile, lastSleepFromReader);
  } else {
    lastSleepFromReader = false;
  }

  if (version >= 5) {
    serialization::readPod(inputFile, pendingBookmarkSpine);
    serialization::readPod(inputFile, pendingBookmarkProgress);
    pendingBookmarkParagraphIndex = UINT16_MAX;
  } else {
    pendingBookmarkSpine = UINT16_MAX;
    pendingBookmarkProgress = -1.0f;
    pendingBookmarkParagraphIndex = UINT16_MAX;
  }

  return true;
}
