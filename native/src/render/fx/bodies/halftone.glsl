// perceptual colour at the centre of the rotated-grid cell holding uv, and the distance to that centre in cell units
vec3 htCell(vec2 uv, float ang, out float d) {
  vec2 asp = vec2(ratio, 1.0);
  float ca = cos(ang), sa = sin(ang);
  vec2 q = uv * asp / size;
  vec2 g = vec2(ca * q.x + sa * q.y, -sa * q.x + ca * q.y);
  vec2 cg = floor(g) + 0.5;
  d = length(g - cg);
  vec2 back = vec2(ca * cg.x - sa * cg.y, sa * cg.x + ca * cg.y);
  return fxPerc(fxUnpremul(getColor(back * size / asp)));
}

float inkCoverage(vec2 uv, float ang, int ink) {
  float d;
  vec3 pc = htCell(uv, ang, d);
  float k = 1.0 - max(pc.r, max(pc.g, pc.b));
  float v = ink == 3 ? k : (1.0 - (ink == 0 ? pc.r : (ink == 1 ? pc.g : pc.b)) - k) / max(1.0 - k, 0.001);
  float r = sqrt(clamp(v, 0.0, 1.0)) * 0.707;
  float aa = 0.7 / (size * resolution.y);
  return smoothstep(r + aa, r - aa, d);
}

vec4 effect(vec2 uv) {
  vec4 c = getColor(uv);
  vec3 sc = fxPerc(fxUnpremul(c));
  vec3 ht;
  float base = radians(angle - 45.0);
  if (cmyk) {
    float cc = inkCoverage(uv, radians(15.0) + base, 0);
    float mm = inkCoverage(uv, radians(75.0) + base, 1);
    float yy = inkCoverage(uv, base, 2);
    float kk = inkCoverage(uv, radians(45.0) + base, 3);
    ht = vec3((1.0 - cc) * (1.0 - kk), (1.0 - mm) * (1.0 - kk), (1.0 - yy) * (1.0 - kk));
  } else {
    float d;
    vec3 pc = htCell(uv, radians(angle), d);
    float l = dot(pc, vec3(0.2126, 0.7152, 0.0722));
    float r = sqrt(l) * 0.707;
    float aa = 0.7 / (size * resolution.y);
    ht = pc / max(max(pc.r, max(pc.g, pc.b)), 0.1) * smoothstep(r + aa, r - aa, d);
  }
  return vec4(fxLin(mix(sc, ht, amount)) * c.a, c.a);
}
