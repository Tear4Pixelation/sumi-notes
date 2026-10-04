// Unit tests for the document library's filesystem logic in syncscribble/doclibrary.cpp. Like layertest.cpp
//  these need no GL context and no document, so they also build and run on their own:
//
//   g++ -std=c++14 -O2 -DNDEBUG -I . -I syncscribble -isystem stb -DLIBRARYTEST_MAIN
//       scribbletest/librarytest.cpp syncscribble/doclibrary.cpp -o librarytest && ./librarytest
//   (one command, run from the repo root.)
//
// Everything happens in a fresh directory under the system temp dir, which is removed afterwards.
// runLibraryTests() returns the number of failed checks and is also called from ScribbleTest::runAll().

#ifdef LIBRARYTEST_MAIN
#define PLATFORMUTIL_IMPLEMENTATION
#include "ulib/platformutil.h"
#define STRINGUTIL_IMPLEMENTATION
#include "ulib/stringutil.h"
#define FILEUTIL_IMPLEMENTATION
#include "ulib/fileutil.h"
#endif

#include <stdio.h>
#include <stdlib.h>

#include "doclibrary.h"

static int nLibraryChecksFailed = 0;

static void libraryCheckTrue(bool condition, const char* what)
{
  if(!condition) {
    ++nLibraryChecksFailed;
    printf("FAIL: %s\n", what);
  }
}

static void writeTestFile(const FSPath& path, const char* content)
{
  FILE* file = fopen(path.c_str(), "wb");
  if(file) {
    fputs(content, file);
    fclose(file);
  }
}

// A fresh install creates and marks its directory, and the next run finds the same one again.
static void testAcquireFresh(const FSPath& scratch)
{
  std::string library = DocLibrary::acquire(scratch.child("Fresh"));
  libraryCheckTrue(library == scratch.childPath("Fresh/"), "a missing directory is created and used");
  libraryCheckTrue(DocLibrary::isLibraryDir(library), "the acquired directory is marked");
  writeTestFile(FSPath(library).child("note.svgz"), "x");
  libraryCheckTrue(DocLibrary::acquire(scratch.child("Fresh")) == library,
      "a directory marked by an earlier run is reused although it is no longer empty");
}

// The whole point of the marker: a folder the user already has, with their own files in it, is never
//  adopted - the browser would recurse into it and write its tag index there.
static void testAcquireSkipsUserFolder(const FSPath& scratch)
{
  FSPath userFolder = scratch.child("Write/");
  createPath(userFolder);
  writeTestFile(userFolder.child("thesis.svgz"), "mine");
  std::string library = DocLibrary::acquire(scratch.child("Write"));
  libraryCheckTrue(library == scratch.childPath("Write 2/"), "a non-empty unmarked folder is skipped for 'Write 2'");
  libraryCheckTrue(!DocLibrary::isLibraryDir(userFolder), "the user's folder is not marked");

  // a plain file with the name wanted for the library is in the way too
  writeTestFile(scratch.child("Other"), "a file, not a folder");
  libraryCheckTrue(DocLibrary::acquire(scratch.child("Other")) == scratch.childPath("Other 2/"),
      "a file with the library's name is not treated as a missing directory");
}

static void testEmptyDir(const FSPath& scratch)
{
  FSPath empty = scratch.child("Empty/");
  createPath(empty);
  writeTestFile(empty.child(".DS_Store"), "litter");
  libraryCheckTrue(DocLibrary::isEmptyDir(empty), "a folder holding only OS litter counts as empty");
  libraryCheckTrue(DocLibrary::acquire(empty) == empty.path, "an empty existing folder is adopted");
  FSPath hidden = scratch.child("Hidden/");
  createPath(hidden);
  writeTestFile(hidden.child(".secret"), "user data");
  libraryCheckTrue(!DocLibrary::isEmptyDir(hidden), "a hidden file of the user's makes a folder non-empty");
}

// "Write 2/x" starts with the characters of "Write" - a prefix compare without the separator would call it
//  part of the "Write" library and skip importing it.
static void testContains(const FSPath& scratch)
{
  FSPath library = scratch.child("Lib/");
  libraryCheckTrue(DocLibrary::contains(library, library.child("doc.svgz")), "a document in the library is inside it");
  libraryCheckTrue(DocLibrary::contains(library, library.child("sub/doc.svgz")), "a document in a subfolder is inside it");
  libraryCheckTrue(!DocLibrary::contains(library, scratch.child("Lib 2/doc.svgz")), "a sibling sharing the name prefix is outside");
  libraryCheckTrue(!DocLibrary::contains(library, scratch.child("Lib/../doc.svgz")), "a path escaping with .. is outside");
  libraryCheckTrue(!DocLibrary::contains(library, library), "the library itself is not a document inside it");
  libraryCheckTrue(!DocLibrary::contains(FSPath(""), library.child("doc.svgz")), "no library contains nothing");
  // iOS: /var is a symlink to /private/var and paths come in either spelling, the library's own included.
  //  Only stripping the file's prefix made every document look external, so each open imported a copy.
  FSPath iosLibrary("/private/var/mobile/Containers/Data/Application/A1/Documents/Sumi/");
  libraryCheckTrue(DocLibrary::contains(iosLibrary, iosLibrary.child("doc.svgz")),
      "an iOS library under /private/var contains its own documents");
  libraryCheckTrue(DocLibrary::contains(iosLibrary, FSPath("/var/mobile/Containers/Data/Application/A1/Documents/Sumi/doc.svgz")),
      "a /var spelling of a document is inside a /private/var library");
  libraryCheckTrue(DocLibrary::contains(FSPath("/var/mobile/Containers/Data/Application/A1/Documents/Sumi/"), iosLibrary.child("doc.svgz")),
      "a /private/var spelling of a document is inside a /var library");
  libraryCheckTrue(!DocLibrary::contains(iosLibrary, FSPath("/var/mobile/Containers/Data/Application/A1/Documents/doc.svgz")),
      "a document next to the iOS library is still outside it");
}

static void testUniquePath(const FSPath& scratch)
{
  FSPath dir = scratch.child("Unique/");
  createPath(dir);
  libraryCheckTrue(DocLibrary::uniquePath(dir, "Notes", "svgz") == dir.child("Notes.svgz"), "a free name is used as is");
  writeTestFile(dir.child("Notes.svgz"), "1");
  writeTestFile(dir.child("Notes (2).svgz"), "2");
  libraryCheckTrue(DocLibrary::uniquePath(dir, "Notes", "svgz") == dir.child("Notes (3).svgz"), "taken names are numbered past");
}

static void testFindDocuments(const FSPath& scratch)
{
  FSPath root = scratch.child("Old/");
  createPath(root.child("a/b/c/d/"));
  createPath(root.child(".hidden/"));
  createPath(root.child("Lib/"));
  writeTestFile(root.child("top.svgz"), "x");
  writeTestFile(root.child("a/one.svgz"), "x");
  writeTestFile(root.child("a/b/c/d/deep.svgz"), "x");
  writeTestFile(root.child("doc_page001.svg"), "x");
  writeTestFile(root.child("drawing.svg"), "x");
  writeTestFile(root.child("photo.png"), "x");
  writeTestFile(root.child(".hidden/secret.svgz"), "x");
  writeTestFile(root.child("Lib/already.svgz"), "x");
  auto found = DocLibrary::findDocuments(root, "svgz svg", 3, root.child("Lib/"));
  auto has = [&found](const FSPath& path) { return std::find(found.begin(), found.end(), path) != found.end(); };
  libraryCheckTrue(has(root.child("top.svgz")) && has(root.child("a/one.svgz")), "documents are found at depth");
  libraryCheckTrue(has(root.child("drawing.svg")), "a plain svg document is found");
  libraryCheckTrue(!has(root.child("a/b/c/d/deep.svgz")), "the depth limit is honoured");
  libraryCheckTrue(!has(root.child("doc_page001.svg")), "page files of an HTML document are not documents");
  libraryCheckTrue(!has(root.child("photo.png")), "other file types are ignored");
  libraryCheckTrue(!has(root.child(".hidden/secret.svgz")), "hidden folders are skipped");
  libraryCheckTrue(!has(root.child("Lib/already.svgz")), "the library itself is skipped when nested in the source");
  libraryCheckTrue(found.size() == 3, "exactly the three expected documents are found");
}

static void testCopyInto(const FSPath& scratch)
{
  FSPath source = scratch.child("CopySrc/"), library = scratch.child("CopyLib/");
  createPath(source.child("x/"));
  createPath(library);
  writeTestFile(source.child("Same.svgz"), "first");
  writeTestFile(source.child("x/Same.svgz"), "second");
  auto copied = DocLibrary::copyInto({source.child("Same.svgz"), source.child("x/Same.svgz")}, library);
  libraryCheckTrue(copied.size() == 2, "both documents are copied");
  libraryCheckTrue(readFile(library.childPath("Same.svgz").c_str()) == "first"
      && readFile(library.childPath("Same (2).svgz").c_str()) == "second", "a name collision is numbered, not overwritten");
  libraryCheckTrue(FSPath(source.child("Same.svgz")).exists(), "copying leaves the original in place");
}

static void testMoveContents(const FSPath& scratch)
{
  std::string from = DocLibrary::acquire(scratch.child("MoveFrom"));
  std::string to = DocLibrary::acquire(scratch.child("MoveTo"));
  createPath(FSPath(from).child("sub/"));
  writeTestFile(FSPath(from).child("doc.svgz"), "doc");
  writeTestFile(FSPath(from).child("sub/nested.svgz"), "nested");
  writeTestFile(FSPath(from).child(".write-tags"), "tags");
  libraryCheckTrue(DocLibrary::moveContents(from, to), "a move into a freshly acquired library succeeds");
  libraryCheckTrue(readFile(FSPath(to).childPath("doc.svgz").c_str()) == "doc", "documents arrive");
  libraryCheckTrue(readFile(FSPath(to).childPath("sub/nested.svgz").c_str()) == "nested", "subfolders arrive");
  libraryCheckTrue(FSPath(FSPath(to).child(".write-tags")).exists(), "the tag index moves with the documents");
  libraryCheckTrue(DocLibrary::isLibraryDir(to), "the destination is still marked");
  libraryCheckTrue(!isDirectory(FSPath(from).filePath().c_str()), "the emptied source is removed");
}

int runLibraryTests()
{
  nLibraryChecksFailed = 0;
  const char* tmpEnv = getenv("TMPDIR");
  if(!tmpEnv || !tmpEnv[0]) tmpEnv = getenv("TEMP");
  FSPath scratch(fstring("%s/write-librarytest-%d/", tmpEnv && tmpEnv[0] ? tmpEnv : "/tmp", rand() % 1000000));
  if(!createPath(scratch)) {
    printf("FAIL: cannot create %s\n", scratch.c_str());
    return 1;
  }
  testAcquireFresh(scratch);
  testAcquireSkipsUserFolder(scratch);
  testEmptyDir(scratch);
  testContains(scratch);
  testUniquePath(scratch);
  testFindDocuments(scratch);
  testCopyInto(scratch);
  testMoveContents(scratch);
  removeDir(scratch);
  return nLibraryChecksFailed;
}

#ifdef LIBRARYTEST_MAIN
int main()
{
  int failed = runLibraryTests();
  printf("%d library checks failed\n", failed);
  return failed ? 1 : 0;
}
#endif
