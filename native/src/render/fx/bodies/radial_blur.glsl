vec4 effect(vec2 uv) {
  vec2 d = (centre - uv) * amount * 0.5;
  vec4 acc = vec4(0.0);
  for (int i = 0; i < 24; ++i) acc += getColor(uv + d * (float(i) / 23.0 - 0.5));
  return acc / 24.0;
}
