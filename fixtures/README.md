# Fixtures

DesignIR fixtures consumed by the embed smoke tests.

## `synthetic/`
A tiny hand-authored `design.ir.json` — one frame, one rounded rectangle, one
text node, no assets. Isolates the embed lifecycle (parse → materialize → attach
→ render) from importer/asset variability. Used by M1.1–M1.3.

## `figma-vst-style/`
The real "VST Style" plugin frame (1000×600) from the test Figma file
`KCKIyZoWXjde6qVNCm4qPa`, node `3:42`. Used by M1.4 (real-design render) and
M1.8 (parameter bridge).

Parameter-bridge note: this design carries **no** `pulpParamKey` binding
metadata and **no** meter widgets — it's a visual export of 15 knobs. The embed
therefore derives one parameter per bindable control, keyed by the control's
widget id (e.g. `Knob_Small47`). M1.8 enumerates those 15 params and exercises
the bidirectional flow against them. When a design DOES carry `pulpParamKey`
metadata (or meter bindings), those keys take over as the param identity and
meters route through the AudioBridge — but that path is unexercised by this
fixture.

Regenerate:

```bash
# 1. Export the frame to a figma-plugin-export-v1 envelope (needs a Figma PAT
#    with file_content:read in $FIGMA_TOKEN or ~/.config/pulp/figma-token).
python3 <pulp>/tools/import-design/figma_rest_export.py \
  --url 'https://www.figma.com/design/KCKIyZoWXjde6qVNCm4qPa/Untitled?node-id=3-42' \
  --out figma-vst-style/scene.pulp.json

# 2. Convert the envelope to DesignIR JSON (baked lane).
<pulp>/build/tools/cli/pulp-cpp import-design --from figma-plugin \
  --file figma-vst-style/scene.pulp.json \
  --emit ir-json --mode baked --output figma-vst-style/design.ir.json --no-tokens
# Expect: 16 root children, 32 assets.
```

Notes:
- `node-id=3-42` in the URL is Figma node `3:42`; the frame is named "VST Style".
- The baked ir-json lane requires the importer fix in the seam branch
  (`fix(import-design): keep figma-plugin tree + assets on the baked emit lane`).
  Older `pulp` builds drop the tree/assets on this lane.
- `scene.pulp.json` + `assets/` are the raw export; `design.ir.json` is the
  embed SDK's actual input.
