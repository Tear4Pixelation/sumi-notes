# History panel

The toolbar's **History** button (`ic_menu_history`, formerly the undo arrow) opens a panel of ticks -
one per undo step - that scroll under a fixed mark at the centre. `ButtonDragTimeline` in
`touchwidgets.cpp` replaces the older `ButtonDragDial`, which was the same interaction on a circle. A
circle has no ends, so "where am I" and "how much further can I go" could only be hinted at, by a wedge
that grew when you over-turned it; the ends of a line are simply where the ticks stop.

The button still behaves like an undo button on a plain tap (`onStep(-1)`), and `Ctrl+Z` plus the menu
entries are untouched - which is what makes the rename cost recognition rather than capability.

- **Drag right undoes, left redoes** - the opposite of "right is the future", and deliberate. With the
  mark pinned at the centre, what is under the finger is the tape, not the playhead, so dragging the tape
  right must bring earlier ticks to the centre. The other sign slides the ticks against the finger
  pushing them.
- **Event coords and layout units are the same space** (the framebuffer is scaled as a whole for DPI), so
  one tick of drag is one tick of ruler with no conversion. A screenshot will disagree - the capture is
  downscaled from the app's larger framebuffer - so measure this from a trace, not from pixels.
- **Two lanes, chosen by where the drag is, not by which button.** A stylus has one input and cannot
  express "the other button", and one panel showing both features explains them once, where two separate
  controls would each need their own explanation. Steps **only fire once the pointer is inside the
  panel**: the press starts on the button above it, so every gesture travels down into a lane, and that
  travel is never purely vertical - acting on its sideways component would lock the gesture into whichever
  lane it happened to be crossing. The first step that lands locks the lane, since scrubbing sideways
  always drifts vertically too. Right click (or long press) still goes straight to the select lane,
  locked, for anyone who already has it in their fingers.
- **Panel width is `2*max(back, fwd) + 1` ticks**, clamped to `TIMELINE_MIN/MAX_TICKS`, fixed for the
  whole gesture. The ruler is centred on the current position, so sizing it to `back + fwd + 1` pushes the
  end cap off the edge exactly when the history is lopsided - which is most of the time. The cost is that
  a lopsided history looks off-centre at rest.
- **The select lane's range is derived, not measured.** It walks the same history backwards, so it reaches
  at most `undoSteps()` - an upper bound, since a step that added no strokes is skipped rather than
  counted - and forward nothing, as nothing is selected at press. Without this it drew an open ruler both
  ways, promising history that wasn't there. Being refused pins the true end (`applySteps`), which is also
  how any range left at -1 is discovered.
- **Holding against the window edge keeps stepping** on a timer. A ruler runs out of screen where a dial
  did not, so whichever direction points at the near edge has little room. This is why the button sits at
  the end of the *tools* row rather than in the file ops panel: there it was at the right edge, where the
  rightward drag - *undo*, the common case - had almost no travel. It is still needed, since a vertical
  toolbar cramps redo instead, and pointer capture would fix only the mouse. The timer must be torn down
  on release and on a fresh press, or it keeps undoing after the finger is gone.
- The button is a verb, not a tool, and never shows the tools' checked state; the separator before it is
  what keeps it from reading as an eighth mode. This is the layout on *every* platform: `vertToolbar`
  (`mainwindow.h`) is initialised false and never assigned, so the `tbcfg`-driven vertical toolbar beside
  it is dead code, and phones and tablets get these same floating panels. Narrow screens are handled by
  `AutoAdjContainer` hiding widgets in `ui-priority` order instead.
- **`undoRedoBtn` is priority 5, above the tools' 4**, so the tools are dropped *first*. At phone density
  (preview with `--screenDPI=522`, see below) that leaves the Editing panel holding History and nothing
  else, with all seven tools in the overflow menu. The priority predates the move - it made more sense
  when the button lived in the file ops panel - but the result now looks like a bug.

To preview another device's layout, override the config from the command line: `screenDPI` sets
`paintScale = dpi/150`, so a higher value shrinks the window's logical width exactly as a denser screen
does, and `agent-display.sh run` passes extra args through to the app:

```
tools/agent-display.sh run --debug -- --screenDPI=141   # ~iPad landscape
tools/agent-display.sh run --debug -- --screenDPI=202   # ~iPad portrait
tools/agent-display.sh run --debug -- --screenDPI=522   # ~phone
```

The headless output stays 1280x720, so this reproduces width-driven layout, not portrait aspect.
- **`UndoHistory::undoSteps()/redoSteps()`** count `HEADER` items either side of `pos`: one step is one
  action, so this is not derivable from `hist.size()` and `pos`.
- **Colors and weights are hardcoded, and must stay theme-neutral.** The panel is custom-drawn onto an
  `SvgCustomNode`, so there is no theme color to read: ticks and idle labels are one mid-gray, and the
  ends are set apart by weight and height rather than brightness, since anything tuned to the dark theme
  recedes on the light one. An earlier attempt to highlight the armed lane with a translucent filled rect
  rendered far brighter than its alpha suggested and swallowed the label on it; the armed lane is named
  in bold and in its own color instead.
- The first-run hint is retired **by use** - the first step that actually lands - not by dismissal, so it
  cannot be waved away without being understood, and dragging against a dead end does not count. Stored
  as `historyHintDone` in `ScribbleConfig`.
