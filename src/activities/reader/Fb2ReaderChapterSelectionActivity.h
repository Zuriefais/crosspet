#pragma once
#include <Fb2.h>

#include <memory>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class Fb2ReaderChapterSelectionActivity final : public Activity {
  Fb2* fb2;
  ButtonNavigator buttonNavigator;
  int currentSectionIndex = 0;
  int selectorIndex = 0;

  int getTotalItems() const;

 public:
  explicit Fb2ReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Fb2* fb2,
                                             const int currentSectionIndex)
      : Activity("Fb2ReaderChapterSelection", renderer, mappedInput),
        fb2(fb2),
        currentSectionIndex(currentSectionIndex) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
};
