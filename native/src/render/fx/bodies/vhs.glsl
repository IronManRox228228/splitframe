vec4 effect(vec2 uv) {
  float fr = floor(time * 30.0) + float(seed) * 3.7;
  float row = floor(uv.y * resolution.y);
  float jit = (fxHash(vec2(floor(row * 0.5), fr)) - 0.5) * 0.006 * amount;
  float wob = sin(uv.y * 24.0 + time * 3.0) * 0.0015 * amount;
  float bandY = fract(time * 0.11 + float(seed) * 0.173);
  float band = smoothstep(0.07, 0.0, abs(uv.y - bandY));
  float x = uv.x + jit + wob + band * sin(uv.y * 90.0 + fr) * 0.02 * amount;
  float o = bleed * 0.012;
  vec4 c = getColor(vec2(x, uv.y));
  vec4 cr = 0.5 * (getColor(vec2(x - o, uv.y)) + getColor(vec2(x - o * 2.0, uv.y)));
  vec4 cb = 0.5 * (getColor(vec2(x + o, uv.y)) + getColor(vec2(x + o * 2.0, uv.y)));
  vec3 rgb = vec3(cr.r, c.g, cb.b);
  float n = fxHash(floor(uv * resolution) + fr * vec2(13.1, 7.7)) - 0.5;
  rgb += (n * 0.12 + band * (fxHash(vec2(row, fr)) - 0.4) * 0.3) * noise * c.a;
  return vec4(max(rgb, vec3(0.0)), c.a);
}
