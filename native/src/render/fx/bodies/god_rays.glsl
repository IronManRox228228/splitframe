vec4 effect(vec2 uv) {
  vec4 c = getColor(uv);
  vec2 stepv = (position - uv) * reach / 32.0;
  vec2 p = uv;
  vec3 acc = vec3(0.0);
  float w = 1.0, tot = 0.0;
  for (int i = 0; i < 32; ++i) {
    p += stepv;
    vec3 s = getColor(p).rgb;
    acc += s * smoothstep(threshold, threshold + 0.3, fxLuma(s)) * w;
    tot += w;
    w *= 0.95;
  }
  vec3 rays = acc / tot * tint.rgb * intensity;
  return vec4(c.rgb + rays, max(c.a, min(1.0, fxLuma(rays))));
}
