#version 440
// Refraction glass: a rounded slab that bends what is behind it. The texture is the backdrop region under the slab, grown by `margin`
// pixels on every side. Near the rim the sample point is pulled from further inside the slab (a lens: stronger the closer to the
// edge, along the rim's normal), the picture is softened with a small disc blur, red and blue are split a little along the normal
// (chromatic fringe), then the glass tint goes on and a specular rim is added, brighter toward the top-left light.
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 glassSize;   // slab size in px
    vec2 srcSize;     // backdrop texture size in px (slab + 2 * margin)
    float margin;
    float radius;     // corner radius in px
    float bevel;      // how far in from the rim the lens reaches, px
    float strength;   // peak displacement at the rim, px
    float blur;       // blur disc radius, px
    float aberration; // red/blue split at the rim, px
    vec4 tint;        // glass colour (rgb) and how much of it covers the picture (a)
    vec4 rim;         // specular rim colour (rgb) and strength (a)
};
layout(binding = 1) uniform sampler2D src;

vec3 tap(vec2 uv, vec2 px, float r) {
    vec3 c = texture(src, uv).rgb;
    c += texture(src, uv + vec2(0.924, 0.383) * r * px).rgb;
    c += texture(src, uv + vec2(-0.383, 0.924) * r * px).rgb;
    c += texture(src, uv + vec2(-0.924, -0.383) * r * px).rgb;
    c += texture(src, uv + vec2(0.383, -0.924) * r * px).rgb;
    c += texture(src, uv + vec2(0.707, -0.707) * r * px * 0.6).rgb;
    c += texture(src, uv + vec2(0.707, 0.707) * r * px * 0.6).rgb;
    c += texture(src, uv + vec2(-0.707, 0.707) * r * px * 0.6).rgb;
    c += texture(src, uv + vec2(-0.707, -0.707) * r * px * 0.6).rgb;
    return c / 9.0;
}

void main() {
    vec2 p = qt_TexCoord0 * glassSize;
    vec2 c = glassSize * 0.5;
    vec2 q = abs(p - c) - (c - vec2(radius));
    float d = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - radius; // < 0 inside
    float inside = max(-d, 0.0);
    float cover = 1.0 - smoothstep(-0.75, 0.75, d);

    vec2 s = sign(p - c);
    vec2 mq = max(q, vec2(0.0));
    vec2 n = (mq.x > 0.0 || mq.y > 0.0) ? normalize(mq) * s : (q.x > q.y ? vec2(s.x, 0.0) : vec2(0.0, s.y));

    float e = 1.0 - clamp(inside / max(bevel, 1.0), 0.0, 1.0);
    float lens = e * e;
    vec2 base = p - n * lens * strength;
    vec2 px = 1.0 / srcSize;
    vec2 uv = (base + vec2(margin)) * px;
    vec2 ca = n * lens * aberration * px;
    float rr = tap(uv - ca, px, blur).r;
    float gg = tap(uv, px, blur).g;
    float bb = tap(uv + ca, px, blur).b;
    vec3 col = vec3(rr, gg, bb);

    col = mix(col, tint.rgb, tint.a);

    vec2 light = normalize(vec2(-0.6, -0.8));
    float facing = max(dot(n, light), 0.0);
    float edge = exp(-inside / 1.4);
    float spec = edge * (0.30 + 0.70 * facing) * rim.a + exp(-inside / 5.0) * facing * 0.10 * rim.a;
    col += rim.rgb * spec + vec3(0.05) * lens;

    fragColor = vec4(clamp(col, 0.0, 1.0) * cover, cover) * qt_Opacity;
}
