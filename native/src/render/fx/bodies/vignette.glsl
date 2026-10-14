// Darkens towards the corners. d is 0 at the centre and 1 at the corners whatever the aspect ratio.
vec4 effect(vec2 uv) {
  vec4 c = getColor(uv);
  float d = length((uv - 0.5) * vec2(ratio, 1.0)) / (0.5 * length(vec2(ratio, 1.0)));
  float f = 1.0 - amount * smoothstep(radius, radius + max(softness, 0.001), d);
  return vec4(c.rgb * f, c.a);
}
