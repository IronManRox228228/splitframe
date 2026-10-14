vec4 effect(vec2 uv) {
  float a = radians(angle);
  vec2 p = (uv - vec2(0.5, 1.0 - focus)) * vec2(ratio, 1.0);
  float d = abs(dot(p, vec2(-sin(a), cos(a))));
  float k = smoothstep(band * 0.5, band * 0.5 + max(falloff, 0.01), d);
  float rr = maxblur * k * resolution.y;
  if (rr < 0.3) return getColor(uv);
  vec4 acc = vec4(0.0);
  for (int i = 0; i < 40; ++i) {
    float r = sqrt((float(i) + 0.5) / 40.0);
    float th = float(i) * 2.39996323;
    acc += getColor(uv + vec2(cos(th), sin(th)) * r * rr / resolution);
  }
  return acc / 40.0;
}
