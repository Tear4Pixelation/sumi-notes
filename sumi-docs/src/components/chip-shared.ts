// Used by both the site's <Chip> and its Studio preview, so nothing here may
// import from Next.js.
import type { CSSProperties } from "react";

/** Named colors; any other value is passed through as a CSS color. */
export const chipColors = {
  gray: "#71717a",
  red: "#dc2626",
  orange: "#ea580c",
  yellow: "#ca8a04",
  green: "#16a34a",
  teal: "#0d9488",
  blue: "#2563eb",
  purple: "#9333ea",
  pink: "#db2777",
} as const;

export function chipStyle(color?: string): CSSProperties | undefined {
  if (!color) return undefined;
  const value = chipColors[color as keyof typeof chipColors] ?? color;
  return { "--chip-color": value } as CSSProperties;
}

/** Host of an external http(s) link, without `www.`; undefined for internal links. */
export function chipHost(href?: string): string | undefined {
  if (!href) return undefined;
  try {
    const url = new URL(href);
    if (url.protocol !== "http:" && url.protocol !== "https:") return undefined;
    return url.hostname.replace(/^www\./, "");
  } catch {
    return undefined;
  }
}

export function faviconUrl(host: string): string {
  return `https://icons.duckduckgo.com/ip3/${host}.ico`;
}

/** Explicit icon first, otherwise the link's favicon. */
export function chipIcon(href?: string, icon?: string): string | undefined {
  if (icon) return icon;
  const host = chipHost(href);
  return host ? faviconUrl(host) : undefined;
}
