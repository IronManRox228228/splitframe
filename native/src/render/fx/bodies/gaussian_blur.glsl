// Separable gaussian: pass 0 blurs along x, pass 1 along y. radius is in pixels (sigma = radius / 2, at most 48 taps a side).
vec4 effect(vec2 uv) {
  vec2 stepUv = fxPass == 0 ? vec2(1.0 / resolution.x, 0.0) : vec2(0.0, 1.0 / resolution.y);
  float sigma = max(radius * 0.5, 0.0001);
  int n = int(min(ceil(radius), 48.0));
  vec4 acc = vec4(0.0);
  float sum = 0.0;
  for (int i = -48; i <= 48; ++i) {
    if (abs(i) > n) continue;
    float w = exp(-0.5 * float(i * i) / (sigma * sigma));
    acc += getColor(uv + stepUv * float(i)) * w;
    sum += w;
  }
  return acc / sum;
}
