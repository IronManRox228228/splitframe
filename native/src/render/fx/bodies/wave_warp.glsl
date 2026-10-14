vec4 effect(vec2 uv) {
  float a = radians(angle);
  vec2 dir = vec2(cos(a), sin(a));
  float ph = dot((uv - 0.5) * vec2(ratio, 1.0), dir) / max(wavelength, 0.01) * 6.2831853 + time * speed * 6.2831853;
  return getColor(uv + vec2(-dir.y, dir.x) * sin(ph) * amplitude * vec2(1.0 / ratio, 1.0));
}
