vec4 effect(vec2 uv) {
  float k = curvature * 0.15;
  vec2 p = uv * 2.0 - 1.0;
  vec2 q = p * vec2(1.0 + k * p.y * p.y, 1.0 + k * p.x * p.x) / (1.0 + k);
  vec2 s = q * 0.5 + 0.5;
  vec4 c = getColor(s);
  vec3 rgb = c.rgb;
  float scan = 0.5 + 0.5 * cos(s.y * resolution.y / max(line_size, 1.0) * 6.2831853);
  rgb *= mix(1.0, 0.35 + 0.65 * scan, scanlines) * (1.0 + scanlines * 0.15);
  float m = mod(floor(s.x * resolution.x), 3.0);
  vec3 tri = vec3(m < 0.5 ? 1.0 : 0.6, (m > 0.5 && m < 1.5) ? 1.0 : 0.6, m > 1.5 ? 1.0 : 0.6);
  rgb *= mix(vec3(1.0), tri * 1.36, mask);
  rgb *= 1.0 - curvature * 0.18 * dot(p, p);
  return vec4(rgb, c.a);
}
