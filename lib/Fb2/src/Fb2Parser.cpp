#include "Fb2Parser.h"

#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <Logging.h>

#include <cstring>
#include <utility>

#define TAG "FB2_PARSE"

namespace {
constexpr size_t READ_CHUNK_SIZE = 4096;

bool isWhitespace(char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

const char* stripNamespace(const char* name) {
  const char* local = strrchr(name, ':');
  return local ? local + 1 : name;
}

constexpr const char* PARAGRAPH_TAGS[] = {"v", "text-author"};
constexpr int NUM_PARAGRAPH_TAGS = 2;

constexpr const char* BLOCK_TAGS[] = {"poem", "stanza", "cite", "epigraph", "annotation"};
constexpr int NUM_BLOCK_TAGS = 5;

bool matches(const char* tag, const char* const tags[], int count) {
  for (int i = 0; i < count; i++) {
    if (strcmp(tag, tags[i]) == 0) return true;
  }
  return false;
}
}  // namespace

Fb2Parser::Fb2Parser(const std::string& filepath, GfxRenderer& renderer, int fontId, float lineCompression,
                     bool extraParagraphSpacing, bool forceParagraphIndents, CssTextAlign paragraphAlignment,
                     uint16_t viewportWidth, uint16_t viewportHeight, bool hyphenationEnabled,
                     bool bionicReadingEnabled, bool guideReadingEnabled,
                     const std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t)>& completePageFn)
    : filepath_(filepath),
      renderer_(renderer),
      fontId_(fontId),
      lineCompression_(lineCompression),
      extraParagraphSpacing_(extraParagraphSpacing),
      forceParagraphIndents_(forceParagraphIndents),
      paragraphAlignment_(paragraphAlignment),
      viewportWidth_(viewportWidth),
      viewportHeight_(viewportHeight),
      hyphenationEnabled_(hyphenationEnabled),
      bionicReadingEnabled_(bionicReadingEnabled),
      guideReadingEnabled_(guideReadingEnabled),
      completePageFn_(completePageFn) {}

Fb2Parser::~Fb2Parser() {
  if (xmlParser_) {
    XML_ParserFree(xmlParser_);
    xmlParser_ = nullptr;
  }
  if (resumeFile_) {
    resumeFile_.close();
  }
}

bool Fb2Parser::parseAndBuildPages() {
  if (resumeFile_) {
    resumeFile_.close();
  }

  if (!Storage.openFileForRead("FB2", filepath_, resumeFile_)) {
    LOG_ERR(TAG, "Failed to open file: %s", filepath_.c_str());
    return false;
  }

  uint8_t peekBuffer[READ_CHUNK_SIZE + 1];
  size_t peekBytes = resumeFile_.read(peekBuffer, READ_CHUNK_SIZE);
  const char* explicitEncoding = nullptr;
  bomSkip_ = 0;

  if (peekBytes >= 3 && peekBuffer[0] == 0xEF && peekBuffer[1] == 0xBB && peekBuffer[2] == 0xBF) {
    explicitEncoding = "UTF-8";
    bomSkip_ = 3;
  }
  resumeFile_.seekSet(bomSkip_);

  xmlParser_ = XML_ParserCreate(explicitEncoding);
  if (!xmlParser_) {
    LOG_ERR(TAG, "Failed to create XML parser");
    resumeFile_.close();
    return false;
  }

  XML_SetUserData(xmlParser_, this);
  XML_SetElementHandler(xmlParser_, startElement, endElement);
  XML_SetCharacterDataHandler(xmlParser_, characterData);

  startNewPage();

  uint8_t buffer[READ_CHUNK_SIZE + 1];

  while (resumeFile_.available() > 0) {
    size_t bytesRead = resumeFile_.read(buffer, READ_CHUNK_SIZE);
    if (bytesRead == 0) break;

    int done = (resumeFile_.available() == 0) ? 1 : 0;
    auto status = XML_Parse(xmlParser_, reinterpret_cast<const char*>(buffer), static_cast<int>(bytesRead), done);
    if (status == XML_STATUS_ERROR) {
      LOG_ERR(TAG, "Parse error at line %lu: %s", XML_GetCurrentLineNumber(xmlParser_),
              XML_ErrorString(XML_GetErrorCode(xmlParser_)));
      XML_ParserFree(xmlParser_);
      xmlParser_ = nullptr;
      resumeFile_.close();
      return false;
    }

    if (status == XML_STATUS_SUSPENDED || stopRequested_) {
      XML_ParserFree(xmlParser_);
      xmlParser_ = nullptr;
      resumeFile_.close();
      return true;
    }
  }

  // Flush remaining content
  flushPartWordBuffer();
  if (currentTextBlock_ && !currentTextBlock_->isEmpty()) {
    makePages();
  }

  // Emit final page
  if (currentPage_ && !currentPage_->elements.empty()) {
    completePageFn_(std::move(currentPage_), 0, 0);
    pagesCreated_++;
  }

  XML_ParserFree(xmlParser_);
  xmlParser_ = nullptr;
  resumeFile_.close();
  currentTextBlock_.reset();
  currentPage_.reset();

  LOG_INF(TAG, "Parsed %d pages from %s", pagesCreated_, filepath_.c_str());
  return true;
}

void XMLCALL Fb2Parser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  (void)atts;
  auto* self = static_cast<Fb2Parser*>(userData);
  const char* localName = stripNamespace(name);

  if (self->depth_ >= 100) {
    self->depth_++;
    return;
  }

  if (self->skipUntilDepth_ < self->depth_) {
    self->depth_++;
    return;
  }

  if (strcmp(localName, "binary") == 0) {
    self->skipUntilDepth_ = self->depth_;
    self->depth_++;
    return;
  }

  if (strcmp(localName, "body") == 0) {
    self->bodyCount_++;
    self->inBody_ = (self->bodyCount_ == 1);
    self->depth_++;
    return;
  }

  if (!self->inBody_) {
    self->depth_++;
    return;
  }

  if (strcmp(localName, "section") == 0) {
    self->sectionCounter_++;
    if (!self->firstSection_) {
      self->flushPartWordBuffer();
      if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
        self->makePages();
      }
      if (self->currentPage_ && !self->currentPage_->elements.empty()) {
        self->completePageFn_(std::move(self->currentPage_), 0, 0);
        self->pagesCreated_++;
      }
      self->startNewPage();
    }
    self->firstSection_ = false;
    self->anchorMap_.emplace_back("section_" + std::to_string(self->sectionCounter_ - 1), self->pagesCreated_);
  } else if (strcmp(localName, "title") == 0) {
    self->inTitle_ = true;
    self->boldUntilDepth_ = std::min(self->boldUntilDepth_, self->depth_);
    self->flushPartWordBuffer();
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
    }
    self->startNewTextBlock(CssTextAlign::Center);
  } else if (strcmp(localName, "subtitle") == 0) {
    self->inSubtitle_ = true;
    self->boldUntilDepth_ = std::min(self->boldUntilDepth_, self->depth_);
    self->flushPartWordBuffer();
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
    }
    self->startNewTextBlock(CssTextAlign::Center);
  } else if (strcmp(localName, "p") == 0) {
    self->inParagraph_ = true;
    if (!self->currentTextBlock_) {
      CssTextAlign align = (self->inTitle_ || self->inSubtitle_) ? CssTextAlign::Center : self->paragraphAlignment_;
      self->startNewTextBlock(align);
    }
  } else if (strcmp(localName, "emphasis") == 0) {
    self->flushPartWordBuffer();
    self->italicUntilDepth_ = std::min(self->italicUntilDepth_, self->depth_);
  } else if (strcmp(localName, "code") == 0) {
    self->flushPartWordBuffer();
    self->italicUntilDepth_ = std::min(self->italicUntilDepth_, self->depth_);
  } else if (strcmp(localName, "strong") == 0) {
    self->flushPartWordBuffer();
    self->boldUntilDepth_ = std::min(self->boldUntilDepth_, self->depth_);
  } else if (strcmp(localName, "empty-line") == 0) {
    self->flushPartWordBuffer();
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
    }
    self->addVerticalSpacing(1);
  } else if (strcmp(localName, "image") == 0) {
    // Skip images in v1
  } else if (matches(localName, PARAGRAPH_TAGS, NUM_PARAGRAPH_TAGS)) {
    self->inParagraph_ = true;
    if (!self->currentTextBlock_) {
      self->startNewTextBlock(CssTextAlign::Left);
    }
  } else if (matches(localName, BLOCK_TAGS, NUM_BLOCK_TAGS)) {
    self->flushPartWordBuffer();
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
    }
    self->addVerticalSpacing(1);
  }

  self->depth_++;
}

void XMLCALL Fb2Parser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<Fb2Parser*>(userData);
  const char* localName = stripNamespace(name);

  if (strcmp(localName, "emphasis") == 0 || strcmp(localName, "code") == 0 || strcmp(localName, "strong") == 0) {
    self->flushPartWordBuffer();
  }

  self->depth_--;

  if (self->depth_ <= self->boldUntilDepth_) {
    self->boldUntilDepth_ = INT_MAX;
  }
  if (self->depth_ <= self->italicUntilDepth_) {
    self->italicUntilDepth_ = INT_MAX;
  }

  if (self->skipUntilDepth_ == self->depth_) {
    self->skipUntilDepth_ = INT_MAX;
    return;
  }

  if (!self->inBody_) {
    return;
  }

  if (strcmp(localName, "body") == 0) {
    self->inBody_ = false;
    return;
  }

  if (strcmp(localName, "title") == 0) {
    self->inTitle_ = false;
    self->flushPartWordBuffer();
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
    }
    self->addVerticalSpacing(1);
  } else if (strcmp(localName, "subtitle") == 0) {
    self->inSubtitle_ = false;
    self->flushPartWordBuffer();
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
    }
    self->addVerticalSpacing(1);
  } else if (strcmp(localName, "p") == 0) {
    self->inParagraph_ = false;
    self->flushPartWordBuffer();
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
    }
  } else if (matches(localName, PARAGRAPH_TAGS, NUM_PARAGRAPH_TAGS)) {
    self->inParagraph_ = false;
    self->flushPartWordBuffer();
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
    }
  } else if (matches(localName, BLOCK_TAGS, NUM_BLOCK_TAGS)) {
    self->flushPartWordBuffer();
    if (self->currentTextBlock_ && !self->currentTextBlock_->isEmpty()) {
      self->makePages();
    }
    self->addVerticalSpacing(1);
  }
}

void XMLCALL Fb2Parser::characterData(void* userData, const XML_Char* s, int len) {
  auto* self = static_cast<Fb2Parser*>(userData);

  if (self->skipUntilDepth_ < self->depth_) return;
  if (!self->inBody_) return;

  for (int i = 0; i < len; i++) {
    if (isWhitespace(s[i])) {
      if (self->partWordBufferIndex_ > 0) {
        self->flushPartWordBuffer();
      }
      continue;
    }

    if (self->partWordBufferIndex_ >= MAX_WORD_SIZE) {
      self->flushPartWordBuffer();
    }

    self->partWordBuffer_[self->partWordBufferIndex_++] = s[i];
  }
}

void Fb2Parser::flushPartWordBuffer() {
  if (!currentTextBlock_ || partWordBufferIndex_ == 0) {
    partWordBufferIndex_ = 0;
    return;
  }

  partWordBuffer_[partWordBufferIndex_] = '\0';
  partWordBufferIndex_ = 0;

  currentTextBlock_->addWord(partWordBuffer_, getCurrentFontFamily());
}

void Fb2Parser::startNewTextBlock(CssTextAlign align) {
  if (stopRequested_) return;
  if (currentTextBlock_) {
    if (currentTextBlock_->isEmpty()) {
      BlockStyle bs;
      bs.alignment = align;
      currentTextBlock_->setBlockStyle(bs);
      return;
    }
    makePages();
  }
  currentTextBlock_ = std::make_unique<ParsedText>(extraParagraphSpacing_, forceParagraphIndents_, hyphenationEnabled_,
                                                   bionicReadingEnabled_, guideReadingEnabled_);
  BlockStyle bs;
  bs.alignment = align;
  currentTextBlock_->setBlockStyle(bs);
}

void Fb2Parser::makePages() {
  if (!currentTextBlock_ || currentTextBlock_->isEmpty()) return;

  flushPartWordBuffer();

  if (!currentPage_) {
    startNewPage();
  }

  const int lineHeight = static_cast<int>(renderer_.getLineHeight(fontId_) * lineCompression_);
  bool continueProcessing = true;

  currentTextBlock_->layoutAndExtractLines(
      renderer_, fontId_, viewportWidth_,
      [this, &continueProcessing](const std::shared_ptr<TextBlock>& line) {
        if (!continueProcessing) return;
        addLineToPage(line);
      },
      true);

  if (!hitMaxPages_) {
    switch (lineCompression_ > 1.2f ? 3 : (lineCompression_ < 0.8f ? 1 : 2)) {
      case 1:
        currentPageNextY_ += lineHeight / 4;
        break;
      case 3:
        currentPageNextY_ += lineHeight;
        break;
    }
    currentTextBlock_.reset();
  }
}

void Fb2Parser::addLineToPage(std::shared_ptr<TextBlock> line) {
  const int lineHeight = static_cast<int>(renderer_.getLineHeight(fontId_) * lineCompression_);

  if (!currentPage_) {
    startNewPage();
  }

  if (currentPageNextY_ + lineHeight > viewportHeight_) {
    completePageFn_(std::move(currentPage_), 0, 0);
    pagesCreated_++;
    startNewPage();
  }

  currentPage_->elements.push_back(std::make_shared<PageLine>(std::move(line), 0, currentPageNextY_));
  currentPageNextY_ += lineHeight;
}

void Fb2Parser::startNewPage() {
  currentPage_ = std::make_unique<Page>();
  currentPageNextY_ = 0;
}

EpdFontFamily::Style Fb2Parser::getCurrentFontFamily() const {
  bool bold = (boldUntilDepth_ < INT_MAX);
  bool italic = (italicUntilDepth_ < INT_MAX);
  if (bold && italic) return EpdFontFamily::BOLD_ITALIC;
  if (bold) return EpdFontFamily::BOLD;
  if (italic) return EpdFontFamily::ITALIC;
  return EpdFontFamily::REGULAR;
}

void Fb2Parser::addVerticalSpacing(int lines) {
  const int lineHeight = static_cast<int>(renderer_.getLineHeight(fontId_) * lineCompression_);
  currentPageNextY_ += lineHeight * lines;
}
