vec4 effect(vec2 uv) {
  float sd = float(seed) * 1.618 + floor(time * rate) * 7.13;
  vec2 cells = vec2(max(floor(float(blocks) * ratio * 0.5), 1.0), float(blocks));
  vec2 id = floor(uv * cells);
  float hit = step(1.0 - amount * 0.5, fxHash(id + sd));
  float dx = (fxHash(id.yx + sd * 1.3) - 0.5) * 0.25 * amount * hit;
  float rowHit = step(1.0 - amount * 0.25, fxHash(vec2(id.y, sd + 3.0)));
  dx += (fxHash(vec2(id.y, sd + 9.0)) - 0.5) * 0.15 * amount * rowHit;
  float s = amount * 0.012 * (0.4 + 1.6 * max(hit, rowHit));
  vec2 p = vec2(uv.x + dx, uv.y);
  vec4 g = getColor(p);
  return vec4(getColor(p + vec2(s, 0.0)).r, g.g, getColor(p - vec2(s, 0.0)).b, g.a);
}
