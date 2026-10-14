// Author: bobylito
// License: MIT
// Description: The incoming image appears through growing polka dots
// Tags: dots, pattern

const float SQRT_2 = 1.414213562373;
// @param Number of dots across the image



vec4 transition(vec2 uv) {
  bool nextImage = distance(fract(uv * dots), vec2(0.5, 0.5)) < ( progress / distance(uv, center));
  return nextImage ? getToColor(uv) : getFromColor(uv);
}
