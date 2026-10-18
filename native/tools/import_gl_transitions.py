#!/usr/bin/env python3
"""Import gl-transitions (https://github.com/gl-transitions/gl-transitions) into the effects framework.

  python native/tools/import_gl_transitions.py [--src C:/dev/gl-transitions] [--qsb C:/Qt/6.8.3/msvc2022_64/bin/qsb.exe]

For every transitions/*.glsl it
  * parses the header comments (Author / License / Description / Tags) and the `uniform` lines (type, `// = default`);
  * rewrites the GLSL where the framework needs it (see RULES / OVERRIDES below);
  * writes the body to src/render/fx/bodies/gl_<name>.glsl (header comment kept on top),
  * adds an entry to src/render/fx/registry/gl_transitions.json (category "transition"),
  * writes src/render/fx/THIRD_PARTY_GL_TRANSITIONS.md (author + license of every transition, license texts),
  * writes the generated shader src/render/fx/shaders/gl_<name>.frag through fxgen.
With --qsb every generated shader is also compiled once (SPIR-V + HLSL 50 + MSL, like CMake) so a bad port is
reported here instead of at build time; transitions that fail are skipped and listed.

Param mapping (the framework's param types are float, int, enum, bool, vec2, point, color):
  float, int, bool           as they are (min/max guessed from the default, see float_range)
  vec2 named *center*        point (top-left origin in the UI; the default is flipped from gl-transitions' y-up)
  other vec2                 vec2
  vec3 / vec4 named *colo(u)r* / *clr*   colour param; for vec3 the body's uses get `.rgb`
  ivec2 / other vec4         exploded into scalar params `<name>X` / `<name>Y` (int) or `<name>_x .. _w` (float),
                             and a `#define <name> ivec2(..)` / `vec4(..)` at the top of the body
  sampler2D                  not supported (no extra texture binding): the transition is skipped
"""

import argparse
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FX = ROOT / "src" / "render" / "fx"
sys.path.insert(0, str(Path(__file__).resolve().parent))
import fxgen  # noqa: E402

PREFIX = "gl_"

# file stem -> (id without prefix, display name) where the generic snake_case / Title rules read badly
NAMES = {
    "HSVfade": ("hsv_fade", "HSV Fade"),
    "TVStatic": ("tv_static", "TV Static"),
    "ZoomRigthWipe": ("zoom_right_wipe", "Zoom Right Wipe"),
    "randomNoisex": ("random_noise_x", "Random Noise X"),
    "burn0": ("burn_0", "Burn 0"),
    "x_axis_translation": ("x_axis_translation", "X Axis Translation"),
    "old_tv_lost_signal": ("old_tv_lost_signal", "Old TV Lost Signal"),
}

# transitions that cannot be ported, with the reason
SKIP = {}

# name fragment -> tag; the header's own `// Tags:` line is added too
NAME_TAGS = [
    ("wipe", "wipe"), ("slide", "wipe"), ("open", "wipe"), ("close", "wipe"), ("left", "wipe"), ("right", "wipe"),
    ("zoom", "zoom"), ("scale", "zoom"), ("glitch", "glitch"), ("static", "glitch"), ("datamosh", "glitch"), ("noise", "glitch"),
    ("tv", "glitch"), ("flicker", "glitch"), ("circle", "geometric"), ("square", "geometric"), ("rect", "geometric"),
    ("box", "geometric"), ("grid", "geometric"), ("hexagon", "geometric"), ("star", "geometric"), ("heart", "geometric"),
    ("chess", "geometric"), ("bowtie", "geometric"), ("polka", "geometric"), ("tiles", "geometric"), ("pinwheel", "geometric"),
    ("radial", "geometric"), ("angular", "geometric"), ("mosaic", "geometric"), ("pixel", "geometric"),
    ("perlin", "organic"), ("water", "organic"), ("ripple", "organic"), ("wind", "organic"), ("dreamy", "organic"),
    ("morph", "organic"), ("swirl", "organic"), ("leaf", "organic"), ("wave", "organic"), ("melt", "organic"),
    ("dissolve", "organic"), ("burn", "light"), ("film", "light"), ("exposure", "light"), ("fade", "light"),
    ("cube", "3d"), ("flip", "3d"), ("curl", "3d"), ("door", "3d"), ("swap", "3d"), ("stereo", "3d"), ("fold", "3d"),
    ("roll", "3d"), ("book", "3d"), ("bounce", "3d"),
]

# ---- RULES: mechanical rewrites applied to every body ----
RULES = [
    (re.compile(r"\btexture2D\s*\(\s*from\s*,"), "getFromColor("),
    (re.compile(r"\btexture2D\s*\(\s*to\s*,"), "getToColor("),
]

# ---- OVERRIDES: hand patches for one transition each, for what qsb / the template rejects ----
# file stem -> list of (regex, replacement). A pattern that matches nothing aborts the import (the source changed).
OVERRIDES = {
    # the local `strength` shadows the uniform it is computed from
    "CrossZoom": [
        (r"float strength = Sinusoidal_easeInOut\(0\.0, strength,", "float strengthNow = Sinusoidal_easeInOut(0.0, strength,"),
        (r"toCenter \* percent \* strength,", "toCenter * percent * strengthNow,"),
    ],
}

# float params whose default is >1 but must stay positive (divisors); others may go down to 0
DIVISORS = re.compile(r"(?i)^(pixelsize|blocksize|grid_num|mosaicnum|dots|count|zoom|size|scale|bars|steps|segments|waves|horizontalhexagons|"
                      r"horizontalbars|verticalslits|n_noise_pixels|frequency|bounces|floating|zoom_quickness)$")
COLOR_NAME = re.compile(r"(?i)colou?r|clr")
CENTER_NAME = re.compile(r"(?i)center")
INT_RANGES = {("Slides", "type"): (0, 8), ("Rolls", "type"): (0, 3), ("Box", "location"): (0, 3), ("Box", "rectIn"): (0, 1),
              ("Mosaic", "endx"): (-4, 4), ("Mosaic", "endy"): (-4, 4)}

UNIFORM = re.compile(r"^[ \t]*uniform\s+(\w+)\s+(\w+)\s*(?:/\*\s*(.*?)\s*\*/)?\s*;[ \t]*(?://\s*(.*))?$", re.M)


def snake(stem):
    if stem in NAMES:
        return NAMES[stem][0]
    s = stem.replace("-", "_")
    s = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1_\2", s)
    s = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", s)
    return re.sub(r"_+", "_", s).lower()


def title(stem):
    if stem in NAMES:
        return NAMES[stem][1]
    return " ".join(w if w.isupper() else w.capitalize() for w in snake(stem).split("_"))


def header_of(text):
    """(leading comment block, the rest): // lines, blank lines and /* */ blocks before the first code line."""
    lines = text.split("\n")
    out, i, in_block = [], 0, False
    while i < len(lines):
        s = lines[i].strip()
        if in_block:
            out.append(lines[i])
            if "*/" in s:
                in_block = False
        elif s.startswith("//") or s == "":
            out.append(lines[i])
        elif s.startswith("/*"):
            out.append(lines[i])
            in_block = "*/" not in s
        else:
            break
        i += 1
    while out and out[-1].strip() == "":
        out.pop()
    return "\n".join(out), "\n".join(lines[i:])


def annotations(rest):
    """name -> (description, (min, max) or None) from the `// @param|@color|@range(a, b[, step]) text` lines above each uniform."""
    out, lines = {}, rest.split("\n")
    for i, line in enumerate(lines):
        m = UNIFORM.match(line)
        if not m:
            continue
        desc, rng, j = None, None, i - 1
        while j >= 0 and lines[j].strip().startswith("//"):
            a = re.match(r"\s*//\s*@(param|color|range)\b(?:\(([^)]*)\))?\s*(.*)$", lines[j])
            if a:
                desc = desc or (a.group(3).strip() or None)
                if a.group(1) == "range" and a.group(2):
                    v = [float(x) for x in a.group(2).split(",")[:2]]
                    rng = (v[0], v[1])
            j -= 1
        out[m.group(2)] = (desc, rng)
    return out


def field(header, name):
    m = re.search(rf"^\s*//\s*{name}\s*:\s*(.+?)\s*$", header, re.M | re.I)
    return m.group(1).strip() if m else None


def parse_default(typ, raw):
    """Default (python value) from a comment that starts with `=`; None when absent or unparseable."""
    if not raw:
        return None
    m = re.match(r"=\s*(.*)", raw.strip())
    if not m:
        return None
    raw = m.group(1).split(";")[0].strip()
    try:
        if typ == "float":
            return float(raw)
        if typ == "int":
            return int(float(raw))
        if typ == "bool":
            return raw.lower() == "true"
        v = re.match(r"i?vec(\d)\s*\(([^)]*)\)", raw)
        if v:
            n = int(v.group(1))
            vals = [float(x) for x in v.group(2).split(",") if x.strip()]
            if len(vals) == 1:
                vals = vals * n
            return vals if len(vals) == n else None
    except ValueError:
        return None
    return None


def float_range(name, d):
    if d is None or d == 0:
        return 0.0, 1.0
    if d < 0:
        return 2 * d, -2 * d
    lo = round(d / 20.0, 6) if (d > 1 and DIVISORS.match(name)) else 0.0
    return lo, (1.0 if d <= 0.5 else 2.0 if d <= 1.0 else round(2.0 * d, 6))


def clean(x):
    return int(x) if isinstance(x, float) and x == int(x) else x


def label_of(name):
    label = re.sub(r"(?<=[a-z0-9])(?=[A-Z])|_", " ", name).strip()
    return label[:1].upper() + label[1:]


def build_params(stem, uniforms, body, notes):
    """-> (params, prelude lines, body); ValueError for unsupported types."""
    params, prelude = [], []
    for typ, name, raw1, raw2 in uniforms:
        if len(name) == 1 and name in "xyzwrgbastpq":
            # RULE: a one-letter param would become a macro that also rewrites `.z` swizzles (and the template's own `.z`)
            new = name + "Value"
            body = re.sub(rf"(?<![.\w]){name}\b", new, body)
            notes[new] = notes.get(name, (None, None))
            name = new
        d = parse_default(typ, raw1 or raw2) if (raw1 or raw2) else None
        if d is None and raw1 and raw2:
            d = parse_default(typ, raw2)
        label = label_of(name)
        desc, rng = notes.get(name, (None, None))
        n0 = len(params)
        if typ == "float":
            lo, hi = rng if rng else float_range(name, d)
            params.append({"name": name, "label": label, "type": "float", "min": clean(lo), "max": clean(hi), "default": clean(d if d is not None else (lo + hi) / 2)})
        elif typ == "int":
            dd = d or 0
            lo, hi = rng or INT_RANGES.get((stem, name), (0 if dd < 1 else 1, max(4, 3 * dd)))
            params.append({"name": name, "label": label, "type": "int", "min": int(lo), "max": int(hi), "default": dd})
        elif typ == "bool":
            params.append({"name": name, "label": label, "type": "bool", "default": bool(d)})
        elif typ == "vec2":
            d = d or [0.5, 0.5]
            if CENTER_NAME.search(name):
                params.append({"name": name, "label": label, "type": "point", "default": [clean(d[0]), clean(round(1.0 - d[1], 6))]})
            else:
                params.append({"name": name, "label": label, "type": "vec2", "min": -2, "max": 2, "default": [clean(v) for v in d]})
        elif typ in ("vec3", "vec4") and COLOR_NAME.search(name):
            d = d or [1.0, 1.0, 1.0, 1.0][: int(typ[-1])]
            if typ == "vec3":
                d = d + [1.0]
                # the macro is a vec4: `color` -> `color.rgb`, `color.r` -> `color.rgb.r`
                body = re.sub(rf"\b{name}\b(?!\s*\.)", f"{name}.rgb", body)
                body = re.sub(rf"\b{name}\b\s*\.(?!rgb\b)", f"{name}.rgb.", body)
            params.append({"name": name, "label": label, "type": "color", "default": [clean(v) for v in d]})
        elif typ == "ivec2":
            d = d or [4, 4]
            params.append({"name": f"{name}X", "label": f"{label} X", "type": "int", "min": 1, "max": max(32, 3 * int(d[0])), "default": int(d[0])})
            params.append({"name": f"{name}Y", "label": f"{label} Y", "type": "int", "min": 1, "max": max(32, 3 * int(d[1])), "default": int(d[1])})
            prelude.append(f"#define {name} ivec2({name}X, {name}Y)")
        elif typ in ("vec3", "vec4"):
            n = int(typ[-1])
            d = d or [0.0] * n
            for i, c in enumerate("xyzw"[:n]):
                params.append({"name": f"{name}_{c}", "label": f"{label} {c.upper()}", "type": "float", "min": -1, "max": 2, "default": clean(d[i])})
            prelude.append(f"#define {name} {typ}({', '.join(f'{name}_{c}' for c in 'xyzw'[:n])})")
        elif typ == "sampler2D":
            default_tex = "clouds" if (stem == "displacement" or "displacement" in name.lower()) else "linear_lr"
            params.append({"name": name, "label": label, "type": "texture", "default": default_tex})
            body = re.sub(rf"\btexture2D\s*\(\s*{re.escape(name)}\s*,", "getTexture(", body)
        else:
            raise ValueError(f"unsupported uniform type {typ} ({name})")
        if desc:
            params[n0]["description"] = desc
    return params, prelude, body


def tags_for(stem, header):
    tags = []
    for t in (field(header, "Tags") or "").split(","):
        t = t.strip().lower().replace(" ", "-")
        if t and t not in tags:
            tags.append(t)
    low = stem.lower()
    for frag, tag in NAME_TAGS:
        if frag in low and tag not in tags:
            tags.append(tag)
    return tags or ["transition"]


FUNC = re.compile(r"(?:^|\n)[ \t]*(?:const\s+)?(?:float|int|bool|vec[234]|ivec[234]|mat[234]|void)\s+\w+\s*\(([^)]*)\)\s*\{")
HLSL_RESERVED = ["signed", "unsigned", "half", "double", "line", "point", "triangle", "register", "packoffset", "texture", "sampler", "buffer",
                 "matrix", "vector", "string", "linear", "centroid", "precise", "shared", "static", "extern", "volatile", "inline", "interface",
                 "typedef", "namespace", "snorm", "unorm", "min16float", "lineadj", "triangleadj", "pass", "technique", "compile", "decl"]
TYPE = r"(?:float|int|bool|vec[234]|ivec[234]|bvec[234]|mat[234])"


def localize(rest, names):
    """RULE: a function that declares a local or a parameter named like a framework macro (progress, time, ratio,
    resolution or a param) gets it renamed to `<name>_` inside that function, so the macro does not rewrite the
    declaration."""
    out, i = [], 0
    while True:
        m = FUNC.search(rest, i)
        if not m:
            out.append(rest[i:])
            break
        depth, j = 0, m.end() - 1
        while j < len(rest):
            depth += {"{": 1, "}": -1}.get(rest[j], 0)
            j += 1
            if depth == 0:
                break
        func = rest[m.start():j]
        for n in sorted(names):
            if re.search(rf"\b{TYPE}\s+{n}\b(?!\s*\()", func):
                head, sep, tail = func.partition("{")
                func = re.sub(rf"\b{n}\b(?!\s*\()", n + "_", head) + sep + re.sub(rf"\b{n}\b(?!\s*\()", n + "_", tail)
        out.append(rest[i:m.start()])
        out.append(func)
        i = j
    return "".join(out)


def import_one(src_file):
    stem = src_file.stem
    if stem in SKIP:
        raise ValueError(SKIP[stem])
    text = src_file.read_text(encoding="utf-8").replace("\r\n", "\n")
    header, rest = header_of(text)
    uniforms = [(m.group(1), m.group(2), m.group(3), m.group(4)) for m in UNIFORM.finditer(rest)]
    notes = annotations(text)
    rest = UNIFORM.sub("", rest)
    params, prelude, rest = build_params(stem, uniforms, rest, notes)
    for rx, rep in RULES:
        rest = rx.sub(rep, rest)
    for pattern, rep in OVERRIDES.get(stem, []):
        rest, n = re.subn(pattern, rep, rest, flags=re.S)
        if n == 0:
            raise SystemExit(f"{stem}: override {pattern!r} matched nothing (the source changed?)")
    macros = {"progress", "time", "ratio", "resolution"} | {p["name"] for p in params} | {m.group(1) for m in re.finditer(r"#define (\w+) ", "\n".join(prelude))}
    rest = localize(rest, macros)
    # RULE: names that are fine in GLSL but reserved in HLSL (qsb passes them, the D3D compiler rejects them) get a `_` suffix
    for w in HLSL_RESERVED:
        if re.search(rf"\b{TYPE}\s+{w}\b", rest):
            rest = re.sub(rf"\b{w}\b", w + "_", rest)
    body = header + "\n"
    if prelude:
        body += "\n// params that are not a single float / int / bool / vec2 / colour become scalars, re-assembled here\n" + "\n".join(prelude) + "\n"
    body += "\n" + rest.strip("\n") + "\n"
    pid = PREFIX + snake(stem)
    entry = {
        "id": pid,
        "name": title(stem),
        "category": "transition",
        "kind": "transition",
        "body": f"bodies/{pid}.glsl",
        "description": field(header, "Description") or title(stem),
        "tags": tags_for(stem, header),
        "whenToUse": [f"{title(stem).lower()} style change of scene", "a stylised transition from the gl-transitions collection"],
        "params": params,
    }
    info = {"stem": stem, "id": pid, "author": field(header, "Author") or "unknown (see header)",
            "license": field(header, "License") or "MIT (repository default)"}
    return entry, body, info


def compile_check(qsb, frag):
    with tempfile.TemporaryDirectory() as d:
        r = subprocess.run([qsb, "--glsl", "330", "--hlsl", "50", "--msl", "12", "-o", str(Path(d) / "o.qsb"), str(frag)], capture_output=True, text=True)
    return None if r.returncode == 0 else (r.stderr or r.stdout).strip()


BSD2_TEXT = """Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."""


def write_third_party(src, infos, skipped):
    lines = [
        "# Third-party shaders: gl-transitions",
        "",
        "The `gl_*` transitions (bodies in `bodies/gl_*.glsl`, generated shaders in `shaders/gl_*.frag`, registry in",
        "`registry/gl_transitions.json`) are ports of https://github.com/gl-transitions/gl-transitions. Each body keeps its",
        "author / license header. Generated by `tools/import_gl_transitions.py`; do not edit by hand.",
        "",
        "## Transitions",
        "",
        "| Effect id | Original file | Author | License |",
        "|---|---|---|---|",
    ]
    for i in sorted(infos, key=lambda x: x["id"]):
        lines.append(f"| `{i['id']}` | {i['stem']}.glsl | {i['author']} | {i['license']} |")
    if skipped:
        lines += ["", "## Not ported", ""]
        lines += [f"- {stem}.glsl: {why}" for stem, why in sorted(skipped.items())]
    mit = (src / "LICENSE").read_text(encoding="utf-8").strip()
    lines += ["", "## License texts", "",
              "### MIT (the gl-transitions repository; every transition whose header says MIT or names no license)", "", "```", mit, "```", ""]
    page = (src / "transitions" / "InvertedPageCurl.glsl")
    block = re.search(r"/\*(.*?)\*/", page.read_text(encoding="utf-8"), re.S) if page.exists() else None
    bsd3 = "\n".join(l for l in (block.group(1).strip().split("\n") if block else []) if not l.strip().startswith("in vec2")).strip()
    lines += ["### BSD 3-Clause (InvertedPageCurl, Hewlett-Packard)", "", "```", bsd3, "```", ""]
    stereo = src / "transitions" / "StereoViewer.glsl"
    hdr = header_of(stereo.read_text(encoding="utf-8").replace("\r\n", "\n"))[0] if stereo.exists() else ""
    lines += ["### BSD 2-Clause (StereoViewer)", "", "File header:", "", "```", hdr, "```", "", "```", BSD2_TEXT, "```", ""]
    (FX / "THIRD_PARTY_GL_TRANSITIONS.md").write_text("\n".join(lines), encoding="utf-8", newline="\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", default="C:/dev/gl-transitions")
    ap.add_argument("--qsb", help="qsb.exe: compile-check every generated shader; failing transitions are skipped")
    ap.add_argument("--only", help="only this file stem (debugging; writes no registry or notice)")
    a = ap.parse_args()
    src = Path(a.src)
    files = sorted((src / "transitions").glob("*.glsl"), key=lambda p: p.stem.lower())
    entries, infos, skipped = [], [], {}
    seen = set()
    for f in files:
        if a.only and f.stem != a.only:
            continue
        try:
            entry, body, info = import_one(f)
        except ValueError as e:
            skipped[f.stem] = str(e)
            continue
        if entry["id"] in seen:
            raise SystemExit(f"duplicate id {entry['id']}")
        seen.add(entry["id"])
        (FX / entry["body"]).write_text(body, encoding="utf-8", newline="\n")
        frag = FX / "shaders" / f"{entry['id']}.frag"
        frag.write_text(fxgen.render(entry["id"], "transition", entry["params"], body), encoding="utf-8", newline="\n")
        if a.qsb:
            err = compile_check(a.qsb, frag)
            if err:
                skipped[f.stem] = "does not compile: " + " | ".join(err.splitlines()[-3:])
                (FX / entry["body"]).unlink()
                frag.unlink()
                print(f"FAIL {f.stem}: {err[:600]}")
                continue
        entries.append(entry)
        infos.append(info)
    if not a.only:
        keep = {e["id"] for e in entries}
        for d, pat in (("shaders", PREFIX + "*.frag"), ("bodies", PREFIX + "*.glsl")):
            for f in (FX / d).glob(pat):
                if f.stem not in keep:
                    f.unlink()
        (FX / "registry" / "gl_transitions.json").write_text(json.dumps({"effects": entries}, indent=1) + "\n", encoding="utf-8", newline="\n")
        write_third_party(src, infos, skipped)
    print(f"{len(entries)} ported, {len(skipped)} skipped")
    for stem, why in sorted(skipped.items()):
        print(f"  skipped {stem}: {why[:200]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
