// Author: gre
// License: MIT
// Description: A crossfade in which each color channel changes at a different time
// Tags: fade, color

// Usage: fromStep and toStep must be in [0.0, 1.0] range 
// and all(fromStep) must be < all(toStep)

// @range(0, 1) When each channel (r, g, b, a) starts changing

// params that are not a single float / int / bool / vec2 / colour become scalars, re-assembled here
#define fromStep vec4(fromStep_x, fromStep_y, fromStep_z, fromStep_w)
#define toStep vec4(toStep_x, toStep_y, toStep_z, toStep_w)

// @range(0, 1) When each channel (r, g, b, a) finishes changing


vec4 transition (vec2 uv) {
  vec4 a = getFromColor(uv);
  vec4 b = getToColor(uv);
  return mix(a, b, smoothstep(fromStep, toStep, vec4(progress)));
}
