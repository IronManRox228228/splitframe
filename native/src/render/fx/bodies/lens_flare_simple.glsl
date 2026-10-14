vec4 effect(vec2 uv) {
  vec4 c = getColor(uv);
  vec2 asp = vec2(ratio, 1.0);
  vec2 p = (uv - position) * asp;
  float sz = max(size, 0.01) * 0.25;
  float len = length(p);
  float flare = exp(-len * len / (0.05 * sz * sz)) + 0.25 * exp(-len / (sz * 2.0));
  flare += streak * exp(-abs(p.y) / (sz * 0.08)) * exp(-abs(p.x) / (sz * 3.0)) * 0.6;
  vec2 toCentre = (vec2(0.5) - position) * asp;
  for (int k = 0; k < 3; ++k) {
    float f = k == 0 ? 0.35 : (k == 1 ? 0.8 : 1.5);
    float r = sz * (0.45 + 0.25 * float(k));
    float d = length(p - toCentre * f);
    flare += 0.3 * exp(-pow((d - r) / (r * 0.3), 2.0)) + 0.12 * exp(-d * d / (r * r));
  }
  vec3 add = tint.rgb * flare * intensity;
  return vec4(c.rgb + add, max(c.a, min(1.0, fxLuma(add))));
}
