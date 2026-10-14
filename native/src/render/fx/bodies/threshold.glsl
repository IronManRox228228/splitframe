vec4 effect(vec2 uv) {
  vec4 c = getColor(uv);
  vec3 sc = fxUnpremul(c);
  float l = sqrt(clamp(fxLuma(sc), 0.0, 1.0));
  float t = softness < 0.001 ? step(level, l) : smoothstep(level - softness, level + softness, l);
  return vec4(mix(sc, vec3(t), amount) * c.a, c.a);
}
