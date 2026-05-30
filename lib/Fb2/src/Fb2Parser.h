#pragma once

#include <expat.h>

#include <climits>
#include <functional>
#include <memory>
#include <string>

#include <Epub/ParsedText.h>
#include <Epub/blocks/BlockStyle.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/css/CssStyle.h>

class Page;
class GfxRenderer;

class Fb2Parser {
 public:
  Fb2Parser(const std::string& filepath, GfxRenderer& renderer, int fontId, float lineCompression,
            bool extraParagraphSpacing, bool forceParagraphIndents, CssTextAlign paragraphAlignment,
            uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
            bool bionicReadingEnabled, bool guideReadingEnabled,
            const std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t)>& completePageFn);
  ~Fb2Parser();

  bool parseAndBuildPages();

  const std::vector<std::pair<std::string, uint16_t>>& getAnchors() const { return anchorMap_; }

 private:
  const std::string& filepath_;
  GfxRenderer& renderer_;
  int fontId_;
  float lineCompression_;
  bool extraParagraphSpacing_;
  bool forceParagraphIndents_;
  CssTextAlign paragraphAlignment_;
  uint16_t viewportWidth_;
  uint16_t viewportHeight_;
  bool hyphenationEnabled_;
  bool bionicReadingEnabled_;
  bool guideReadingEnabled_;

  std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t)> completePageFn_;

  // Expat state
  XML_Parser xmlParser_ = nullptr;
  bool stopRequested_ = false;

  // XML depth tracking
  int depth_ = 0;
  int skipUntilDepth_ = INT_MAX;
  int boldUntilDepth_ = INT_MAX;
  int italicUntilDepth_ = INT_MAX;
  bool inBody_ = false;
  bool inTitle_ = false;
  bool inSubtitle_ = false;
  bool inParagraph_ = false;
  int bodyCount_ = 0;
  int sectionCounter_ = 0;
  bool firstSection_ = true;

  // Word buffer
  static constexpr int MAX_WORD_SIZE = 200;
  char partWordBuffer_[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex_ = 0;

  // Page building
  std::unique_ptr<ParsedText> currentTextBlock_;
  std::unique_ptr<Page> currentPage_;
  int16_t currentPageNextY_ = 0;

  // Anchor map for TOC navigation
  std::vector<std::pair<std::string, uint16_t>> anchorMap_;

  // Callback state
  uint16_t pagesCreated_ = 0;
  uint16_t maxPages_ = 0;
  bool hitMaxPages_ = false;
  int16_t pendingSpacing_ = 0;

  // File reading
  HalFile resumeFile_;
  size_t bomSkip_ = 0;

  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL endElement(void* userData, const XML_Char* name);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);

  void flushPartWordBuffer();
  void startNewTextBlock(CssTextAlign align);
  void makePages();
  void addLineToPage(std::shared_ptr<TextBlock> line);
  void startNewPage();
  EpdFontFamily::Style getCurrentFontFamily() const;
  void addVerticalSpacing(int lines);
};
