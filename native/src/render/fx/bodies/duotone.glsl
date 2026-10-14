vec4 effect(vec2 uv) {
  vec4 c = getColor(uv);
  vec3 sc = fxUnpremul(c);
  float l = sqrt(clamp(fxLuma(sc), 0.0, 1.0));
  vec3 d = mix(shadow.rgb, highlight.rgb, l);
  return vec4(mix(sc, d, amount) * c.a, c.a);
}
