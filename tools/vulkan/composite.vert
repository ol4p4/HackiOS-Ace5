#version 450
/* The full-screen quad of the composite step: no vertex buffer, no uniforms --
 * three vertices that cover the viewport, with texture coordinates outside
 * [0,1] at the two off-screen corners so the visible area is 0..1. */
layout(location = 0) out vec2 uv;

void main()
{
	vec2 corners[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
	vec2 texcoords[3] = vec2[](vec2(0.0, 0.0), vec2(2.0, 0.0), vec2(0.0, 2.0));

	gl_Position = vec4(corners[gl_VertexIndex], 0.0, 1.0);
	uv = texcoords[gl_VertexIndex];
}
