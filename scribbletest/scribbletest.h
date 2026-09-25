#ifndef SCRIBBLETEST_H
#define SCRIBBLETEST_H

#include "scribbledoc.h"
#include "bookmarkview.h"

class ScribbleTest
{
friend class ScribbleApp;
public:
  ScribbleTest(const std::string &path);
  ScribbleTest(ScribbleDoc* sd, BookmarkView* bv, ScribbleMode* sm);
  ~ScribbleTest();

  void runAll(bool runsynctest = false);
  // draw/save/reload/resize round-trip for parametric shapes; returns the number of failed checks
  int shapeRoundTripTest();
  // interrupting a shape gesture (pen button / right-click) must leave no orphaned element
  int shapeInterruptTest();
  // the document theme recipe must survive save/reload, and applying it must not flatten page rulings
  int themeRoundTripTest();
  // restyling moves the theme's own ink, leaves everything else alone, and undoes in one step
  int restyleTest();
  int outlineTest();
  int outlineNestTest();
  // a locked layer must be immune to selection and to every eraser, and moving an element between
  // layers must undo both the layer and the restacking it caused
  int layerTest();
  int docStateSyncTest();
  // the curve fit must take the whole-pixel lattice out of the stroke without moving it off the path
  // the samples came from, at both dense and sparse (mouse-rate) sample spacing
  int curveFitTest();
  // hold-to-snap: a held stroke becomes a shape, the rest of the gesture scales it, and the shape as
  // recognized is its own undo step; a held scratch-out erases what it covers
  int shapeSnapTest();
  // ruling regions: the ruled tools follow a (tilted) region's lines, the region stays below all ink and
  // out of reach of ink selection and erasers, moves with its ink, and survives undo, reload and sync
  int rulingRegionTest();
  void performanceTest();
  void inputTest();
  void syncSlaveMsg(std::string msg, int level);

  // result string to be read by caller
  std::string resultStr;

  static bool exitAfterTest;

private:
  ScribbleConfig* scribbleConfig;
  ScribbleDoc* scribbleDoc;
  ScribbleArea* scribbleArea;
  ScribbleMode* scribbleMode;
  BookmarkView* bookmarkArea;
  Image* screenImg;
  Painter* screenPaint;
  Rect screenRect;
  std::string outPath;
  int nFailed;

  // shared whiteboard testing
  pugi::xml_document swbXML;
  ScribbleTest* syncSlave;
  int syncTestNum;
  bool waitForSyncDone;
  void startSyncTest(int testnum);
  void waitForSync();
  void checkStrokeMap(ScribbleDoc* doc, const char* msg);

  static const int pen = INPUTSOURCE_PEN;
  static const int press = INPUTEVENT_PRESS;
  static const int release = INPUTEVENT_RELEASE;
  void undo() { scribbleDoc->doCommand(ID_UNDO); }
  void redo() { scribbleDoc->doCommand(ID_REDO); }
  void doCommand(int cmd) { scribbleDoc->doCommand(cmd); }
  bool testCompareFiles(const char* f1, const char* f2, bool svgonly = false);
  void ie(Dim x, Dim y, Dim p, int src, int ev = 0, int mm = 0);
  void mtinput(inputevent_t ev1, Dim x1, Dim y1, inputevent_t ev2, Dim x2, Dim y2);
  void ss(Dim offset);
  void s1(Dim xoffset, Dim yoffset);
  void s2(Dim xoffset, Dim yoffset);
  void s3(Dim xoffset, Dim yoffset);
  void f2(Dim xoffset, Dim yoffset);
  void hr(Dim xoffset, Dim yoffset);
  void s3();
  void s4();

  void test0();
  void test1();
  void test2();
  void test3();
  void test4();
  void test5();
  void test6();
  void test7();
  void test8();
  void test9();
  void test10();
  void test11();
  void test12();
  void test13();
  void test14();
  void test15();
  void synctest01();
  void synctest01slave1();
  void synctest01slave2();
};

#endif
