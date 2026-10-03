// How <Chip> appears in Fumadocs Studio. The editor never runs the real
// component, so this is a look-alike sharing its stylesheet (chip.css).
//
// Editor limitation (v0.5): only a chip that sits alone in its paragraph is
// drawn like this. One in the middle of a sentence shows as plain label text
// (a label-less `<Chip href="…" />` as its source); it saves correctly either
// way, and the MDX tab shows the tags.
import {
  type ComponentRenderProps,
  emptyComponent,
  type UiComponentSpec,
} from "@fumadocs-editor/ui";
import { chipColors, chipIcon, chipStyle } from "./chip-shared";

function ChipPreview({ props, children }: ComponentRenderProps) {
  const iconSrc = chipIcon(props.href, props.icon);
  return (
    <span className="chip" style={chipStyle(props.color)}>
      {iconSrc && (
        // biome-ignore lint/performance/noImgElement: Studio is not a Next.js app
        <img
          className="chip-icon"
          src={iconSrc}
          alt=""
          contentEditable={false}
          onError={(event) => {
            event.currentTarget.hidden = true;
          }}
        />
      )}
      {children}
    </span>
  );
}

export const chipSpec: UiComponentSpec = {
  name: "Chip",
  label: "Chip",
  contentRegion: { region: "label", placeholder: "Label…" },
  props: [
    { name: "href", label: "Link", type: "string", placeholder: "https://…" },
    {
      name: "color",
      label: "Color",
      type: "enum",
      options: Object.keys(chipColors),
    },
    {
      name: "icon",
      label: "Icon URL",
      type: "string",
      placeholder: "defaults to the link's favicon",
    },
  ],
  regions: { label: "chip-label" },
  insert: (specs) => emptyComponent(chipSpec, specs),
  render: ChipPreview,
};
