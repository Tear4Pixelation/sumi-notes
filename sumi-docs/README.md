# sumi-docs

User documentation for [Sumi Notes](https://sumi-notes.com), built with [Fumadocs](https://fumadocs.dev) on
Next.js and exported as a static site.

## Working on it

```bash
pnpm install
pnpm dev        # live preview at http://localhost:3000
pnpm studio     # Fumadocs Studio, the visual editor
pnpm build      # static export into out/
pnpm start      # serve out/ locally
```

## Where things are

- `content/docs/` - the pages, as `.mdx`. Order and section titles come from each folder's `meta.json`;
  `"..."` there means "every other page, alphabetically".
- `content/docs/assets/` - screenshots. Delete an image when no page references it any more.
- `src/components/chip*` - the `<Chip>` badge used on the feature list, plus its Studio editor spec
  (`chip.editor.tsx`), registered in `fumadocs-studio.config.ts`.
- `src/lib/source.ts` - the content source adapter.

Pages saved from Studio can pick up `&#x20;` entities and trailing `\` line breaks; strip them before
committing.
