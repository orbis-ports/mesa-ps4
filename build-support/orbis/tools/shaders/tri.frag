#version 450

// A colour derived from the fragment's own integer position, so the read-back is a check on WHERE each
// fragment landed and not just on whether something was written. A flat colour would pass even if every
// fragment went to the wrong pixel.
//
// R = x & 0xff, G = y & 0xff, B = 0xa5, A = 0xff. The 0xa5 is the same marker the other tests use: not zero
// and not 0xff, so an untouched or wrongly-formatted target fails rather than passes.

layout(location = 0) out vec4 colour;

void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  colour = vec4(float(p.x & 0xff) / 255.0,
                float(p.y & 0xff) / 255.0,
                165.0 / 255.0,
                1.0);
}
