// Glass light reduction: one texel of the target averages an 8x8 block of the
// source (16 bilinear taps, each the mean of 2x2 texels). Linear light in, out.
precision highp float;

varying vec2 v_texcoord;
uniform sampler2D tex;
uniform vec2 texel; // one source texel in texture coordinates

void main() {
  vec4 sum = vec4(0.0);
  for (int y = 0; y < 4; y++) {
    for (int x = 0; x < 4; x++) {
      sum += texture2D(tex, v_texcoord + (vec2(float(x), float(y)) - 1.5) * 2.0 * texel);
    }
  }
  gl_FragColor = sum / 16.0;
}
