#version 440

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
  vec4 params;
} u;

layout(binding = 1) uniform sampler2D tex0; // RGBA, or the Y plane (R8 / R16)
layout(binding = 2) uniform sampler2D tex1; // the interleaved UV plane (RG8 / RG16) for mode 2

void main() {
  float opacity = u.params.x;
  float mode = u.params.y;
  vec4 c;
  if (mode > 1.5) {
    vec3 s = vec3(texture(tex0, uv).r, texture(tex1, uv).rg);
    c = vec4(dot(u.yuvR.xyz, s) + u.yuvR.w, dot(u.yuvG.xyz, s) + u.yuvG.w, dot(u.yuvB.xyz, s) + u.yuvB.w, 1.0);
    c.rgb = clamp(c.rgb, 0.0, 1.0);
  } else {
    c = texture(tex0, uv);
  }
  if (mode < 1.5) {
    // straight alpha in, premultiplied out; effects (CSS-filter style) act on the straight colour
    if (mode < 0.5) c.rgb = clamp(vec3(dot(u.fxR.xyz, c.rgb) + u.fxR.w, dot(u.fxG.xyz, c.rgb) + u.fxG.w, dot(u.fxB.xyz, c.rgb) + u.fxB.w), 0.0, 1.0) * c.a;
  } else {
    c.rgb = clamp(vec3(dot(u.fxR.xyz, c.rgb) + u.fxR.w, dot(u.fxG.xyz, c.rgb) + u.fxG.w, dot(u.fxB.xyz, c.rgb) + u.fxB.w), 0.0, 1.0);
  }
  fragColor = c * opacity;
}
