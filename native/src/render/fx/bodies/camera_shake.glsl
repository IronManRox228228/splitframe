float n1(float t, float k) { return fxNoise(vec2(t, k)) * 2.0 - 1.0; }

vec4 effect(vec2 uv) {
  float t = time * frequency;
  float sd = float(seed) * 13.37;
  float tx = n1(t, sd + 1.0) + 0.5 * n1(t * 2.3, sd + 5.0);
  float ty = n1(t, sd + 2.0) + 0.5 * n1(t * 2.3, sd + 6.0);
  float tr = n1(t, sd + 3.0) + 0.5 * n1(t * 2.3, sd + 7.0);
  vec2 asp = vec2(ratio, 1.0);
  vec2 q = (uv - 0.5) * asp / crop;
  float a = radians(tr * amount * rotation * 2.0);
  q = vec2(cos(a) * q.x - sin(a) * q.y, sin(a) * q.x + cos(a) * q.y);
  vec2 p = q / asp + 0.5 + vec2(tx, ty) * amount * 0.03 * vec2(1.0 / ratio, 1.0);
  if (edges == 1) p = 1.0 - abs(1.0 - mod(p, 2.0));
  return getColor(p);
}
