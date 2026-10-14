vec4 effect(vec2 uv) {
  vec4 c = getColor(uv);
  vec3 sc = fxPerc(fxUnpremul(c));
  float n = float(levels) - 1.0;
  vec3 q = floor(clamp(sc, 0.0, 1.0) * n + 0.5) / n;
  return vec4(fxLin(mix(sc, q, amount)) * c.a, c.a);
}
