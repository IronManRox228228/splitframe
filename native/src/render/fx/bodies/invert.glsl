vec4 effect(vec2 uv) {
  vec4 c = getColor(uv);
  vec3 sc = clamp(fxPerc(fxUnpremul(c)), 0.0, 1.0);
  return vec4(fxLin(mix(sc, 1.0 - sc, amount)) * c.a, c.a);
}
