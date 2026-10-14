// Splits red and blue apart along an angle (or radially, growing towards the edges); green stays put.
vec4 effect(vec2 uv) {
  vec2 off = vec2(cos(radians(angle)), sin(radians(angle))) * amount;
  if (radial) off = (uv - 0.5) * amount * 2.0;
  vec4 r = getColor(uv + off);
  vec4 g = getColor(uv);
  vec4 b = getColor(uv - off);
  return vec4(r.r, g.g, b.b, max(max(r.a, g.a), b.a));
}
