#pragma once
#include <string>
#include <vector>

struct Profile {
  std::string id;
  std::string name;

  Profile() = default;
  Profile(std::string_view id_, std::string_view name_) : id(id_), name(name_) {}
};

class ProfileStore;
namespace JsonSettingsIO {
bool saveProfiles(const ProfileStore& store, const char* path);
bool loadProfiles(ProfileStore& store, const char* json);
}

class ProfileStore {
 private:
  static ProfileStore instance;
  std::vector<Profile> profiles;
  std::string activeProfileId;

  static constexpr size_t MAX_PROFILES = 8;

  ProfileStore() = default;

  friend bool JsonSettingsIO::saveProfiles(const ProfileStore&, const char*);
  friend bool JsonSettingsIO::loadProfiles(ProfileStore&, const char*);

 public:
  ProfileStore(const ProfileStore&) = delete;
  ProfileStore& operator=(const ProfileStore&) = delete;

  static ProfileStore& getInstance() { return instance; }

  bool saveToFile() const;
  bool loadFromFile();

  const std::vector<Profile>& getProfiles() const { return profiles; }
  size_t getCount() const { return profiles.size(); }

  const Profile* getActiveProfile() const;
  const Profile* getProfileById(const std::string& id) const;
  const std::string& getActiveProfileId() const { return activeProfileId; }
  const std::string& getActiveProfileName() const;

  bool setActiveProfile(const std::string& id);
  bool addProfile(const std::string& name);
  bool removeProfile(const std::string& id);
  bool renameProfile(const std::string& id, const std::string& newName);

  std::string getProfileCacheBase() const;
  std::string getProfileBookmarksDir() const;
  std::string getProfileGlobalStatsPath() const;
  std::string getProfileGlobalStatsBakPath() const;
  std::string getProfileRecentBooksPath() const;
  std::string getProfileStatePath() const;

  bool ensureDefaultProfile();
};

#define PROFILE_STORE ProfileStore::getInstance()
