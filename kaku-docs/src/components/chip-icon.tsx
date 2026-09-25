"use client";

import { useEffect, useRef } from "react";

// Client-only so a broken icon can be hidden: a site without a favicon
// (example.com, say) gets a 404 from the favicon service, which would otherwise
// leave an empty gap at the start of the chip.
export function ChipIcon({ src }: { src: string }) {
  const imageRef = useRef<HTMLImageElement>(null);

  // The page is prerendered, so the image can fail before React attaches
  // onError; catch that case once hydrated.
  useEffect(() => {
    const image = imageRef.current;
    if (image?.complete && image.naturalWidth === 0) image.hidden = true;
  }, []);

  return (
    // biome-ignore lint/performance/noImgElement: a 16px favicon from another site; next/image would need every domain whitelisted
    <img
      ref={imageRef}
      className="chip-icon"
      src={src}
      alt=""
      loading="lazy"
      onError={(event) => {
        event.currentTarget.hidden = true;
      }}
    />
  );
}
