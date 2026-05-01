#pragma once
#include <I18n.h>

#include <string>
#include <vector>

#include "ProfileStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class ProfileSelectActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  std::vector<const Profile*> profileList;

  void rebuildList();
  void renderProfileList(RenderLock&& rl);
  void renderAddProfilePrompt(RenderLock&& rl);
  void renderDeleteConfirm(RenderLock&& rl);
  void renderRenamePrompt(RenderLock&& rl);

  enum class Mode { LIST, ADD, DELETE, RENAME };
  Mode currentMode = Mode::LIST;

  char renameBuffer[64] = {};
  int renameCursor = 0;

 public:
  explicit ProfileSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("ProfileSelect", renderer, mappedInput) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
};
