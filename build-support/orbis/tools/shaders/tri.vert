#version 450

// A full-screen triangle from gl_VertexIndex alone - no vertex buffers, no input state. That keeps the test
// about the SHADER STAGES and the render path rather than about vertex fetch, which has its own failure modes
// and deserves its own test.
//
// Three vertices covering the whole viewport: (-1,-1), (3,-1), (-1,3). The oversized triangle is the standard
// trick; clipping does the rest, and it means the fragment shader runs on every pixel.

void main() {
  vec2 p = vec2((gl_VertexIndex == 1) ? 3.0 : -1.0,
                (gl_VertexIndex == 2) ? 3.0 : -1.0);
  gl_Position = vec4(p, 0.0, 1.0);
}
