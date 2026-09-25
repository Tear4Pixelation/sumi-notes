import Link from "fumadocs-core/link";
import type { ReactNode } from "react";
import { ChipIcon } from "./chip-icon";
import { chipHost, chipIcon, chipStyle } from "./chip-shared";

export interface ChipProps {
  /** makes the chip a link; external links also get the site's favicon */
  href?: string;
  /** a name from `chipColors` or any CSS color; defaults to the text color */
  color?: string;
  /** image URL, replacing the favicon */
  icon?: string;
  /** the label; defaults to the link's host */
  children?: ReactNode;
}

/**
 * A small pill, like a citation in an AI chat: `<Chip href="https://…">Label</Chip>`.
 * Works inline in a sentence.
 */
export function Chip({ href, color, icon, children }: ChipProps) {
  const iconSrc = chipIcon(href, icon);
  const body = (
    <>
      {iconSrc && <ChipIcon src={iconSrc} />}
      <span className="chip-label">{children ?? chipHost(href) ?? href}</span>
    </>
  );

  // not-prose keeps the docs typography from underlining the link and adding margins to the icon
  if (!href) {
    return (
      <span className="chip not-prose" style={chipStyle(color)}>
        {body}
      </span>
    );
  }
  return (
    <Link
      href={href}
      className="chip not-prose"
      style={chipStyle(color)}
      title={href}
    >
      {body}
    </Link>
  );
}
