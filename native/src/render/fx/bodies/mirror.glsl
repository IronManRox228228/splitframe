vec4 effect(vec2 uv) {
  vec2 p = uv;
  if (axis == 0 || axis == 2) {
    float m = 0.5 + offset;
    p.x = flip ? (p.x < m ? 2.0 * m - p.x : p.x) : (p.x > m ? 2.0 * m - p.x : p.x);
  }
  if (axis == 1 || axis == 2) {
    float m = 0.5 + offset;
    p.y = flip ? (p.y > m ? 2.0 * m - p.y : p.y) : (p.y < m ? 2.0 * m - p.y : p.y);
  }
  if (axis == 3) {
    vec2 asp = vec2(ratio, 1.0);
    vec2 q = (uv - 0.5) * asp;
    float seg = 6.2831853 / float(segments);
    float a = mod(atan(q.y, q.x) + offset * 6.2831853, seg);
    a = abs(a - seg * 0.5);
    p = length(q) * vec2(cos(a), sin(a)) / asp + 0.5;
  }
  return getColor(p);
}
