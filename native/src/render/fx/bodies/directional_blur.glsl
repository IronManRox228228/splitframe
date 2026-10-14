vec4 effect(vec2 uv) {
  vec2 dir = vec2(cos(radians(angle)), sin(radians(angle))) * reach * vec2(1.0 / ratio, 1.0);
  vec4 acc = vec4(0.0);
  for (int i = 0; i < 25; ++i) acc += getColor(uv + dir * (float(i) / 24.0 - 0.5));
  return acc / 25.0;
}
