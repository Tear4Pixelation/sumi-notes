# labs/

Throwaway prototypes for evaluating a design before it is specified or built. Nothing here is
compiled into Write, referenced by the Makefile, or shipped.

## color-lab.html

Playground for the themed ink palette generator. Open it directly in a browser; no build, no
dependencies, no server needed.

It models the generator that would eventually live in `ulib`:

```
for each hue family h:
    L = start at cusp(h), offset by "depth", then keep walking away from the
        paper until contrast(ink, paper) >= minContrast
    C = vividness * maxChroma(L, h)
```

The point of the algorithm is that **lightness is never chosen**. It is whatever legibility
against the paper forces, and chroma is whatever the sRGB gamut then allows at that lightness.
The resulting palette has no shared lightness and no shared chroma, which is what keeps the
relationship between its colors from being spottable.

### The knobs that matter

- **Depth below cusp** — the neon-to-ink knob. At 0 every color sits at its most saturated
  possible point and the palette is garish. The Günter Schuler "modern" palette measures about
  0.10 below.
- **Vividness** — a fraction of *each hue's own* maximum chroma, never an absolute number.
  An absolute chroma is dull for magenta and out of gamut for cyan.
- **Min contrast vs paper** — the legibility floor. At the cusp, yellow is 1.1:1 on white and
  literally invisible; this is what pulls it down to a usable color.
- **Force shared envelope** — the comparison case. Turns on the Tailwind/Material approach of
  one lightness for everything with chroma capped to the weakest hue, to show what it costs.

### Presets

`Schuler-like`, `Calm`, `Muted / earthy`, `Neon (bad)`, `Dark paper`, `Strict 4.5:1` — start
from these rather than from the sliders.
