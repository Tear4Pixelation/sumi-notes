import { existsSync } from 'node:fs';
import path from 'node:path';

// Fumadocs Studio stores every upload in `<content root>/assets` and writes
// `./assets/<file>` into whichever page is open, whatever folder that page is
// in: its upload hook is not told the page. MDX resolves a relative path from
// the page's own folder, so a page in a subfolder looks for images that are
// not there. This reads `./assets/...` the way the studio meant it.
// It must run before Fumadocs' remark-image, which turns the path into an import.

const CONTENT_ROOT = path.resolve('content/docs');
const ASSET_PREFIX = /^(\.\/)?assets\//;

interface MdNode {
  type: string;
  url?: string;
  children?: MdNode[];
}

export function remarkContentAssets() {
  return (tree: MdNode, file: { path?: string }) => {
    if (!file.path) return;
    const pageDir = path.dirname(file.path);
    if (pageDir === CONTENT_ROOT) return;

    const visit = (node: MdNode) => {
      if (node.type === 'image' && node.url && ASSET_PREFIX.test(node.url)) {
        // leave a page that really has an assets folder beside it alone
        if (!existsSync(path.join(pageDir, node.url))) {
          const target = path.join(CONTENT_ROOT, node.url);
          node.url = path.relative(pageDir, target).split(path.sep).join('/');
        }
      }
      node.children?.forEach(visit);
    };
    visit(tree);
  };
}
