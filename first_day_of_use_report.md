Observations on my first day with sumi.

# Problems

## Pen Thickness

- Relative size state isn't held properly

> The app might have crashed or something. Anyway, the state of the relative size check mark was not held properly, so I ended up with a pen that was 144 wide (a pen that was 3.2 in absolute measure or something along those lines). It's happened multiple times. The first few times, I got a pen that was 0.6 wide because this switch happened the other way around.

- Relative size isn't used in the "set width" dialog for selections

> When you select a stroke while your pen sizes are relative. The button that changes the stroke width of the selection uses those width numbers as absolutes, meaning that you have three very small presets.

- The 144-width pen could no longer be edited, so I had to delete it, resulting in the permanent loss of one of the three width presets.

> We should remove the right-click options to delete and insert a new preset and fix this bug. We should always lock the amount of width presets to 3, and also recover to 3 if there is only 2 left.

## Shape snapping/editing

- The snapping to specific ego angles is too eager. 

> In the shape recognition mode, it should be a little bit less eager, but similar to how it is right now. But once you're editing the stroke, that's when it becomes important. Because the way you have implemented it eliminates the option to have a slightly diagonal line at all. Because you're snapping to an angle. So a very long line is snapped to the same angle as a very short one, instead of distance. We should snap using distance from the line if it were at zero, forty-five, ninety degrees, and so on, and not the angle difference.

- Getting in and out of shape edit mode is annoying.

> To enter shape edit mode, you have to select the shape using the Select tool. The problem with this is that when you have just snapped to a shape and it turned out just a little bit wrong, you don't want to have to switch to Select for this. The solution is to allow taps of the finger on shapes and images to count as selecting.
> 
> A similar thing goes for edit mode. Once you are in it, it is worse because you cannot exit it without doing something else. Unlike selection mode, when you click outside of it with another tool, you activate that tool immediately. So, if I want to draw multiple lines and I am in the Shapes tool, I cannot get out of edit mode without drawing another line.
> 
> A solution to this has two approaches, and they are not exclusive: either tap outside the selection to clear it, or the first action that happens outside the selection simply unselects it and is not performed, just like it is with normal selection.

## PDF Scanning

- The algorithm for detecting corners does not work at all, even in comparatively very good environments.

- The “Add another page” and “Done” buttons are flipped in position. They should be the other way around, and both should be less wide, with spacing between them in the middle.

## Reflow/Insert Space

- The two-line one selects stuff in the line above as well, somehow.

> When trying to use the insert rule space to move something downwards in the two-ruled mode, it seems to select from the line above the cursor as well.

- We might want to split the rules-space insert into moving down and moving to the side: one for moving right and one for moving down.

## New features we need

- Editor tabs

> In the Unified Sidebar, along with the other options, there should be tabs. When you open a new document or any document, it is placed in a new tab, and the old one is kept in the tabs. This makes it easier to switch between two documents you're editing at the same time without having to split between them. There should also be an extra button in the toolbar to enter the tabs view without altering the state of the sidebar, so that I can access tabs and then quickly look at outlines without having to switch between modes.

- Page organization

>  I think we're going to put this in the unified sidebar as another part that allows you to reorder, select, export, and delete pages. Of course, those actions have to be available when selecting multiple as well.

## Other

- Multiple crashes happened.

- There are many icons, so the toolbars and the icons at the bottom of the sidebar are too small on the iPad.

- Zoom snapping behaves very weirdly. It snaps to some position on the current page you're on, even when it isn't snapping to a specific zoom amount. It should not be doing that.

- Countless times, the content of an arrow pop-up did not stay inside that arrow pop-up. 

> It seems that the arrow pop-up decided to place itself differently because of space constraints, and the content did not follow. This happened especially often when selecting one of the options that spawns an arrow pop-up from the selection context menu, for example, the stroke thickness one.

# Questions

- Screenshotting on PDFs

> When I tried to screenshot a PDF, it worked pretty well, but it exceeded my expectations because it extracted only the text into the screenshot, without the background of the PDF that I had just scanned. It was just an image. How did you implement this, and was it intentional?

# Priority

Start with the Width fixes then Shape Snapping, Tabs and Crashes.
