#version 440

// One layer into the RGBA16F canvas. Source pixels -> scene-linear working space -> premultiplied.
// The transfer functions have CPU twins in color.cpp (tst_color checks them against this shader).

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
  vec4 params; // x opacity, y mode (0 RGBA straight alpha, 1 RGBA premultiplied, 2 NV12/P010), z transfer id, w canvas encoding
  vec4 flags;  // x bits (1 LUT, 2 HDR tone map, 4 effects), y LUT coord scale, z LUT coord offset
  vec4 primR;
  vec4 primG;
  vec4 primB;
  vec4 toDispR;
  vec4 toDispG;
  vec4 toDispB;
  vec4 fromDispR;
  vec4 fromDispG;
  vec4 fromDispB;
} u;

layout(binding = 1) uniform sampler2D tex0; // RGBA, or the Y plane (R8 / R16)
layout(binding = 2) uniform sampler2D tex1; // the interleaved UV plane (RG8 / RG16) for mode 2
layout(binding = 3) uniform sampler3D lut;  // input transform baked from OpenColorIO (flag 1)

vec3 rows(vec4 r0, vec4 r1, vec4 r2, vec3 v) { return vec3(dot(r0.xyz, v), dot(r1.xyz, v), dot(r2.xyz, v)); }

vec3 srgbDecode(vec3 v) {
  v = max(v, vec3(0.0));
  return mix(pow((v + 0.055) / 1.055, vec3(2.4)), v / 12.92, step(v, vec3(0.04045)));
}
vec3 srgbEncode(vec3 l) {
  l = max(l, vec3(0.0));
  return mix(1.055 * pow(l, vec3(1.0 / 2.4)) - 0.055, l * 12.92, step(l, vec3(0.0031308)));
}

vec3 pqDecode(vec3 v) {
  vec3 p = pow(clamp(v, 0.0, 1.0), vec3(1.0 / 78.84375));
  vec3 num = max(p - 0.8359375, vec3(0.0));
  vec3 den = 18.8515625 - 18.6875 * p;
  return pow(num / den, vec3(1.0 / 0.1593017578125)) * (10000.0 / 203.0);
}

vec3 hlgDecode(vec3 v) {
  v = max(v, vec3(0.0));
  vec3 lo = v * v / 3.0;
  vec3 hi = (exp((v - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
  vec3 e = mix(hi, lo, step(v, vec3(0.5)));
  float ys = dot(e, vec3(0.2627, 0.6780, 0.0593));
  return ys > 0.0 ? e * (pow(ys, 0.2) * (1000.0 / 203.0)) : vec3(0.0);
}

// 0 sRGB, 1 BT.709 OETF, 2 BT.1886, 3 gamma 2.2, 4 PQ, 5 HLG, 6 linear, 7 gamma 2.8
vec3 decodeTransfer(int id, vec3 v) {
  if (id == 0) return srgbDecode(v);
  if (id == 4) return pqDecode(v);
  if (id == 5) return hlgDecode(v);
  if (id == 6) return v;
  v = max(v, vec3(0.0));
  if (id == 1) return mix(pow((v + 0.099) / 1.099, vec3(1.0 / 0.45)), v / 4.5, step(v, vec3(0.081)));
  if (id == 2) return pow(v, vec3(2.4));
  if (id == 3) return pow(v, vec3(2.2));
  return pow(v, vec3(2.8));
}

// HDR highlight roll-off for SDR output: identity up to the knee, asymptotic to 1 above it
vec3 toneMap(vec3 l) {
  const float knee = 0.75;
  float m = max(max(l.r, l.g), l.b);
  if (m <= knee) return l;
  float range = 1.0 - knee;
  float mapped = knee + range * (1.0 - exp(-(m - knee) / range));
  return l * (mapped / m);
}

void main() {
  int mode = int(u.params.y + 0.5);
  int bits = int(u.flags.x + 0.5);
  vec4 c;
  if (mode == 2) {
    vec3 s = vec3(texture(tex0, uv).r, texture(tex1, uv).rg);
    c = vec4(clamp(vec3(dot(u.yuvR.xyz, s) + u.yuvR.w, dot(u.yuvG.xyz, s) + u.yuvG.w, dot(u.yuvB.xyz, s) + u.yuvB.w), 0.0, 1.0), 1.0);
  } else {
    c = texture(tex0, uv);
    // premultiplied rasters (text, shapes) carry sRGB-encoded colour: back to straight to linearise
    if (mode == 1) c.rgb = c.a > 0.0 ? c.rgb / c.a : vec3(0.0);
  }

  vec3 lin;
  if ((bits & 1) != 0) {
    lin = textureLod(lut, clamp(c.rgb, 0.0, 1.0) * u.flags.y + vec3(u.flags.z), 0.0).rgb;
  } else {
    lin = rows(u.primR, u.primG, u.primB, decodeTransfer(int(u.params.z + 0.5), c.rgb));
    if ((bits & 2) != 0) lin = toneMap(lin);
  }
  lin = max(lin, vec3(0.0));

  if ((bits & 4) != 0) {
    // colour effects are CSS-filter style maths on display-referred (sRGB-encoded) colour, as in the reference
    vec3 e = srgbEncode(clamp(rows(u.toDispR, u.toDispG, u.toDispB, lin), 0.0, 1.0));
    e = clamp(vec3(dot(u.fxR.xyz, e) + u.fxR.w, dot(u.fxG.xyz, e) + u.fxG.w, dot(u.fxB.xyz, e) + u.fxB.w), 0.0, 1.0);
    lin = rows(u.fromDispR, u.fromDispG, u.fromDispB, srgbDecode(e));
  }

  vec3 outc = lin;
  // display blending (matching the Electron canvas): the canvas holds sRGB-encoded colour
  if (u.params.w > 0.5) outc = srgbEncode(clamp(rows(u.toDispR, u.toDispG, u.toDispB, lin), 0.0, 1.0));
  float a = c.a * u.params.x;
  fragColor = vec4(outc * a, a);
}
