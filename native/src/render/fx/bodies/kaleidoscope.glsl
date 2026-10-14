vec4 effect(vec2 uv) {
  vec2 asp = vec2(ratio, 1.0);
  vec2 q = (uv - centre) * asp;
  float seg = 6.2831853 / float(segments);
  float a = mod(atan(q.y, q.x) - radians(rotation), seg);
  a = abs(a - seg * 0.5);
  vec2 p = length(q) / zoom * vec2(cos(a), sin(a)) / asp + centre;
  return mix(getColor(uv), getColor(p), amount);
}
