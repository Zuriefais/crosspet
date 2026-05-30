#include "Fb2.h"

#include <FsHelpers.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Serialization.h>

#include "Base64Decoder.h"

#define TAG "FB2"

#include <HalStorage.h>

#include <cstring>

namespace {
constexpr uint8_t kMetaCacheVersion = 6;
constexpr char kMetaCacheFile[] = "/meta.bin";
constexpr char kSectionFilePrefix[] = "/section_";
constexpr char kSectionFileSuffix[] = ".fb2";
constexpr char kSectionCacheMarker[] = "/.section_cache_v2";
}  // namespace

std::string Fb2::metaCachePath() const { return cachePath + kMetaCacheFile; }

Fb2::Fb2(std::string filepath, const std::string& cacheDir)
    : filepath(std::move(filepath)), fileSize(0), loaded(false) {
  cachePath = cacheDir + "/fb2_" + std::to_string(std::hash<std::string>{}(this->filepath));

  size_t lastSlash = this->filepath.find_last_of('/');
  size_t lastDot = this->filepath.find_last_of('.');

  if (lastSlash == std::string::npos) {
    lastSlash = 0;
  } else {
    lastSlash++;
  }

  if (lastDot == std::string::npos || lastDot <= lastSlash) {
    title = this->filepath.substr(lastSlash);
  } else {
    title = this->filepath.substr(lastSlash, lastDot - lastSlash);
  }
}

Fb2::~Fb2() {
  if (xmlParser_) {
    XML_ParserFree(xmlParser_);
    xmlParser_ = nullptr;
  }
}

bool Fb2::load() {
  LOG_INF(TAG, "Loading FB2: %s", filepath.c_str());

  if (!Storage.exists(filepath.c_str())) {
    LOG_ERR(TAG, "File does not exist");
    return false;
  }

  if (loadMetaCache() && !sectionOffsets_.empty()) {
    loaded = true;
    LOG_INF(TAG, "Loaded from cache: %s (title: '%s', author: '%s')", filepath.c_str(), title.c_str(), author.c_str());
    if (!generateSectionFiles()) {
      LOG_ERR(TAG, "Failed to generate section files (cache path)");
    }
    return true;
  }

  HalFile file;
  if (!Storage.openFileForRead("FB2", filepath, file)) {
    LOG_ERR(TAG, "Failed to open file");
    return false;
  }

  fileSize = file.size();
  file.close();

  author.clear();

  if (!parseXmlStream()) {
    LOG_ERR(TAG, "Failed to parse XML");
    return false;
  }

  if (!scanSectionOffsets()) {
    LOG_ERR(TAG, "Failed to scan section offsets");
  }

  filterNestedSections();

  saveMetaCache();

  if (!generateSectionFiles()) {
    LOG_ERR(TAG, "Failed to generate section files");
  }

  std::vector<TocItem>().swap(tocItems_);
  if (!loadMetaCache()) {
    LOG_ERR(TAG, "Failed to reload meta cache for LUT");
  }

  loaded = true;
  LOG_INF(TAG, "Loaded FB2: %s (title: '%s', author: '%s', sections: %u)", filepath.c_str(), title.c_str(),
          author.c_str(), static_cast<unsigned int>(sectionOffsets_.size()));
  return true;
}

bool Fb2::loadMetadataOnly() {
  LOG_INF(TAG, "Loading metadata only: %s", filepath.c_str());

  if (!Storage.exists(filepath.c_str())) {
    LOG_ERR(TAG, "File does not exist");
    return false;
  }

  if (loadMetaCache()) {
    return true;
  }

  HalFile file;
  if (!Storage.openFileForRead("FB2", filepath, file)) {
    LOG_ERR(TAG, "Failed to open file");
    return false;
  }
  fileSize = file.size();
  file.close();

  metadataOnly_ = true;
  bool ok = parseXmlStream();
  metadataOnly_ = false;

  if (!ok) {
    return false;
  }

  saveMetaCache();
  return true;
}

void XMLCALL Fb2::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<Fb2*>(userData);

  self->depth++;

  if (self->depth >= 100) {
    return;
  }

  if (self->skipUntilDepth < self->depth) {
    return;
  }

  const char* tag = strrchr(name, ':');
  if (tag) {
    tag++;
  } else {
    tag = name;
  }

  if (strcmp(tag, "binary") == 0) {
    if (!self->coverRef.empty() && atts) {
      bool isTargetBinary = false;
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "id") == 0 && atts[i + 1] && self->coverRef == atts[i + 1]) {
          isTargetBinary = true;
          break;
        }
      }
      if (isTargetBinary) {
        self->coverBinaryOffset_ = XML_GetCurrentByteIndex(self->xmlParser_);
        for (int i = 0; atts[i]; i += 2) {
          if (strcmp(atts[i], "content-type") == 0 && atts[i + 1]) {
            self->coverContentType = atts[i + 1];
            break;
          }
        }
      }
    }
    self->skipUntilDepth = self->depth - 1;
    return;
  }

  if (strcmp(tag, "title-info") == 0) {
    self->inTitleInfo = true;
  }

  if (strcmp(tag, "book-title") == 0 && self->inTitleInfo) {
    self->inBookTitle = true;
    self->title.clear();
  } else if (strcmp(tag, "author") == 0 && self->inTitleInfo) {
    self->inAuthor = true;
    self->currentAuthorFirst.clear();
    self->currentAuthorLast.clear();
  } else if (strcmp(tag, "first-name") == 0 && self->inAuthor) {
    self->inFirstName = true;
  } else if (strcmp(tag, "last-name") == 0 && self->inAuthor) {
    self->inLastName = true;
  } else if (strcmp(tag, "lang") == 0 && self->inTitleInfo) {
    self->inLang = true;
    self->language.clear();
  } else if (strcmp(tag, "coverpage") == 0) {
    self->inCoverPage = true;
  } else if (strcmp(tag, "image") == 0 && self->inCoverPage) {
    if (atts) {
      for (int i = 0; atts[i]; i += 2) {
        const char* attrName = atts[i];
        const char* attrValue = atts[i + 1];

        const char* attr = strrchr(attrName, ':');
        if (attr)
          attr++;
        else
          attr = attrName;

        if ((strcmp(attr, "href") == 0 || strcmp(attrName, "l:href") == 0) && attrValue) {
          if (attrValue[0] == '#') {
            self->coverRef = attrValue + 1;
          } else {
            self->coverRef = attrValue;
          }
          LOG_INF(TAG, "Found cover reference: %s", self->coverRef.c_str());
          break;
        }
      }
    }
  } else if (strcmp(tag, "body") == 0) {
    self->bodyCount_++;
    self->inBody = (self->bodyCount_ == 1);
  } else if (strcmp(tag, "section") == 0 && self->inBody) {
    self->sectionCounter_++;
  } else if (strcmp(tag, "title") == 0 && self->inBody && self->sectionCounter_ > 0) {
    self->inSectionTitle_ = true;
    self->sectionTitleDepth_ = self->depth;
    self->currentSectionTitle_.clear();
  }
}

void XMLCALL Fb2::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<Fb2*>(userData);

  const char* tag = strrchr(name, ':');
  if (tag) {
    tag++;
  } else {
    tag = name;
  }

  if (strcmp(tag, "title-info") == 0) {
    self->inTitleInfo = false;
    if (self->metadataOnly_) {
      XML_StopParser(self->xmlParser_, XML_FALSE);
      return;
    }
  }

  if (strcmp(tag, "book-title") == 0) {
    self->inBookTitle = false;
  } else if (strcmp(tag, "first-name") == 0) {
    self->inFirstName = false;
  } else if (strcmp(tag, "last-name") == 0) {
    self->inLastName = false;
  } else if (strcmp(tag, "author") == 0 && self->inAuthor) {
    std::string fullAuthor;
    if (!self->currentAuthorFirst.empty()) {
      fullAuthor = self->currentAuthorFirst;
      if (!self->currentAuthorLast.empty()) {
        fullAuthor += " ";
      }
    }
    fullAuthor += self->currentAuthorLast;

    if (!fullAuthor.empty()) {
      if (!self->author.empty()) {
        self->author += ", ";
      }
      self->author += fullAuthor;
    }

    self->inAuthor = false;
    self->currentAuthorFirst.clear();
    self->currentAuthorLast.clear();
  } else if (strcmp(tag, "lang") == 0 && self->inLang) {
    self->inLang = false;
  } else if (strcmp(tag, "coverpage") == 0) {
    self->inCoverPage = false;
  } else if (strcmp(tag, "binary") == 0) {
    self->skipUntilDepth = INT_MAX;
  } else if (strcmp(tag, "body") == 0) {
    self->inBody = false;
  } else if (strcmp(tag, "title") == 0 && self->inSectionTitle_ && self->depth == self->sectionTitleDepth_) {
    self->inSectionTitle_ = false;

    std::string& t = self->currentSectionTitle_;
    for (size_t i = 0; i < t.size(); i++) {
      if (t[i] == '\n' || t[i] == '\r') {
        t[i] = ' ';
      }
    }
    size_t start = 0;
    while (start < t.size() && isspace(static_cast<unsigned char>(t[start]))) {
      start++;
    }
    size_t end = t.size();
    while (end > start && isspace(static_cast<unsigned char>(t[end - 1]))) {
      end--;
    }
    if (start > 0 || end < t.size()) {
      t = t.substr(start, end - start);
    }

    if (!t.empty()) {
      self->tocItems_.push_back({t, self->sectionCounter_ - 1});
    }
  }

  self->depth--;
}

void XMLCALL Fb2::characterData(void* userData, const XML_Char* s, int len) {
  auto* self = static_cast<Fb2*>(userData);

  if (self->skipUntilDepth < self->depth) {
    return;
  }

  if (self->inSectionTitle_) {
    self->currentSectionTitle_.append(s, len);
  }

  if (self->inBookTitle) {
    self->title.append(s, len);
  } else if (self->inLang) {
    self->language.append(s, len);
  } else if (self->inFirstName) {
    self->currentAuthorFirst.append(s, len);
  } else if (self->inLastName) {
    self->currentAuthorLast.append(s, len);
  }
}

bool Fb2::parseXmlStream() {
  LOG_INF(TAG, "Starting streaming XML parse");

  HalFile file;
  if (!Storage.openFileForRead("FB2", filepath, file)) {
    return false;
  }

  constexpr size_t kChunkSize = 4096;
  uint8_t buffer[kChunkSize];

  const size_t peekBytes = file.read(buffer, kChunkSize);
  const char* explicitEncoding = nullptr;
  size_t bomSkip = 0;

  const unsigned char* peek = buffer;
  size_t pb = peekBytes;
  if (pb >= 3 && peek[0] == 0xEF && peek[1] == 0xBB && peek[2] == 0xBF) {
    explicitEncoding = "UTF-8";
    bomSkip = 3;
  }
  file.seek(bomSkip);

  xmlParser_ = XML_ParserCreate(explicitEncoding);
  if (!xmlParser_) {
    LOG_ERR(TAG, "Failed to create XML parser");
    file.close();
    return false;
  }

  XML_SetUserData(xmlParser_, this);
  XML_SetElementHandler(xmlParser_, startElement, endElement);
  XML_SetCharacterDataHandler(xmlParser_, characterData);

  bool success = true;

  while (file.available() > 0) {
    const size_t bytesRead = file.read(buffer, kChunkSize);
    if (bytesRead == 0) break;

    const int done = (file.available() == 0) ? 1 : 0;
    if (XML_Parse(xmlParser_, reinterpret_cast<const char*>(buffer), static_cast<int>(bytesRead), done) ==
        XML_STATUS_ERROR) {
      if (metadataOnly_ && XML_GetErrorCode(xmlParser_) == XML_ERROR_ABORTED) {
        break;
      }
      LOG_ERR(TAG, "XML parse error: %s", XML_ErrorString(XML_GetErrorCode(xmlParser_)));
      success = false;
      break;
    }
  }

  file.close();

  if (success) {
    postProcessMetadata();
  }

  XML_ParserFree(xmlParser_);
  xmlParser_ = nullptr;
  return success;
}

bool Fb2::scanSectionOffsets() {
  LOG_INF(TAG, "Scanning section offsets");

  HalFile file;
  if (!Storage.openFileForRead("FB2", filepath, file)) {
    return false;
  }

  constexpr size_t kChunkSize = 4096;
  uint8_t buffer[kChunkSize];

  const size_t peekBytes = file.read(buffer, kChunkSize);
  size_t bomSkip = 0;

  const unsigned char* peek = buffer;
  if (peekBytes >= 3 && peek[0] == 0xEF && peek[1] == 0xBB && peek[2] == 0xBF) {
    bomSkip = 3;
  }
  file.seek(bomSkip);

  struct SectionScanCtx {
    std::vector<SectionOffset>* offsets;
    int depth = 0;
    int skipUntilDepth = INT_MAX;
    bool inBody = false;
    int bodyCount = 0;
    XML_Parser parser = nullptr;
    std::vector<int> sectionStack;
  };

  SectionScanCtx ctx;
  ctx.offsets = &sectionOffsets_;
  sectionOffsets_.clear();

  XML_Parser parser = XML_ParserCreate(nullptr);
  if (!parser) {
    LOG_ERR(TAG, "Failed to create section scanner parser");
    file.close();
    return false;
  }
  ctx.parser = parser;

  auto scanStart = [](void* userData, const XML_Char* name, const XML_Char** atts) {
    (void)atts;
    auto* c = static_cast<SectionScanCtx*>(userData);
    c->depth++;
    if (c->depth >= 100) return;
    if (c->skipUntilDepth < c->depth) return;

    const char* tag = strrchr(name, ':');
    if (tag)
      tag++;
    else
      tag = name;

    if (strcmp(tag, "binary") == 0) {
      c->skipUntilDepth = c->depth - 1;
      return;
    }
    if (strcmp(tag, "body") == 0) {
      c->bodyCount++;
      c->inBody = (c->bodyCount == 1);
      return;
    }
    if (strcmp(tag, "section") == 0 && c->inBody) {
      int64_t byteIdx = XML_GetCurrentByteIndex(c->parser);
      if (byteIdx >= 0) {
        SectionOffset off;
        off.startOffset = static_cast<uint32_t>(byteIdx);
        c->offsets->push_back(off);
        c->sectionStack.push_back(static_cast<int>(c->offsets->size()) - 1);
      }
    }
  };

  auto scanEnd = [](void* userData, const XML_Char* name) {
    auto* c = static_cast<SectionScanCtx*>(userData);
    const char* tag = strrchr(name, ':');
    if (tag)
      tag++;
    else
      tag = name;

    if (strcmp(tag, "binary") == 0) {
      c->skipUntilDepth = INT_MAX;
    }
    if (strcmp(tag, "body") == 0) {
      c->inBody = false;
    }
    if (strcmp(tag, "section") == 0 && c->inBody && !c->sectionStack.empty()) {
      int sectionIdx = c->sectionStack.back();
      c->sectionStack.pop_back();
      int64_t byteIdx = XML_GetCurrentByteIndex(c->parser);
      if (byteIdx >= 0) {
        uint32_t endOffset = static_cast<uint32_t>(byteIdx) + static_cast<uint32_t>(strlen(name)) + 3;
        (*c->offsets)[sectionIdx].endOffset = endOffset;
      }
    }
    c->depth--;
  };

  XML_SetUserData(parser, &ctx);
  XML_SetElementHandler(parser, scanStart, scanEnd);

  bool success = true;
  while (file.available() > 0) {
    const size_t bytesRead = file.read(buffer, kChunkSize);
    if (bytesRead == 0) break;
    const int done = (file.available() == 0) ? 1 : 0;
    if (XML_Parse(parser, reinterpret_cast<const char*>(buffer), static_cast<int>(bytesRead), done) ==
        XML_STATUS_ERROR) {
      success = false;
      break;
    }
  }

  file.close();
  XML_ParserFree(parser);

  if (success) {
    LOG_INF(TAG, "Scanned %u section offsets", static_cast<unsigned int>(sectionOffsets_.size()));
  }
  return success;
}

void Fb2::filterNestedSections() {
  if (sectionOffsets_.size() <= 1) return;

  const size_t n = sectionOffsets_.size();

  std::vector<bool> isParent(n, false);
  for (size_t i = 0; i < n; i++) {
    for (size_t j = 0; j < n; j++) {
      if (i != j && sectionOffsets_[i].startOffset <= sectionOffsets_[j].startOffset &&
          sectionOffsets_[i].endOffset >= sectionOffsets_[j].endOffset &&
          (sectionOffsets_[i].startOffset != sectionOffsets_[j].startOffset ||
           sectionOffsets_[i].endOffset != sectionOffsets_[j].endOffset)) {
        isParent[i] = true;
        break;
      }
    }
  }

  size_t parentCount = 0;
  for (size_t i = 0; i < n; i++) {
    if (isParent[i]) parentCount++;
  }
  if (parentCount == 0) return;

  std::vector<int> remap(n, -1);
  int newIndex = 0;
  for (size_t i = 0; i < n; i++) {
    if (!isParent[i]) {
      remap[i] = newIndex++;
    }
  }

  for (size_t i = 0; i < n; i++) {
    if (!isParent[i]) continue;
    for (size_t j = i + 1; j < n; j++) {
      if (!isParent[j] && sectionOffsets_[j].startOffset >= sectionOffsets_[i].startOffset &&
          sectionOffsets_[j].endOffset <= sectionOffsets_[i].endOffset) {
        remap[i] = remap[j];
        break;
      }
    }
  }

  for (auto& toc : tocItems_) {
    if (toc.sectionIndex >= 0 && toc.sectionIndex < static_cast<int>(n)) {
      toc.sectionIndex = remap[toc.sectionIndex];
    }
  }

  std::vector<SectionOffset> filtered;
  filtered.reserve(n - parentCount);
  for (size_t i = 0; i < n; i++) {
    if (!isParent[i]) {
      filtered.push_back(sectionOffsets_[i]);
    }
  }
  sectionOffsets_ = std::move(filtered);

  LOG_INF(TAG, "Filtered %u parent sections, %u leaf sections remain", static_cast<unsigned int>(parentCount),
          static_cast<unsigned int>(sectionOffsets_.size()));
}

bool Fb2::generateSectionFiles() {
  if (sectionOffsets_.empty()) {
    LOG_INF(TAG, "No sections to generate");
    return true;
  }

  setupCacheDir();

  const std::string markerPath = cachePath + kSectionCacheMarker;
  if (!Storage.exists(markerPath.c_str())) {
    for (size_t i = 0; i < sectionOffsets_.size(); i++) {
      const std::string oldPath = sectionFilePath(static_cast<int>(i));
      if (Storage.exists(oldPath.c_str())) {
        Storage.remove(oldPath.c_str());
      }
    }
  }

  std::string xmlDecl = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  HalFile declFile;
  if (Storage.openFileForRead("FB2", filepath, declFile)) {
    char declBuf[256];
    size_t declRead = declFile.read(declBuf, sizeof(declBuf));
    for (size_t j = 0; j + 1 < declRead; j++) {
      if (declBuf[j] == '?' && declBuf[j + 1] == '>') {
        xmlDecl.assign(declBuf, j + 2);
        xmlDecl += "\n";
        break;
      }
    }
    declFile.close();
  }

  const std::string wrapperStart =
      "<FictionBook xmlns=\"http://www.gribuser.ru/xml/fictionbook/2.0\" "
      "xmlns:l=\"http://www.w3.org/1999/xlink\">\n"
      "  <body>\n";
  const std::string wrapperEnd = "  </body>\n</FictionBook>\n";

  constexpr size_t kStartBackoff = 16;
  constexpr size_t kAnchorWindowSize = 128;
  constexpr char kStartTag[] = "<section";
  constexpr size_t kStartTagLen = sizeof(kStartTag) - 1;
  constexpr char kEndTag[] = "</section>";
  constexpr size_t kEndTagLen = sizeof(kEndTag) - 1;
  constexpr size_t kCopyBufSize = 4096;
  constexpr uint32_t kCopyOverhead = 1024;

  bool allOk = true;
  for (size_t i = 0; i < sectionOffsets_.size(); i++) {
    const std::string outPath = sectionFilePath(static_cast<int>(i));

    if (Storage.exists(outPath.c_str())) {
      continue;
    }

    HalFile outFile;
    if (!Storage.openFileForWrite("FB2", outPath, outFile)) {
      LOG_ERR(TAG, "Failed to create section file: %s", outPath.c_str());
      allOk = false;
      continue;
    }

    outFile.write(xmlDecl.c_str(), xmlDecl.size());
    outFile.write(wrapperStart.c_str(), wrapperStart.size());

    bool sectionWritten = false;
    HalFile inFile;
    if (Storage.openFileForRead("FB2", filepath, inFile)) {
      const uint32_t hintStart = sectionOffsets_[i].startOffset;
      const uint32_t hintEnd = sectionOffsets_[i].endOffset;

      const uint32_t windowStart = (hintStart > kStartBackoff) ? (hintStart - kStartBackoff) : 0;
      uint32_t actualStart = 0;
      bool startAnchored = false;
      if (inFile.seek(windowStart)) {
        uint8_t windowBuf[kAnchorWindowSize];
        const size_t winRead = inFile.read(windowBuf, kAnchorWindowSize);
        for (size_t j = 0; j + kStartTagLen <= winRead; j++) {
          if (memcmp(windowBuf + j, kStartTag, kStartTagLen) == 0) {
            actualStart = windowStart + static_cast<uint32_t>(j);
            startAnchored = true;
            break;
          }
        }
      }

      if (!startAnchored) {
        LOG_ERR(TAG, "section %u: <section start tag not found near offset %u", static_cast<unsigned int>(i),
                static_cast<unsigned int>(windowStart));
      } else if (!inFile.seek(actualStart)) {
        LOG_ERR(TAG, "section %u: seek to %u failed", static_cast<unsigned int>(i),
                static_cast<unsigned int>(actualStart));
      } else {
        const uint32_t span = (hintEnd > actualStart) ? (hintEnd - actualStart) : 0;
        const uint32_t copyCap = span + kCopyOverhead;

        uint8_t copyBuf[kCopyBufSize + kEndTagLen];
        uint32_t consumed = 0;
        size_t carry = 0;
        bool foundClose = false;
        int tagDepth = 0;

        while (consumed < copyCap) {
          size_t maxRead = copyCap - consumed;
          if (maxRead > kCopyBufSize) maxRead = kCopyBufSize;
          const size_t n = inFile.read(copyBuf + carry, maxRead);
          if (n == 0) break;
          consumed += static_cast<uint32_t>(n);
          const size_t bufLen = carry + n;

          size_t closeAt = SIZE_MAX;
          size_t j = 0;
          while (j + kStartTagLen <= bufLen) {
            if (j + kEndTagLen <= bufLen && memcmp(copyBuf + j, kEndTag, kEndTagLen) == 0) {
              tagDepth--;
              if (tagDepth <= 0) {
                closeAt = j;
                break;
              }
              j += kEndTagLen;
            } else if (memcmp(copyBuf + j, kStartTag, kStartTagLen) == 0) {
              tagDepth++;
              j += kStartTagLen;
            } else {
              j++;
            }
          }

          if (closeAt != SIZE_MAX) {
            outFile.write(copyBuf, closeAt + kEndTagLen);
            foundClose = true;
            break;
          }

          if (bufLen > kEndTagLen - 1) {
            const size_t writeLen = bufLen - (kEndTagLen - 1);
            outFile.write(copyBuf, writeLen);
            carry = kEndTagLen - 1;
            memmove(copyBuf, copyBuf + writeLen, carry);
          } else {
            carry = bufLen;
          }
        }

        if (foundClose) {
          sectionWritten = true;
        } else {
          LOG_ERR(TAG, "section %u: </section> close tag not found within %u byte cap", static_cast<unsigned int>(i),
                  static_cast<unsigned int>(copyCap));
        }
      }
      inFile.close();
    }

    if (!sectionWritten) {
      outFile.close();
      Storage.remove(outPath.c_str());
      allOk = false;
      continue;
    }

    outFile.write(wrapperEnd.c_str(), wrapperEnd.size());
    outFile.close();
  }

  if (allOk) {
    HalFile marker;
    if (Storage.openFileForWrite("FB2", markerPath, marker)) {
      marker.close();
    }
  }

  LOG_INF(TAG, "Generated %u section files (allOk=%d)", static_cast<unsigned int>(sectionOffsets_.size()),
          allOk ? 1 : 0);
  return allOk;
}

std::string Fb2::sectionFilePath(int sectionIndex) const {
  return cachePath + kSectionFilePrefix + std::to_string(sectionIndex) + kSectionFileSuffix;
}

std::string Fb2::getSectionPath(int sectionIndex) const {
  if (sectionIndex < 0 || sectionIndex >= static_cast<int>(sectionOffsets_.size())) {
    return filepath;
  }
  return sectionFilePath(sectionIndex);
}

int Fb2::getSectionForTocEntry(int tocIndex) const {
  TocItem item = getTocItem(static_cast<uint16_t>(tocIndex));
  return item.sectionIndex;
}

void Fb2::postProcessMetadata() {
  while (!title.empty() && isspace(static_cast<unsigned char>(title.back()))) {
    title.pop_back();
  }
  while (!title.empty() && isspace(static_cast<unsigned char>(title.front()))) {
    title.erase(title.begin());
  }

  for (size_t i = 0; i < title.size(); i++) {
    if (title[i] == '\n' || title[i] == '\r') {
      title[i] = ' ';
    }
  }

  while (!language.empty() && isspace(static_cast<unsigned char>(language.back()))) {
    language.pop_back();
  }
  while (!language.empty() && isspace(static_cast<unsigned char>(language.front()))) {
    language.erase(language.begin());
  }

  LOG_INF(TAG, "XML parsing complete: title='%s', author='%s', lang='%s'", title.c_str(), author.c_str(),
          language.c_str());
}

bool Fb2::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    LOG_INF(TAG, "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR(TAG, "Failed to clear cache");
    return false;
  }

  LOG_INF(TAG, "Cache cleared successfully");
  return true;
}

void Fb2::setupCacheDir() const {
  if (Storage.exists(cachePath.c_str())) {
    return;
  }

  for (size_t i = 1; i < cachePath.length(); i++) {
    if (cachePath[i] == '/') {
      Storage.mkdir(cachePath.substr(0, i).c_str());
    }
  }
  Storage.mkdir(cachePath.c_str());
}

std::string Fb2::getCoverBmpPath() const { return cachePath + "/cover.bmp"; }

std::string Fb2::findCoverImage() const {
  size_t lastSlash = filepath.find_last_of('/');
  std::string dirPath = (lastSlash == std::string::npos) ? "/" : filepath.substr(0, lastSlash);
  if (dirPath.empty()) {
    dirPath = "/";
  }

  std::string baseName = getTitle();

  const char* extensions[] = {".bmp", ".jpg", ".jpeg", ".png", ".BMP", ".JPG", ".JPEG", ".PNG"};

  for (const auto& ext : extensions) {
    std::string coverPath = dirPath + "/" + baseName + ext;
    if (Storage.exists(coverPath.c_str())) {
      return coverPath;
    }
  }

  const char* coverNames[] = {"cover", "Cover", "COVER"};
  for (const auto& name : coverNames) {
    for (const auto& ext : extensions) {
      std::string coverPath = dirPath + "/" + std::string(name) + ext;
      if (Storage.exists(coverPath.c_str())) {
        return coverPath;
      }
    }
  }

  return "";
}

bool Fb2::extractEmbeddedCover(const std::string& outputPath) const {
  if (coverRef.empty() || coverBinaryOffset_ < 0) return false;

  HalFile fb2File;
  if (!Storage.openFileForRead("FB2", filepath, fb2File)) {
    return false;
  }

  if (!fb2File.seek(static_cast<uint32_t>(coverBinaryOffset_))) {
    fb2File.close();
    return false;
  }

  constexpr size_t kBufSize = 256;
  uint8_t buf[kBufSize];
  bool foundTagEnd = false;
  int scanLimit = 512;

  while (!foundTagEnd && scanLimit > 0 && fb2File.available() > 0) {
    size_t toRead = static_cast<size_t>(scanLimit) < kBufSize ? static_cast<size_t>(scanLimit) : kBufSize;
    size_t n = fb2File.read(buf, toRead);
    if (n == 0) break;
    scanLimit -= static_cast<int>(n);

    for (size_t i = 0; i < n; i++) {
      if (buf[i] == '>') {
        fb2File.seek(fb2File.position() - (n - i - 1));
        foundTagEnd = true;
        break;
      }
    }
  }

  if (!foundTagEnd) {
    fb2File.close();
    return false;
  }

  HalFile outFile;
  if (!Storage.openFileForWrite("FB2", outputPath, outFile)) {
    fb2File.close();
    return false;
  }

  Base64Decoder decoder;
  bool done = false;
  bool success = false;

  auto writeCallback = [&outFile](const uint8_t* data, size_t sz) { return outFile.write(data, sz) == sz; };

  while (!done && fb2File.available() > 0) {
    size_t n = fb2File.read(buf, kBufSize);
    if (n == 0) break;

    size_t feedLen = n;
    for (size_t i = 0; i < n; i++) {
      if (buf[i] == '<') {
        feedLen = i;
        done = true;
        break;
      }
    }

    if (feedLen > 0) {
      decoder.feed(reinterpret_cast<const char*>(buf), static_cast<int>(feedLen), writeCallback);
    }

    if (decoder.failed()) break;
  }

  if (done && !decoder.failed()) {
    success = decoder.finish(writeCallback);
  }

  outFile.close();
  fb2File.close();

  if (!success) {
    Storage.remove(outputPath.c_str());
  }

  LOG_INF(TAG, "Cover extraction %s", success ? "succeeded" : "failed");
  return success;
}

bool Fb2::generateCoverBmp() const {
  const auto coverBmpPath = getCoverBmpPath();
  const auto failedMarkerPath = cachePath + "/.cover.failed";

  if (Storage.exists(coverBmpPath.c_str())) {
    return true;
  }

  if (Storage.exists(failedMarkerPath.c_str())) {
    return false;
  }

  std::string coverImagePath = findCoverImage();

  std::string tmpCoverPath;
  if (coverImagePath.empty() && !coverRef.empty()) {
    std::string ext = ".jpg";
    if (coverContentType == "image/png") {
      ext = ".png";
    } else if (coverContentType == "image/bmp") {
      ext = ".bmp";
    }
    tmpCoverPath = cachePath + "/.tmp_cover" + ext;
    setupCacheDir();
    if (extractEmbeddedCover(tmpCoverPath)) {
      coverImagePath = tmpCoverPath;
    }
  }

  if (coverImagePath.empty()) {
    LOG_INF(TAG, "No cover image found");
    HalFile marker;
    if (Storage.openFileForWrite("FB2", failedMarkerPath, marker)) {
      marker.close();
    }
    return false;
  }

  setupCacheDir();

  bool success = false;

  if (FsHelpers::hasBmpExtension(coverImagePath)) {
    HalFile src, dst;
    if (Storage.openFileForRead("FB2", coverImagePath, src) &&
        Storage.openFileForWrite("FB2", coverBmpPath, dst)) {
      uint8_t copyBuf[1024];
      while (src.available()) {
        size_t bytesRead = src.read(copyBuf, sizeof(copyBuf));
        dst.write(copyBuf, bytesRead);
      }
      success = true;
    }
  } else if (FsHelpers::hasJpgExtension(coverImagePath)) {
    HalFile src, dst;
    if (Storage.openFileForRead("FB2", coverImagePath, src) &&
        Storage.openFileForWrite("FB2", coverBmpPath, dst)) {
      success = JpegToBmpConverter::jpegFileToBmpStream(src, dst);
    }
  }

  if (!tmpCoverPath.empty()) {
    Storage.remove(tmpCoverPath.c_str());
  }

  if (!success) {
    HalFile marker;
    if (Storage.openFileForWrite("FB2", failedMarkerPath, marker)) {
      marker.close();
    }
  }
  return success;
}

std::string Fb2::getThumbBmpPath() const { return cachePath + "/thumb.bmp"; }

bool Fb2::generateThumbBmp() const {
  const auto thumbPath = getThumbBmpPath();
  const auto failedMarkerPath = cachePath + "/.thumb.failed";

  if (Storage.exists(thumbPath.c_str())) {
    return true;
  }

  if (Storage.exists(failedMarkerPath.c_str())) {
    return false;
  }

  if (!Storage.exists(getCoverBmpPath().c_str()) && !generateCoverBmp()) {
    HalFile marker;
    if (Storage.openFileForWrite("FB2", failedMarkerPath, marker)) {
      marker.close();
    }
    return false;
  }

  setupCacheDir();

  HalFile src;
  if (!Storage.openFileForRead("FB2", getCoverBmpPath(), src)) {
    goto fail;
  }

  {
    HalFile dst;
    if (!Storage.openFileForWrite("FB2", thumbPath, dst)) {
      src.close();
      goto fail;
    }

    uint8_t copyBuf[1024];
    while (src.available()) {
      size_t bytesRead = src.read(copyBuf, sizeof(copyBuf));
      dst.write(copyBuf, bytesRead);
    }
    src.close();
    dst.close();
  }

  LOG_INF(TAG, "Generated thumb BMP");
  return true;

fail:
  HalFile marker;
  if (Storage.openFileForWrite("FB2", failedMarkerPath, marker)) {
    marker.close();
  }
  return false;
}

bool Fb2::loadMetaCache() {
  HalFile file;
  if (!Storage.openFileForRead("FB2", metaCachePath(), file)) {
    return false;
  }

  uint8_t version;
  if (!serialization::tryReadPod(file, version) || version != kMetaCacheVersion) {
    LOG_ERR(TAG, "Meta cache version mismatch");
    file.close();
    return false;
  }

  if (!serialization::tryReadString(file, title) || !serialization::tryReadString(file, author) ||
      !serialization::tryReadString(file, coverRef) || !serialization::tryReadString(file, language) ||
      !serialization::tryReadString(file, coverContentType)) {
    LOG_ERR(TAG, "Failed to read meta cache strings");
    file.close();
    return false;
  }

  int64_t cachedOffset;
  if (!serialization::tryReadPod(file, cachedOffset)) {
    file.close();
    return false;
  }
  coverBinaryOffset_ = cachedOffset;

  uint32_t cachedFileSize;
  if (!serialization::tryReadPod(file, cachedFileSize)) {
    file.close();
    return false;
  }
  fileSize = cachedFileSize;

  uint16_t sectionCount;
  if (!serialization::tryReadPod(file, sectionCount)) {
    file.close();
    return false;
  }
  sectionCounter_ = sectionCount;

  uint16_t tocItemCount;
  if (!serialization::tryReadPod(file, tocItemCount)) {
    file.close();
    return false;
  }

  tocItemCount_ = tocItemCount;
  std::vector<uint32_t>().swap(tocLut_);
  tocLut_.reserve(tocItemCount);
  for (uint16_t i = 0; i < tocItemCount; i++) {
    tocLut_.push_back(static_cast<uint32_t>(file.position()));
    std::string dummyStr;
    int16_t dummyIdx;
    if (!serialization::tryReadString(file, dummyStr) || !serialization::tryReadPod(file, dummyIdx)) {
      tocLut_.clear();
      tocItemCount_ = 0;
      file.close();
      return false;
    }
  }

  uint16_t sectionOffsetCount;
  if (serialization::tryReadPod(file, sectionOffsetCount)) {
    sectionOffsets_.clear();
    sectionOffsets_.reserve(sectionOffsetCount);
    for (uint16_t i = 0; i < sectionOffsetCount; i++) {
      SectionOffset off;
      if (!serialization::tryReadPod(file, off.startOffset) ||
          !serialization::tryReadPod(file, off.endOffset)) {
        sectionOffsets_.clear();
        break;
      }
      sectionOffsets_.push_back(off);
    }
  }

  file.close();
  return true;
}

bool Fb2::saveMetaCache() const {
  setupCacheDir();

  HalFile file;
  if (!Storage.openFileForWrite("FB2", metaCachePath(), file)) {
    LOG_ERR(TAG, "Failed to create meta cache");
    return false;
  }

  serialization::writePod(file, kMetaCacheVersion);
  serialization::writeString(file, title);
  serialization::writeString(file, author);
  serialization::writeString(file, coverRef);
  serialization::writeString(file, language);
  serialization::writeString(file, coverContentType);
  serialization::writePod(file, coverBinaryOffset_);

  const uint32_t size32 = static_cast<uint32_t>(fileSize);
  serialization::writePod(file, size32);

  const uint16_t sectionCount = static_cast<uint16_t>(sectionCounter_);
  serialization::writePod(file, sectionCount);

  const uint16_t tocItemCount = static_cast<uint16_t>(tocItems_.size());
  serialization::writePod(file, tocItemCount);

  for (const auto& item : tocItems_) {
    serialization::writeString(file, item.title);
    const int16_t idx = static_cast<int16_t>(item.sectionIndex);
    serialization::writePod(file, idx);
  }

  const uint16_t sectionOffsetCount = static_cast<uint16_t>(sectionOffsets_.size());
  serialization::writePod(file, sectionOffsetCount);
  for (const auto& off : sectionOffsets_) {
    serialization::writePod(file, off.startOffset);
    serialization::writePod(file, off.endOffset);
  }

  file.sync();
  file.close();
  LOG_INF(TAG, "Saved meta cache (%u TOC items, %u sections)", tocItemCount, sectionOffsetCount);
  return true;
}

Fb2::TocItem Fb2::getTocItem(uint16_t index) const {
  TocItem item;
  if (index >= tocItemCount_) return item;

  HalFile file;
  if (!Storage.openFileForRead("FB2", metaCachePath(), file)) return item;

  file.seek(tocLut_[index]);
  serialization::tryReadString(file, item.title);
  int16_t idx;
  if (serialization::tryReadPod(file, idx)) {
    item.sectionIndex = idx;
  }
  file.close();
  return item;
}

size_t Fb2::readContent(uint8_t* buffer, size_t offset, size_t length) const {
  if (!loaded) {
    return 0;
  }

  HalFile file;
  if (!Storage.openFileForRead("FB2", filepath, file)) {
    return 0;
  }

  if (offset > 0) {
    file.seek(offset);
  }

  const size_t bytesRead = file.read(buffer, length);
  file.close();

  return bytesRead;
}
