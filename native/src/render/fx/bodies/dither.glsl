float bayer8(vec2 p) {
  ivec2 q = ivec2(mod(p, 8.0));
  int xr = q.x ^ q.y;
  int v = ((q.y & 1) << 5) | ((xr & 1) << 4) | ((q.y & 2) << 2) | ((xr & 2) << 1) | ((q.y & 4) >> 1) | ((xr & 4) >> 2);
  return (float(v) + 0.5) / 64.0 - 0.5;
}

vec4 effect(vec2 uv) {
  vec4 c = getColor(uv);
  vec3 sc = fxPerc(fxUnpremul(c));
  float n = float(levels) - 1.0;
  float b = bayer8(floor(uv * resolution / max(scale, 1.0)));
  vec3 q = floor(clamp(sc * n + b * amount, 0.0, n) + 0.5) / n;
  return vec4(fxLin(q) * c.a, c.a);
}
