vec4 effect(vec2 uv) {
  vec4 c = getColor(uv);
  float fr = floor(time * rate) + float(seed) * 7.13;
  vec2 g = floor(uv * resolution / size);
  float lum = fxHash(g + fr * vec2(37.1, 17.3)) - 0.5;
  vec3 col = vec3(fxHash(g + fr * vec2(11.7, 23.9) + 5.0), fxHash(g + fr * vec2(7.3, 3.1) + 9.0), fxHash(g + fr * vec2(29.3, 13.7) + 13.0)) - 0.5;
  vec3 p = fxPerc(fxUnpremul(c));
  float l = fxLuma(p);
  float w = 0.35 + 2.6 * l * (1.0 - l);
  p = clamp(p + (vec3(lum) + col * chroma) * amount * 0.35 * w, 0.0, 1.0);
  return vec4(fxLin(p) * c.a, c.a);
}
