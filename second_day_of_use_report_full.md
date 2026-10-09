The following problems are still being encountered with the software:

## --- implemented ---

- **Reordering layers:** I couldn't find out how to reorder layers on my tablet. The sidebar button has the same icon as the split-right-or-left icon. Instead, the split-right-or-left icon should be a rotated version of the top-and-bottom ones.

- **Zoom snapping:** Zoom snapping does not work as intended. When you zoom close to fitting the page horizontally, a toast should appear in the center of the screen that simply says “Fit.” This tells the user that if they let go now, it will fit to the page. This fit stage should not have any gaps around it, like the current one that is snapped to, and the double-click should also snap to that. If the zoom factor is smaller than the fit zoom factor, then right and left movement should be disabled. The same should apply when the zoom is exactly fit.

- **Ruler tools:** The double ruling tool for moving downwards should only select parts of a line—only the part that is to the right of the cursor when the cursor is inside that line, not when it is in the line above, which also triggers that effect. You would have to be two lines above in order to just move everything down. For a single line, it should move everything from that line down if you are very close to the line. If you are in the middle of the line, you should move only parts of that line, and if you are very close to the line, you move the entire part that is below it. This way, we can also undo the splits of horizontal and vertical movement.

## --- open ---

- **Naming outlines on creation:** This should work like creating tags, just with outlines, because having an untitled outline is never a good idea.
- **Split icons:** The split-right and split-left icons are the same as the sidebar icon. Instead, we should rotate the current icon for all four positions.
- **Multiple tag selection:** Exact tag matches should be above top-level matches in the area where there is not a multiple match. There should be some text below the separator that says “Singular match” or something like that. Above the separator, there should be text that says, “All matching tags shown in the library.”
- **Tags in the library:** Notebooks shown in the library should include the tags below, especially when they have other notebooks named very similarly.
- **Current page selection:** The way that the current page is selected seems strange. The page that takes up the biggest amount of space on the screen is the one that’s currently active.

## --- discuss then implement ---

Put up supagents for the open ones, then we can discuss how to implement the following

- **Page management**: The multifunctional sidebar should have a section for pages that shows pages as their thumbnails, maybe two in a row, I think. There should be a way to delete pages, move pages around, and select multiple pages. We also have to be able to export the selected page or a single page to PDF, extract it as an SVGZ, or export it as an image or a list of images.

- **Editor tabs**: In the Unified Sidebar, along with the other options, there should be tabs. When you open a new document or any document, it is placed in a new tab, and the old one is kept in the tabs. This makes it easier to switch between two documents you're editing at the same time without having to split between them. There should also be an extra button in the toolbar to enter the tabs view without altering the state of the sidebar, so that I can access tabs and then quickly look at outlines without having to switch between modes.
