#version 440

// The canvas (RGBA16F, linear working space, or sRGB-encoded for display blending) -> display / delivery
// encoding. Preview draws this into the window, offscreen rendering into an RGBA8 / RGB10A2 texture.

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform U {
  mat4 mvp;
  vec4 uvx;
  vec4 uvy;
  vec4 yuvR;
  vec4 yuvG;
  vec4 yuvB;
  vec4 fxR;
  vec4 fxG;
  vec4 fxB;
  vec4 params; // z output transfer id, w canvas encoding (0 linear, 1 sRGB-encoded)
  vec4 flags;  // x bits (1 display LUT), y LUT coord scale, z LUT coord offset
  vec4 primR;  // working space -> output primaries
  vec4 primG;
  vec4 primB;
  vec4 toDispR;
  vec4 toDispG;
  vec4 toDispB;
  vec4 fromDispR; // Rec.709 -> working space (to undo an sRGB-encoded canvas)
  vec4 fromDispG;
  vec4 fromDispB;
} u;

layout(binding = 1) uniform sampler2D tex0; // the canvas
layout(binding = 3) uniform sampler3D lut;  // display transform baked from OpenColorIO, addressed through the log shaper

vec3 rows(vec4 r0, vec4 r1, vec4 r2, vec3 v) { return vec3(dot(r0.xyz, v), dot(r1.xyz, v), dot(r2.xyz, v)); }

vec3 srgbDecode(vec3 v) {
  v = max(v, vec3(0.0));
  return mix(pow((v + 0.055) / 1.055, vec3(2.4)), v / 12.92, step(v, vec3(0.04045)));
}
vec3 srgbEncode(vec3 l) {
  l = max(l, vec3(0.0));
  return mix(1.055 * pow(l, vec3(1.0 / 2.4)) - 0.055, l * 12.92, step(l, vec3(0.0031308)));
}

vec3 pqEncode(vec3 l) {
  vec3 y = pow(clamp(l * (203.0 / 10000.0), 0.0, 1.0), vec3(0.1593017578125));
  return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3(78.84375));
}

vec3 hlgEncode(vec3 l) {
  vec3 fd = max(l, vec3(0.0)) * (203.0 / 1000.0);
  float ys = dot(fd, vec3(0.2627, 0.6780, 0.0593));
  vec3 e = ys > 0.0 ? fd * pow(ys, -1.0 / 6.0) : vec3(0.0);
  vec3 lo = sqrt(3.0 * e);
  vec3 hi = 0.17883277 * log(max(12.0 * e - 0.28466892, vec3(1e-6))) + 0.55991073;
  return mix(hi, lo, step(e, vec3(1.0 / 12.0)));
}

// 0 sRGB, 1 BT.709 OETF, 2 BT.1886, 3 gamma 2.2, 4 PQ, 5 HLG, 6 linear, 7 gamma 2.8
vec3 encodeTransfer(int id, vec3 l) {
  if (id == 0) return srgbEncode(l);
  if (id == 4) return pqEncode(l);
  if (id == 5) return hlgEncode(l);
  l = max(l, vec3(0.0));
  if (id == 6) return l;
  if (id == 1) return mix(1.099 * pow(l, vec3(0.45)) - 0.099, l * 4.5, step(l, vec3(0.018)));
  if (id == 2) return pow(l, vec3(1.0 / 2.4));
  if (id == 3) return pow(l, vec3(1.0 / 2.2));
  return pow(l, vec3(1.0 / 2.8));
}

void main() {
  int bits = int(u.flags.x + 0.5);
  vec3 rgb = texture(tex0, uv).rgb; // the canvas is opaque: the project background is drawn first
  if (u.params.w > 0.5) rgb = rows(u.fromDispR, u.fromDispG, u.fromDispB, srgbDecode(clamp(rgb, 0.0, 1.0)));

  vec3 outc;
  if ((bits & 1) != 0) {
    // linear light -> log shaper coordinate (16 stops from 2^-10): same constants as ColorManager::shaperEncode
    vec3 s = clamp((log2(max(rgb, vec3(0.0)) + 1.0 / 1024.0) + 10.0) / 16.0, 0.0, 1.0);
    outc = textureLod(lut, s * u.flags.y + vec3(u.flags.z), 0.0).rgb;
  } else {
    outc = encodeTransfer(int(u.params.z + 0.5), rows(u.primR, u.primG, u.primB, rgb));
  }
  fragColor = vec4(clamp(outc, 0.0, 1.0), 1.0);
}
