#!/usr/bin/env python3
"""Generate effect fragment shaders from body files (native/src/render/fx).

An effect is a registry entry (fx/registry/*.json: id, kind, params, ...) plus a body file that defines
`vec4 effect(vec2 uv)` (filter) or `vec4 transition(vec2 uv)` (transition, gl-transitions style). This script wraps
the body in the matching template (fx/templates/{filter,transition}.frag.tmpl): common uniform block, getColor /
getFromColor / getToColor, progress / time / ratio / resolution, and one `#define` per param that reads its vec4 slot
(slot = position in the params list, see render/effects.h). The output goes to fx/shaders/<id>.frag, which
CMake bakes with qt_add_shaders (the list is globbed: a new registry entry needs no CMake or C++ edit).

  python native/tools/fxgen.py                 regenerate every shader from the registry
  python native/tools/fxgen.py --check         exit 1 if a generated shader is missing or stale (no writes)
  python native/tools/fxgen.py --effect wipe   only this id

One-off, without a registry entry (for example a batch port of gl-transitions GLSL, whose `uniform` lines are
dropped because the params become macros):

  python native/tools/fxgen.py --body my.glsl --kind transition --params params.json --out my.frag

where params.json is the registry's "params" list: [{"name": "smoothness", "type": "float", "default": 0.5}, ...].
"""

import argparse
import json
import re
import sys
from pathlib import Path

FX = Path(__file__).resolve().parent.parent / "src" / "render" / "fx"
MAX_PARAMS = 16
RESERVED = {"progress", "time", "ratio", "resolution", "fxPass", "fxPasses", "fxSrc", "fxSrc2", "fxTex0", "fxUv", "fxTiming", "fxRes", "fxP", "u",
            "getColor", "getOriginal", "getTexture", "fxHash", "fxNoise", "fxLuma", "fxUnpremul", "fxPerc", "fxLin", "getFromColor", "getToColor", "effect", "transition", "main", "fragColor"}
IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
READ = {
    "float": "(u.fxP[{i}].x)",
    "int": "(int(u.fxP[{i}].x + 0.5))",
    "enum": "(int(u.fxP[{i}].x + 0.5))",
    "bool": "(u.fxP[{i}].x > 0.5)",
    "vec2": "(u.fxP[{i}].xy)",
    "point": "(u.fxP[{i}].xy)",
    "color": "(u.fxP[{i}])",
}
GLSL_TYPE = {"float": "float", "int": "int", "enum": "int", "bool": "bool", "vec2": "vec2", "point": "vec2", "color": "vec4"}
# `uniform float x; // = 1.0` as written in gl-transitions; the params replace them
UNIFORM_LINE = re.compile(r"^\s*uniform\s+[A-Za-z0-9_]+\s+[A-Za-z0-9_]+\s*(\[[^\]]*\])?\s*;.*$", re.M)
PRECISION_LINE = re.compile(r"^\s*precision\s+\w+\s+\w+\s*;.*$", re.M)


def params_block(params):
    if len(params) > MAX_PARAMS:
        raise SystemExit(f"more than {MAX_PARAMS} params")
    seen = set()
    lines = []
    for i, p in enumerate(params):
        name, typ = p.get("name", ""), p.get("type", "")
        if not IDENT.match(name) or name in RESERVED or name in seen:
            raise SystemExit(f"bad or reserved or duplicate param name: {name!r}")
        if typ == "texture":
            seen.add(name)
            lines.append(f"// slot {i}: texture {name} (sampled via getTexture(uv))")
            continue
        if typ not in READ:
            raise SystemExit(f"param {name}: unknown type {typ!r}")
        seen.add(name)
        lines.append(f"#define {name} {READ[typ].format(i=i)} // slot {i}, {GLSL_TYPE[typ]}")
    return "\n".join(lines) + ("\n" if lines else "")


def render(effect_id, kind, params, body_text):
    template = (FX / "templates" / ("filter.frag.tmpl" if kind == "filter" else "transition.frag.tmpl")).read_text(encoding="utf-8")
    body = PRECISION_LINE.sub("", UNIFORM_LINE.sub("", body_text)).strip("\n") + "\n"
    needed = "effect" if kind == "filter" else "transition"
    if not re.search(rf"\bvec4\s+{needed}\s*\(\s*vec2\s+\w+\s*\)", body):
        raise SystemExit(f"{effect_id}: body must define `vec4 {needed}(vec2 uv)`")
    out = template.replace("@ID@", effect_id).replace("@PARAMS@", params_block(params)).replace("@BODY@", body)
    return out.replace("\r\n", "\n")


def registry_entries():
    for f in sorted((FX / "registry").glob("*.json")):
        for e in json.loads(f.read_text(encoding="utf-8")).get("effects", []):
            yield f, e


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--effect", help="only this effect id")
    ap.add_argument("--check", action="store_true", help="verify instead of writing")
    ap.add_argument("--body"), ap.add_argument("--kind", choices=["filter", "transition"]), ap.add_argument("--params"), ap.add_argument("--out")
    a = ap.parse_args()

    if a.body:
        if not (a.kind and a.out):
            raise SystemExit("--body needs --kind and --out")
        params = json.loads(Path(a.params).read_text(encoding="utf-8")) if a.params else []
        Path(a.out).write_text(render(Path(a.body).stem, a.kind, params, Path(a.body).read_text(encoding="utf-8")), encoding="utf-8", newline="\n")
        return 0

    stale = 0
    count = 0
    for f, e in registry_entries():
        if a.effect and e["id"] != a.effect:
            continue
        body_path = FX / e.get("body", f"bodies/{e['id']}.glsl")
        text = render(e["id"], e["kind"], e.get("params", []), body_path.read_text(encoding="utf-8"))
        target = FX / "shaders" / f"{e['id']}.frag"
        count += 1
        if a.check:
            if not target.exists() or target.read_text(encoding="utf-8").replace("\r\n", "\n") != text:
                print(f"stale: {target.name}")
                stale += 1
        else:
            target.write_text(text, encoding="utf-8", newline="\n")
    print(f"{count} effect(s) {'checked' if a.check else 'generated'}")
    return 1 if stale else 0


if __name__ == "__main__":
    sys.exit(main())
