#include "ProfileStore.h"

#include <HalStorage.h>
#include <I18n.h>
#include <JsonSettingsIO.h>
#include <Logging.h>

#include <algorithm>

#include "BookmarkStore.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"

ProfileStore ProfileStore::instance;

namespace {
constexpr char PROFILES_FILE_JSON[] = "/.crosspoint/profiles.json";
constexpr char DEFAULT_PROFILE_ID[] = "default";
}

bool ProfileStore::saveToFile() const {
  Storage.mkdir("/.crosspoint");
  Storage.mkdir("/.crosspoint/profiles");
  return JsonSettingsIO::saveProfiles(*this, PROFILES_FILE_JSON);
}

bool ProfileStore::loadFromFile() {
  if (Storage.exists(PROFILES_FILE_JSON)) {
    String json = Storage.readFile(PROFILES_FILE_JSON);
    if (!json.isEmpty()) {
      bool result = JsonSettingsIO::loadProfiles(*this, json.c_str());
      if (result) {
        LOG_DBG("PROF", "Loaded profiles from JSON");
        return true;
      }
    }
  }

  LOG_DBG("PROF", "No profiles file found, will create default on first use");
  return false;
}

bool ProfileStore::ensureDefaultProfile() {
  if (!profiles.empty()) {
    if (!activeProfileId.empty()) {
      return getProfileById(activeProfileId) != nullptr;
    }
    activeProfileId = profiles[0].id;
    return true;
  }

  profiles.push_back({DEFAULT_PROFILE_ID, tr(STR_PROFILE_DEFAULT)});
  activeProfileId = DEFAULT_PROFILE_ID;
  return saveToFile();
}

const Profile* ProfileStore::getActiveProfile() const {
  return getProfileById(activeProfileId);
}

const Profile* ProfileStore::getProfileById(const std::string& id) const {
  const auto it = std::find_if(profiles.begin(), profiles.end(),
                               [&id](const Profile& p) { return p.id == id; });
  return (it != profiles.end()) ? &*it : nullptr;
}

const std::string& ProfileStore::getActiveProfileName() const {
  const Profile* p = getActiveProfile();
  if (p) return p->name;
  static const std::string empty;
  return empty;
}

bool ProfileStore::setActiveProfile(const std::string& id) {
  if (activeProfileId == id) return true;
  if (!getProfileById(id)) {
    LOG_ERR("PROF", "Cannot set active profile: id '%s' not found", id.c_str());
    return false;
  }
  activeProfileId = id;
  if (!saveToFile()) return false;

  LOG_DBG("PROF", "Reloading profile-scoped stores for profile '%s'", id.c_str());
  RECENT_BOOKS.loadFromFile();
  APP_STATE.loadFromFile();
  BOOKMARKS.unload();
  return true;
}

bool ProfileStore::addProfile(const std::string& name) {
  if (profiles.size() >= MAX_PROFILES) {
    LOG_ERR("PROF", "Cannot add profile: limit of %zu reached", MAX_PROFILES);
    return false;
  }

  for (const auto& p : profiles) {
    if (p.name == name) {
      LOG_ERR("PROF", "Profile name '%s' already exists", name.c_str());
      return false;
    }
  }

  char buf[32];
  snprintf(buf, sizeof(buf), "profile_%zu", profiles.size());
  Profile prof;
  prof.id = buf;
  prof.name = name;
  profiles.push_back(prof);
  Storage.mkdir("/.crosspoint/profiles");
  Storage.mkdir(("/.crosspoint/profiles/" + prof.id).c_str());

  LOG_DBG("PROF", "Added profile '%s' (id=%s)", name.c_str(), prof.id.c_str());
  return saveToFile();
}

bool ProfileStore::removeProfile(const std::string& id) {
  if (activeProfileId == id) {
    LOG_ERR("PROF", "Cannot remove active profile");
    return false;
  }

  if (profiles.size() <= 1) {
    LOG_ERR("PROF", "Cannot remove last profile");
    return false;
  }

  const auto it = std::remove_if(profiles.begin(), profiles.end(),
                                  [&id](const Profile& p) { return p.id == id; });
  if (it == profiles.end()) {
    LOG_ERR("PROF", "Profile '%s' not found for removal", id.c_str());
    return false;
  }

  profiles.erase(it, profiles.end());
  LOG_DBG("PROF", "Removed profile '%s'", id.c_str());
  return saveToFile();
}

bool ProfileStore::renameProfile(const std::string& id, const std::string& newName) {
  const Profile* p = getProfileById(id);
  if (!p) {
    LOG_ERR("PROF", "Profile '%s' not found for rename", id.c_str());
    return false;
  }

  for (const auto& other : profiles) {
    if (other.id != id && other.name == newName) {
      LOG_ERR("PROF", "Profile name '%s' already exists", newName.c_str());
      return false;
    }
  }

  Profile& mutableProf = const_cast<Profile&>(*p);
  mutableProf.name = newName;
  LOG_DBG("PROF", "Renamed profile '%s' to '%s'", id.c_str(), newName.c_str());
  return saveToFile();
}

std::string ProfileStore::getProfileCacheBase() const {
  if (activeProfileId == DEFAULT_PROFILE_ID) {
    return "/.crosspoint";
  }
  return "/.crosspoint/profiles/" + activeProfileId;
}

std::string ProfileStore::getProfileBookmarksDir() const {
  return getProfileCacheBase() + "/bookmarks";
}

std::string ProfileStore::getProfileGlobalStatsPath() const {
  return getProfileCacheBase() + "/global_stats.bin";
}

std::string ProfileStore::getProfileGlobalStatsBakPath() const {
  return getProfileCacheBase() + "/global_stats.bin.bak";
}

std::string ProfileStore::getProfileRecentBooksPath() const {
  return getProfileCacheBase() + "/recent.json";
}

std::string ProfileStore::getProfileStatePath() const {
  return getProfileCacheBase() + "/state.json";
}
