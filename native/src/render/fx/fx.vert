#version 440

// One full-frame quad for every effect pass. `pos` is the unit square; mvp maps it onto the target.
// The block must stay identical in every effect fragment shader (fx/templates/*.tmpl) and in
// render/effects.h's FxUniforms.
layout(location = 0) in vec2 pos;
layout(location = 0) out vec2 fxUv;

layout(std140, binding = 0) uniform U {
  mat4 mvp;
  vec4 fxTiming; // x progress, y time (seconds), z ratio (width / height), w pass index
  vec4 fxRes;    // x width, y height (pixels), z pass count
  vec4 fxP[16];  // the effect's params, one vec4 slot each, in schema order
} u;

out gl_PerVertex {
  vec4 gl_Position;
};

void main() {
  fxUv = pos;
  gl_Position = u.mvp * vec4(pos, 0.0, 1.0);
}
