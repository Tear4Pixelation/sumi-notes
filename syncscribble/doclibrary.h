#pragma once

#include "ulib/fileutil.h"
#include <string>
#include <vector>
#include <map>

// The document library: the one directory the tag document browser owns. Filesystem logic only - no
//  dependency on ScribbleApp or the GUI - so it can be tested standalone (scribbletest/librarytest.cpp).
//
// A library directory is marked with a MARKER file. That marker is what lets acquire() tell "our library
//  from a previous run" apart from "a folder the user happens to have called Write", so we never adopt,
//  and then recurse into and write tag indexes over, a directory we didn't create.
namespace DocLibrary {

extern const char* MARKER;

bool isLibraryDir(const FSPath& dir);
// true if dir exists and holds nothing but OS litter (.DS_Store, desktop.ini, Thumbs.db, .localized)
bool isEmptyDir(const FSPath& dir);
// marks dir as a library; fails if the marker cannot be written, which is also our writability test
bool markLibrary(const FSPath& dir);

// Returns the first usable directory among base, "base 2", "base 3", ...: one already marked as a
//  library, an existing empty directory, or one that can be created. The result is marked and has a
//  trailing '/'. Returns "" if nothing writable was found.
std::string acquire(const FSPath& base, int maxTries = 50);

// whether file lies inside library (compared on canonical paths)
bool contains(const FSPath& library, const FSPath& file);

// dir/name.ext, or dir/name (2).ext, ... - the first that does not exist
FSPath uniquePath(const FSPath& dir, const std::string& name, const std::string& ext);

// Write documents (extensions in exts, space separated) under root, skipping hidden entries, split-page
//  SVGs and the directory skip (normally the library itself, which may be nested under root), and not
//  descending more than maxDepth levels - root may be a whole home directory.
std::vector<FSPath> findDocuments(const FSPath& root, const std::string& exts, int maxDepth,
    const FSPath& skip = FSPath());

// Copies each document into library under a unique name; returns original -> copy for those that
//  succeeded. A copy is only counted once its size matches, since copyFile() does not report write errors.
std::map<std::string, std::string> copyInto(const std::vector<FSPath>& docs, const FSPath& library);

// Moves everything in from (including the marker and the tag index) into to, which must be empty or
//  missing; from is removed if it ends up empty. Returns false if anything was left behind.
bool moveContents(const FSPath& from, const FSPath& to);

// the user's documents folder (XDG_DOCUMENTS_DIR on Linux, ~/Documents elsewhere); "" if unknown
std::string userDocumentsDir();

}  // namespace DocLibrary
