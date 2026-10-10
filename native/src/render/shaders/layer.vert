#version 440

// One layer = one quad. `pos` is the unit square; mvp puts it on the canvas (item transform and all).
// The output pass (output.frag) draws its quad with this same vertex shader.
layout(location = 0) in vec2 pos;
layout(location = 0) out vec2 uv;

// Keep this block identical in layer.frag and output.frag (and in compositor.cpp's Params).
layout(std140, binding = 0) uniform U {
  mat4 mvp;
  vec4 uvx;   // source u = dot(uvx.xy, pos) + uvx.z   (undoes the stream's rotation metadata)
  vec4 uvy;
  vec4 yuvR;  // planar YUV -> RGB: out = dot(row.xyz, samples) + row.w
  vec4 yuvG;
  vec4 yuvB;
  vec4 fxR;   // colour effects on RGB, same layout
  vec4 fxG;
  vec4 fxB;
  vec4 params; // x opacity, y mode (0 RGBA straight alpha, 1 RGBA premultiplied, 2 NV12/P010), z transfer id, w canvas encoding
  vec4 flags;  // x bits (1 LUT, 2 HDR tone map, 4 effects), y LUT coord scale, z LUT coord offset
  vec4 primR;  // layer: source primaries -> working space; output: working space -> target primaries
  vec4 primG;
  vec4 primB;
  vec4 toDispR;   // working space -> Rec.709 primaries (what display-referred maths is done in)
  vec4 toDispG;
  vec4 toDispB;
  vec4 fromDispR; // and back
  vec4 fromDispG;
  vec4 fromDispB;
} u;

out gl_PerVertex {
  vec4 gl_Position;
};

void main() {
  uv = vec2(dot(u.uvx.xy, pos) + u.uvx.z, dot(u.uvy.xy, pos) + u.uvy.z);
  gl_Position = u.mvp * vec4(pos, 0.0, 1.0);
}
