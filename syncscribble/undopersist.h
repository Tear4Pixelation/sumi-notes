#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>
#include "pugixml.hpp"

class Document;
class Element;
class Page;
class ScribbleDoc;
class UndoHistoryItem;

// Persistent undo history (docs/agent/undo-persistence.md): the most recent steps of a document's undo
//  (and redo) history, written to a sidecar in app-private storage after every save and rebuilt into the
//  UndoHistory when the same file is opened again.  Never inside the document: a shared file must not
//  carry the content its author erased.

// Collects one step's items while UndoHistoryItem::persist() writes them.  Elements and pages are
//  referred to by where they are in the document *as it is while writing* - for the sidecar that is the
//  file just saved, so every reference resolves against the reopened file:
//    L<page>.<index>  element <index> of live page <page>      L<page>  a live page
//    P<def>.<index>   element <index> of a detached page def   P<def>   a detached page (deleted, or added
//    E<def>           a detached element (deleted, or added           and since undone), written in full
//                     and since undone), written in full
//  An empty string is NULL (e.g. "next" for an element that was last on its page).
class UndoPersistWriter
{
public:
  UndoPersistWriter(Document* doc) : document(doc) {}

  // starts an item record; strokeItem() also writes the element and the page the item holds it against
  pugi::xml_node item(const char* name);
  pugi::xml_node strokeItem(const char* name, Element* s, Page* page);
  std::string elementRef(Element* s, Page* page);
  std::string pageRef(Page* page);
  // something could not be expressed; the caller drops the whole history rather than save part of a step
  void fail(const char* why);

  bool failed = false;

  // --- used by UndoPersist ---
  struct Def { std::string text; };
  void beginStep();
  std::string endStep();  // the step's item records
  std::vector<int> stepDefs;  // definitions this step refers to (by id), in first-use order
  std::vector<Def> defs;  // all definitions, by id
  std::map<std::string, std::string> verify;  // "L3.12" -> "type x0 y0 x1 y1" for every on-page reference

private:
  Document* document;
  pugi::xml_document stepDoc;
  std::map<Element*, int> elementDefs;
  std::map<Page*, int> pageDefs;
  std::map<Page*, int> livePages;
  std::map<Page*, std::map<Element*, int>> pageIndex;
  int useDef(int id);
  int pageDefId(Page* page);
  int indexOnPage(Page* page, Element* s);
};

class UndoPersist
{
public:
  // write the sidecar for sd's file (just saved); removes it if there is nothing to keep
  static bool save(ScribbleDoc* sd);
  // rebuild the saved history into sd's (empty) UndoHistory; a sidecar that does not match the file
  //  exactly as it is on disk is deleted silently.  Returns the number of steps restored.
  static int restore(ScribbleDoc* sd);
  // the library renamed or moved a document: its history goes with it
  static void documentMoved(const std::string& from, const std::string& to);
  static void documentDeleted(const std::string& path);

  // where sidecars live (set at startup next to saved/); empty disables persistence
  static std::string dir;
  // off for ScribbleTest except the tests that want it, so the fixtures' saves and reopens stay as they were
  static bool enabled;

  // exposed for tests
  static std::string sidecarPath(const std::string& docPath);
  static std::string fingerprint(const std::string& docPath);
  static constexpr int FORMAT_VERSION = 1;

private:
  static std::string docKey(const std::string& docPath);
  static void cleanup();
  static UndoHistoryItem* readItem(const pugi::xml_node& node, ScribbleDoc* sd, class UndoPersistReader& rd);
};
