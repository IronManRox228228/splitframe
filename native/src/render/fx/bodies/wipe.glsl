// A soft edge sweeps across the frame along `direction`: the incoming picture appears behind it, starting on the
// side the edge comes from.
vec4 transition(vec2 uv) {
  vec2 v = normalize(direction);
  float extent = abs(v.x) + abs(v.y);               // how far v reaches across the unit square
  float d = dot(uv - vec2(0.5), v) / extent + 0.5;  // 0..1 along v
  float s = max(softness, 0.001);
  float front = mix(-s, 1.0, progress);
  float m = 1.0 - smoothstep(front, front + s, d);  // 1 = incoming
  return mix(getFromColor(uv), getToColor(uv), m);
}
