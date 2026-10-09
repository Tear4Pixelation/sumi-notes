Following issues are still encountered. Start right now with the high priority ones.

# High prio
- Highlighter should go behind text that is on scanned documents as well.
- Current page is the one that is in the middle of the screen
> The page that is where the middle pixels of the screen are is the active one
- Relative size isn't correctly applied when selecting strokes inside a paper patch
> When selecting strokes in a paper patch and setting their width through the context menu, they get the relative width relative to the page it seems
- Sometimes, we encounter following symptoms together'
	+ Scrilling becomes stiff (No overshoot)
	+ Sidebar UI is still responsive but actions don't happen (for some time)
	+ Switching pages takes like 10s while in this state

# Mid prio
- fit snapping is a bit buggy, sometimes you get sent to another page.
- Add option to import pdf as new pages to this document
- When a scribble is detected without long press, the scrible without erasing should be added to undo history in case the detection was faulty
- When there is a long vertical line, make the vertical space insertion also work only on a side of this line like the horizontal/lined select (they break line at the vertical line)
- The button to change how the paper patch is ofset should be movable throughout the paper patch so that I can align it with something clearly visible.

# Low prio
- Library view does not resize with the app
- more vibrant highlighter (less transparent and more saturated)
- Music sheet paper

# New features
(don't implement yet)
- New "+" menu next to add page. It will work as a menu for "add things to this page". It will currently contain adding a scanned doc and an image to the page, a paper patch, and maybe a new coordinate system paper patch?