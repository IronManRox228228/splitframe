vec4 effect(vec2 uv) {
  if (radius < 0.25) return getColor(uv);
  vec4 acc = vec4(0.0);
  float sum = 0.0;
  for (int i = 0; i < 48; ++i) {
    float r = sqrt((float(i) + 0.5) / 48.0);
    float th = float(i) * 2.39996323;
    vec4 s = getColor(uv + vec2(cos(th), sin(th)) * r * radius / resolution);
    float w = 1.0 + bokeh * 6.0 * max(fxLuma(s.rgb) - 0.6, 0.0);
    acc += s * w;
    sum += w;
  }
  return acc / sum;
}
