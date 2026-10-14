// Author: gre
// License: MIT
// Description: A crossfade that brightens toward a color in the middle
// Tags: fade, color, flash
// @color Color the images brighten toward

vec4 transition (vec2 uv) {
  return mix(
    getFromColor(uv) + vec4(progress*color.rgb, 1.0),
    getToColor(uv) + vec4((1.0-progress)*color.rgb, 1.0),
    progress
  );
}
