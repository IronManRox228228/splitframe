// Author: gre
// License: MIT
// Description: A fade through a solid color, black by default
// Tags: fade, color
// @color Color to fade through

// @range(0, 1) How long the color.rgb shows in the middle

vec4 transition (vec2 uv) {
  return mix(
    mix(vec4(color.rgb, 1.0), getFromColor(uv), smoothstep(1.0-colorPhase, 0.0, progress)),
    mix(vec4(color.rgb, 1.0), getToColor(uv), smoothstep(    colorPhase, 1.0, progress)),
    progress);
}
