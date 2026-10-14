vec4 effect(vec2 uv) {
  vec2 cell = vec2(max(size, 1.0)) / resolution;
  return getColor((floor(uv / cell) + 0.5) * cell);
}
