vec4 effect(vec2 uv) {
  vec2 asp = vec2(ratio, 1.0);
  vec2 p = (uv - 0.5) * asp;
  p *= (1.0 + amount * dot(p, p)) / zoom;
  vec2 q = p / asp + 0.5;
  if (q.x < 0.0 || q.x > 1.0 || q.y < 0.0 || q.y > 1.0) return vec4(0.0);
  return getColor(q);
}
