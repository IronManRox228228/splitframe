// Show the clip only inside (or outside) a polygon of up to 8 points; feathered edge.
vec4 effect(vec2 uv) {
  vec2 pts[8];
  pts[0] = p0;
  pts[1] = p1;
  pts[2] = p2;
  pts[3] = p3;
  pts[4] = p4;
  pts[5] = p5;
  pts[6] = p6;
  pts[7] = p7;

  int n = clamp(count, 3, 8);
  float minEdgeDist = 1e6;
  bool inside = false;

  for (int i = 0; i < 8; ++i) {
    if (i >= n) break;
    int j = (i + 1) % n;
    vec2 a = pts[i];
    vec2 b = pts[j];

    // Even-odd crossing test with horizontal ray to +x
    if ((a.y > uv.y) != (b.y > uv.y)) {
      float xCross = a.x + (uv.y - a.y) * (b.x - a.x) / (b.y - a.y);
      if (uv.x < xCross) {
        inside = !inside;
      }
    }

    // Distance from uv to segment [a, b]
    vec2 ab = b - a;
    vec2 ap = uv - a;
    float lenSq = dot(ab, ab);
    float t = lenSq > 1e-8 ? clamp(dot(ap, ab) / lenSq, 0.0, 1.0) : 0.0;
    vec2 closest = a + t * ab;
    minEdgeDist = min(minEdgeDist, length(uv - closest));
  }

  float signedDist = inside ? minEdgeDist : -minEdgeDist;
  float f = max(feather, 1e-5);
  float alpha = smoothstep(-f, f, signedDist);
  if (invert) {
    alpha = 1.0 - alpha;
  }
  alpha *= clamp(opacity, 0.0, 1.0);

  return getColor(uv) * alpha;
}
