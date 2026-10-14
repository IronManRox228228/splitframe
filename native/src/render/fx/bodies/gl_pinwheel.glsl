// Author: Mr Speaker
// License: MIT
// Description: A spinning pinwheel reveals the incoming image
// Tags: pinwheel, radial, rotate

// @param Rotation speed

vec4 transition(vec2 uv) {
  
  vec2 p = uv.xy / vec2(1.0).xy;
  
  float circPos = atan(p.y - 0.5, p.x - 0.5) + progress * speed;
  float modPos = mod(circPos, 3.1415 / 4.);
  float signed_ = sign(progress - modPos);
  
  return mix(getToColor(p), getFromColor(p), step(signed_, 0.5));
  
}
