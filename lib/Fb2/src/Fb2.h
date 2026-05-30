#pragma once

#include <expat.h>

#include <climits>
#include <cstdint>
#include <string>
#include <vector>

class Fb2 {
 public:
  struct TocItem {
    std::string title;
    int sectionIndex = -1;
  };

  struct SectionOffset {
    uint32_t startOffset = 0;
    uint32_t endOffset = 0;
  };

 private:
  std::string filepath;
  std::string cachePath;
  std::string title;
  std::string author;
  std::string coverRef;
  std::string coverContentType;
  std::string language;
  int64_t coverBinaryOffset_ = -1;
  size_t fileSize = 0;
  bool loaded = false;
  std::vector<SectionOffset> sectionOffsets_;

  XML_Parser xmlParser_ = nullptr;
  int depth = 0;
  int skipUntilDepth = INT_MAX;

  bool inBookTitle = false;
  bool inFirstName = false;
  bool inLastName = false;
  bool inAuthor = false;
  bool inTitleInfo = false;
  bool inCoverPage = false;
  bool inLang = false;
  std::string currentAuthorFirst;
  std::string currentAuthorLast;

  bool inBody = false;
  int bodyCount_ = 0;

  std::vector<TocItem> tocItems_;
  std::vector<uint32_t> tocLut_;
  uint16_t tocItemCount_ = 0;
  int sectionCounter_ = 0;
  bool inSectionTitle_ = false;
  int sectionTitleDepth_ = 0;
  std::string currentSectionTitle_;

  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL endElement(void* userData, const XML_Char* name);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);

  bool metadataOnly_ = false;

  bool parseXmlStream();
  bool scanSectionOffsets();
  void filterNestedSections();
  bool generateSectionFiles();
  void postProcessMetadata();
  bool loadMetaCache();
  bool saveMetaCache() const;
  std::string metaCachePath() const;
  std::string sectionFilePath(int sectionIndex) const;

 public:
  explicit Fb2(std::string filepath, const std::string& cacheDir);
  ~Fb2();

  bool load();
  bool loadMetadataOnly();

  bool clearCache() const;
  void setupCacheDir() const;

  const std::string& getCachePath() const { return cachePath; }
  const std::string& getPath() const { return filepath; }

  const std::string& getTitle() const { return title; }
  const std::string& getAuthor() const { return author; }
  const std::string& getLanguage() const { return language; }
  size_t getFileSize() const { return fileSize; }

  std::string getCoverBmpPath() const;
  bool generateCoverBmp() const;
  std::string getThumbBmpPath() const;
  bool generateThumbBmp() const;

  size_t readContent(uint8_t* buffer, size_t offset, size_t length) const;

  bool extractEmbeddedCover(const std::string& outputPath) const;
  std::string findCoverImage() const;

  bool isLoaded() const { return loaded; }

  uint16_t tocCount() const { return tocItemCount_; }
  TocItem getTocItem(uint16_t index) const;

  uint16_t getSectionCount() const { return static_cast<uint16_t>(sectionOffsets_.size()); }
  std::string getSectionPath(int sectionIndex) const;
  int getSectionForTocEntry(int tocIndex) const;
  const std::vector<SectionOffset>& getSectionOffsets() const { return sectionOffsets_; }
};
