vec4 effect(vec2 uv) {
  vec2 asp = vec2(ratio, 1.0);
  vec2 q = (uv - centre) * asp;
  float f = 1.0 - smoothstep(0.0, radius, length(q));
  float a = radians(angle) * f * f;
  float ca = cos(a), sa = sin(a);
  q = vec2(ca * q.x - sa * q.y, sa * q.x + ca * q.y);
  return getColor(q / asp + centre);
}
