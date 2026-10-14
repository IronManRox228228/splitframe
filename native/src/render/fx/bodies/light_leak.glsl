vec3 hsv2rgb(vec3 h) {
  vec3 k = clamp(abs(fract(h.x + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
  return h.z * mix(vec3(1.0), k, h.y);
}
vec4 effect(vec2 uv) {
  vec4 c = getColor(uv);
  float t = time * speed;
  vec2 dir = vec2(cos(radians(angle)), sin(radians(angle)));
  float edge = 0.5 + dot(uv - 0.5, -dir);          // 0 at the lit edge, 1 at the far one
  float along = dot(uv - 0.5, vec2(-dir.y, dir.x)); // position along the edge
  float n = fxNoise(vec2(along * 2.5 + t, t * 0.6));
  float pulse = 0.75 + 0.25 * sin(t * 2.1);
  float m = clamp(1.0 - edge / max(spread, 0.05), 0.0, 1.0);
  m = m * m * (0.4 + 1.2 * n) * pulse;
  vec3 hot = hsv2rgb(vec3(hue / 360.0, 0.85, 1.0));
  vec3 warm = hsv2rgb(vec3((hue + 35.0) / 360.0, 0.55, 1.0));
  vec3 leak = clamp(mix(hot, warm, n) * m * intensity * 1.6, 0.0, 1.0);
  vec3 sc = fxUnpremul(c);
  return vec4((1.0 - (1.0 - sc) * (1.0 - leak)) * c.a, c.a);
}
