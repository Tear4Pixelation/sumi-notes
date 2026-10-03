// Fumadocs Studio, the visual editor: `pnpm studio`.
import { defineConfig } from "@fumadocs-editor/studio";
import {
  admonitionSpec,
  filesFenceSpecs,
  fumadocsUiComponents,
} from "@fumadocs-editor/ui";
import { chipSpec } from "./src/components/chip.editor";

export default defineConfig({
  // Studio's defaults, plus our own components
  components: [
    ...fumadocsUiComponents,
    admonitionSpec,
    ...filesFenceSpecs,
    chipSpec,
  ],
  styles: ["./src/components/chip.css"],
});
