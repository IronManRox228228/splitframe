// Bloom in three passes: 0 = bright-pass + horizontal blur, 1 = vertical blur, 2 = add the blurred light back onto the original.
vec4 effect(vec2 uv) {
  if (fxPass == 2) {
    vec4 o = getOriginal(uv);
    vec3 g = getColor(uv).rgb * intensity;
    return vec4(o.rgb + g, max(o.a, min(1.0, fxLuma(g))));
  }
  float rpx = max(radius * resolution.y, 0.5);
  vec2 dir = fxPass == 0 ? vec2(1.0 / resolution.x, 0.0) : vec2(0.0, 1.0 / resolution.y);
  vec3 acc = vec3(0.0);
  float sum = 0.0;
  for (int i = -12; i <= 12; ++i) {
    float w = exp(-0.5 * float(i * i) / 36.0); // sigma = radius / 2
    vec3 s = getColor(uv + dir * (float(i) * rpx / 12.0)).rgb;
    if (fxPass == 0) s *= smoothstep(threshold, threshold + 0.3, fxLuma(s));
    acc += s * w;
    sum += w;
  }
  return vec4(acc / sum, 1.0);
}
