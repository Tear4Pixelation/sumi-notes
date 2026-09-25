#include "doclibrary.h"
#include "ulib/stringutil.h"
#include <cstdlib>
#include <cstring>

namespace DocLibrary {

const char* MARKER = ".write-library";

// files an OS drops into any folder it has shown; a directory holding only these is still "empty"
static bool isOsLitter(const std::string& name)
{
  return name == ".DS_Store" || name == ".localized" || name == "desktop.ini" || name == "Thumbs.db";
}

bool isLibraryDir(const FSPath& dir)
{
  return FSPath(dir.childPath(MARKER)).exists();
}

bool isEmptyDir(const FSPath& dir)
{
  if(!isDirectory(dir.c_str()))
    return false;
  for(const std::string& entry : lsDirectory(dir)) {
    if(!isOsLitter(entry))
      return false;
  }
  return true;
}

bool markLibrary(const FSPath& dir)
{
  FSPath marker(dir.childPath(MARKER));
  if(marker.exists())
    return true;
  FILE* file = fopen(marker.c_str(), "wb");
  if(!file)
    return false;
  static const char note[] = "This folder is Kaku's document library. Documents opened from elsewhere are copied here.\n";
  bool written = fwrite(note, 1, sizeof(note) - 1, file) == sizeof(note) - 1;
  return fclose(file) == 0 && written;
}

static FSPath asDir(const FSPath& path)
{
  return path.isDir() ? path : FSPath(path.path + "/");
}

std::string acquire(const FSPath& base, int maxTries)
{
  if(base.isEmpty())
    return "";
  std::string basePath = asDir(base).filePath();
  for(int attempt = 1; attempt <= maxTries; ++attempt) {
    FSPath candidate(attempt == 1 ? basePath + "/" : fstring("%s %d/", basePath.c_str(), attempt));
    // anything else already at this path belongs to the user: never adopt it
    bool usable = isLibraryDir(candidate) || isEmptyDir(candidate)
        || (!candidate.exists() && !FSPath(candidate.filePath()).exists() && createPath(candidate));
    if(usable && markLibrary(candidate))
      return candidate.c_str();
  }
  return "";
}

bool contains(const FSPath& library, const FSPath& file)
{
  if(library.isEmpty())
    return false;  // asDir() would turn "" into "/", which contains everything
  std::string root = canonicalPath(asDir(library));
  std::string path = canonicalPath(file);
  if(root.empty() || path.size() <= root.size())
    return false;
#if PLATFORM_WIN
  return strncasecmp(path.c_str(), root.c_str(), root.size()) == 0;
#else
  return strncmp(path.c_str(), root.c_str(), root.size()) == 0;
#endif
}

FSPath uniquePath(const FSPath& dir, const std::string& name, const std::string& ext)
{
  FSPath result = dir.child(name + "." + ext);
  for(int copyNum = 2; result.exists(); ++copyNum)
    result = dir.child(fstring("%s (%d).%s", name.c_str(), copyNum, ext.c_str()));
  return result;
}

std::vector<FSPath> findDocuments(const FSPath& root, const std::string& exts, int maxDepth, const FSPath& skip)
{
  std::vector<FSPath> result;
  std::string skipPath = skip.isEmpty() ? "" : canonicalPath(asDir(skip));
  std::vector<std::pair<FSPath, int>> stack = {{asDir(root), 0}};
  while(!stack.empty()) {
    FSPath dir = stack.back().first;
    int depth = stack.back().second;
    stack.pop_back();
    if(!skipPath.empty() && canonicalPath(dir) == skipPath)
      continue;
    for(const std::string& entry : lsDirectory(dir)) {
      if(entry.empty() || entry.front() == '.')
        continue;
      FSPath info = dir.child(entry);
      if(info.isDir()) {
        if(depth < maxDepth)
          stack.push_back({info, depth + 1});
        continue;
      }
      // the per-page files of a multi-file HTML document are not documents on their own
      if(info.extension() == "svg" && StringRef(info.baseName()).chop(3).endsWith("_page"))
        continue;
      if(containsWord(exts.c_str(), info.extension().c_str()))
        result.push_back(info);
    }
  }
  return result;
}

static bool copyVerified(const FSPath& src, const FSPath& dest)
{
  if(copyFile(src, dest) && getFileSize(dest) == getFileSize(src))
    return true;
  removeFile(dest.c_str());
  return false;
}

std::map<std::string, std::string> copyInto(const std::vector<FSPath>& docs, const FSPath& library)
{
  std::map<std::string, std::string> copied;
  for(const FSPath& doc : docs) {
    FSPath dest = uniquePath(asDir(library), doc.baseName(), doc.extension());
    if(copyVerified(doc, dest))
      copied[doc.c_str()] = dest.c_str();
  }
  return copied;
}

bool moveContents(const FSPath& from, const FSPath& to)
{
  FSPath srcDir = asDir(from), destDir = asDir(to);
  if(!createPath(destDir))
    return false;
  bool allMoved = true;
  for(const std::string& entry : lsDirectory(srcDir)) {
    FSPath src = srcDir.child(entry), dest = destDir.child(entry);
    if(entry == MARKER && FSPath(dest).exists()) {
      removeFile(src.c_str());
      continue;
    }
    // a plain rename first: instant on the same volume, and it keeps mtimes (the tag cache is keyed by them)
    if(!dest.exists() && moveFile(FSPath(src.filePath()), FSPath(dest.filePath())))
      continue;
    if(src.isDir())
      allMoved = moveContents(src, dest) && allMoved;
    else if(!dest.exists() && copyVerified(src, dest))
      removeFile(src.c_str());
    else
      allMoved = false;
  }
  if(lsDirectory(srcDir).empty())
    removeDir(srcDir);
  return allMoved;
}

std::string userDocumentsDir()
{
#if PLATFORM_WIN
  wchar_t* profile = _wgetenv(L"USERPROFILE");
  std::string home = profile ? wstr_to_utf8(profile) : "";
  return home.empty() ? "" : FSPath(home, "Documents/").c_str();
#else
  const char* homeEnv = getenv("HOME");
  std::string home = homeEnv ? homeEnv : "";
  if(home.empty())
    return "";
#if PLATFORM_LINUX
  // XDG_DOCUMENTS_DIR is localized (e.g. ~/Dokumente) and only readable from user-dirs.dirs
  const char* configEnv = getenv("XDG_CONFIG_HOME");
  std::string configHome = configEnv && configEnv[0] ? configEnv : home + "/.config";
  std::string dirsFile = readFile(FSPath(configHome, "user-dirs.dirs").c_str());
  static const char key[] = "XDG_DOCUMENTS_DIR=";
  for(const std::string& line : splitStr<std::vector>(dirsFile.c_str(), '\n', true)) {
    StringRef value = StringRef(line.c_str()).trimmed();
    if(!value.startsWith(key))
      continue;
    std::string dir = value.slice(strlen(key)).toString();
    dir.erase(std::remove(dir.begin(), dir.end(), '"'), dir.end());
    if(StringRef(dir).startsWith("$HOME"))
      dir.replace(0, 5, home);
    if(!dir.empty() && dir[0] == '/')
      return FSPath(dir + "/").c_str();
  }
#endif
  return FSPath(home, "Documents/").c_str();
#endif
}

}  // namespace DocLibrary
