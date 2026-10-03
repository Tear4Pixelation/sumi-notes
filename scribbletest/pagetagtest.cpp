// Unit tests for the page tag summary (docs/agent/page-tags.md) and the tag index's document cache in
//  syncscribble/tagstore.cpp.  No GL context and no document, so they also build and run on their own:
//
//   g++ -std=c++14 -O2 -DNDEBUG -I . -I syncscribble -DPAGETAGTEST_MAIN
//       scribbletest/pagetagtest.cpp syncscribble/tagstore.cpp -o pagetagtest && ./pagetagtest
//   (one command, run from the repo root.)
//
// runPageTagTests() returns the number of failed checks and is also called from ScribbleTest::runAll().

#ifdef PAGETAGTEST_MAIN
#define PLATFORMUTIL_IMPLEMENTATION
#include "ulib/platformutil.h"
#define STRINGUTIL_IMPLEMENTATION
#include "ulib/stringutil.h"
#define FILEUTIL_IMPLEMENTATION
#include "ulib/fileutil.h"
#endif

#include <stdio.h>
#include <stdlib.h>

#include "tagstore.h"

static int nPageTagChecksFailed = 0;

static void pageTagCheck(bool condition, const char* what)
{
  if(!condition) {
    ++nPageTagChecksFailed;
    printf("FAIL: %s\n", what);
  }
}

static void testPageTagsRoundTrip()
{
  std::vector<TagStore::PageTags> pages(3);
  pages[0].page = 0;
  pages[0].tagIds = {"t1"};
  pages[1].page = 6;
  pages[1].tagIds = {"t1", "t12"};
  pages[1].title = "Quadratics";
  // every character the format or its readers treat specially
  pages[2].page = 11;
  pages[2].tagIds = {"t3"};
  pages[2].title = "a|b;c,d%e&f<g>h\"i'j\tk\nl";
  std::string stored = TagStore::formatPageTags(pages);
  std::vector<TagStore::PageTags> back = TagStore::parsePageTags(stored.c_str());
  pageTagCheck(back.size() == 3, "three pages survive a round trip");
  if(back.size() != 3)
    return;
  pageTagCheck(back[0].page == 0 && back[0].tagIds == pages[0].tagIds && back[0].title.empty(), "untitled page");
  pageTagCheck(back[1].page == 6 && back[1].tagIds == pages[1].tagIds && back[1].title == "Quadratics", "two tags and a title");
  pageTagCheck(back[2].title == pages[2].title, "title with separators and XML characters");
  // extractDocConfigValue reads the value raw out of the file, so nothing XML would escape may appear,
  //  and the tag index is a tab-separated line format
  pageTagCheck(stored.find_first_of("&<>\"'\t\n") == std::string::npos, "stored form needs no XML escaping");
}

static void testPageTagsEdgeCases()
{
  pageTagCheck(TagStore::parsePageTags("").empty(), "empty summary");
  pageTagCheck(TagStore::parsePageTags(NULL).empty(), "missing summary");
  // a page with no tags is not a tagged page, however it got into the string
  std::vector<TagStore::PageTags> pages(1);
  pages[0].page = 2;
  pageTagCheck(TagStore::formatPageTags(pages).empty(), "page with no tags is not written");
  pageTagCheck(TagStore::parsePageTags("2||title").empty(), "page with no tags is not read");
  pageTagCheck(TagStore::parsePageTags("junk;3|t1|").size() == 1, "malformed record skipped, the rest kept");
}

// The cache must hold a document with no tags and no page tags - its DOC line ends in two empty fields,
//  which a getline split drops, and the entry then looked stale on every refresh
static void testIndexKeepsEmptyFields(const std::string& dir)
{
  std::string indexPath = dir + "write-pagetagtest-index";
  {
    TagStore store(indexPath);
    store.setDocTags("a.svgz", 1000, {}, "");
    store.setDocTags("b.svgz", 2000, {"t1"}, "4|t2|Title");
    pageTagCheck(store.save(), "index saved");
  }
  TagStore store(indexPath);
  pageTagCheck(store.load(), "index loaded");
  std::vector<std::string> tagIds;
  std::string pageTags = "unchanged";
  pageTagCheck(store.cachedDocTags("a.svgz", 1000, &tagIds, &pageTags), "untagged document stays cached");
  pageTagCheck(tagIds.empty() && pageTags.empty(), "untagged document has nothing");
  pageTagCheck(store.cachedDocTags("b.svgz", 2000, &tagIds, &pageTags), "tagged document stays cached");
  pageTagCheck(tagIds == std::vector<std::string>{"t1"} && pageTags == "4|t2|Title", "tagged document's tags");
  pageTagCheck(!store.cachedDocTags("b.svgz", 2001, &tagIds, &pageTags), "changed mtime is a miss");

  // an index written before page tags has four fields: its documents must be reread for their pages
  FILE* file = fopen(indexPath.c_str(), "wb");
  if(file) {
    fputs("DOC\tc.svgz\t3000\tt1\n", file);
    fclose(file);
  }
  store.load();
  pageTagCheck(!store.cachedDocTags("c.svgz", 3000, &tagIds, &pageTags), "pre-page-tag entry is reread");
  remove(indexPath.c_str());
}

int runPageTagTests()
{
  nPageTagChecksFailed = 0;
  const char* tmpEnv = getenv("TMPDIR");
  std::string dir = std::string(tmpEnv && tmpEnv[0] ? tmpEnv : "/tmp") + "/";
  testPageTagsRoundTrip();
  testPageTagsEdgeCases();
  testIndexKeepsEmptyFields(dir);
  return nPageTagChecksFailed;
}

#ifdef PAGETAGTEST_MAIN
int main()
{
  int failed = runPageTagTests();
  printf("%d page tag checks failed\n", failed);
  return failed ? 1 : 0;
}
#endif
