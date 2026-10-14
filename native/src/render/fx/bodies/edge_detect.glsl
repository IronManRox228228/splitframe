float lumAt(vec2 p) { return fxLuma(fxPerc(fxUnpremul(getColor(p)))); }

float sobel(vec2 uv, vec2 s) {
  float a = lumAt(uv + s * vec2(-1.0, 1.0)), b = lumAt(uv + s * vec2(0.0, 1.0)), c = lumAt(uv + s * vec2(1.0, 1.0));
  float d = lumAt(uv + s * vec2(-1.0, 0.0)), f = lumAt(uv + s * vec2(1.0, 0.0));
  float g = lumAt(uv + s * vec2(-1.0, -1.0)), h = lumAt(uv + s * vec2(0.0, -1.0)), i = lumAt(uv + s * vec2(1.0, -1.0));
  float gx = (c + 2.0 * f + i) - (a + 2.0 * d + g);
  float gy = (g + 2.0 * h + i) - (a + 2.0 * b + c);
  return length(vec2(gx, gy));
}

vec4 effect(vec2 uv) {
  vec4 c = getColor(uv);
  vec2 s = width / resolution;
  float e = sobel(uv, s) * strength;
  if (glow > 0.0) e += (sobel(uv, s * 4.0) * 0.5 + sobel(uv, s * 8.0) * 0.35) * glow * strength;
  float m = clamp(e, 0.0, 1.0);
  return vec4(mix(c.rgb, tint.rgb * m * c.a, amount), c.a);
}
