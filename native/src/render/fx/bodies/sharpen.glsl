vec4 effect(vec2 uv) {
  vec2 s = radius / resolution;
  vec4 c = getColor(uv);
  vec4 b = c * 4.0
         + (getColor(uv + vec2(s.x, 0.0)) + getColor(uv - vec2(s.x, 0.0)) + getColor(uv + vec2(0.0, s.y)) + getColor(uv - vec2(0.0, s.y))) * 2.0
         + getColor(uv + s) + getColor(uv - s) + getColor(uv + vec2(s.x, -s.y)) + getColor(uv + vec2(-s.x, s.y));
  b /= 16.0;
  return vec4(max(c.rgb + (c.rgb - b.rgb) * amount, vec3(0.0)), c.a);
}
