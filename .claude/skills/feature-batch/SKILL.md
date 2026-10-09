---
name: feature-batch
description: How we work when the user hands over a list of features to implement - one subagent per feature, each in its own git worktree with its own agent display. Use when given two or more features/tasks to build.
---

# Implementing a list of features

1. **Split.** One feature = one subagent. Group only features that touch the same code
   (they would conflict anyway). Read the relevant `docs/agent/*.md` yourself first only if
   needed to split; otherwise let the subagent read it.
2. **Worktree per subagent.** Spawn each with `isolation: "worktree"` so edits and builds
   never collide. Spawn all independent ones in a single message so they run concurrently.
   **Model:** for difficult tasks (root-cause investigations, subtle bugs, cross-cutting
   changes, anything where a wrong guess is costly) pass `model: "opus"` (Opus 5.5) on the
   Agent call; routine, well-scoped features use the default.
3. **Own display per subagent.** `tools/agent-display.sh` supports parallel displays: set
   `AGENT_DISPLAY_NAME=<feature-slug>` in every call (state lives in
   `$XDG_RUNTIME_DIR/write-agent-display-<name>`). Without it, agents share one display and
   fight over it. Tell each subagent its name and to `stop` its display when done. Still
   never run `Release/Sumi` / `Debug/Sumi` directly (see CLAUDE.md, Agent display).
4. **Don't waste tokens.**
   - Subagent prompts are self-contained but short: the goal, the relevant `docs/agent/` file,
     the display name, and what to report. No pasted file dumps.
   - Subagents read only the parts they need, grep before reading, avoid re-reading edited
     files, and take a screenshot only when the result must be seen (blind and scripted).
   - Ask for a terse final report: what changed, files, how verified, open issues. No diffs.
   - Don't re-verify what a subagent already verified; don't poll - wait for notifications.
5. **Build sparingly, and only through the gate.** The user's PC cannot take many
   subagents compiling at once. Batch edits, build when a chunk is done (not after every
   change), and never run bare `make`/`./gww`: use `tools/build-gate.sh make ...` (from
   `syncscribble/`). It caps all agents together at 90% CPU / 80% RAM and queues builds
   beyond 2 at a time - if it prints "queued", just wait. Tell every subagent this.
6. **Verify** per CLAUDE.md: tests that fail against the broken code, a screenshot of the
   result for UI work, and update the matching `docs/agent/*.md` for non-obvious decisions.
7. **Integrate.** Each subagent commits on its worktree branch. Merge branches one at a time
   (resolve conflicts, rebuild if two features overlap), then report to the user what landed,
   what did not, and anything surprising. Clean up finished worktrees afterwards.
8. **Keep the user posted**: say what is being spawned up front, report each completion
   briefly, flag blockers immediately.
