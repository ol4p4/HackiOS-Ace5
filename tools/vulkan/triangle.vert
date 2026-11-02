#version 450
/* The triangle the gate's next stage draws: no vertex buffers, no descriptors --
 * the three corners come from gl_VertexIndex, so the only turning parts are the
 * pipeline and the draw itself. */
void main()
{
	vec2 corners[3] = vec2[](vec2(-0.75, -0.75), vec2(0.75, -0.75), vec2(0.0, 0.75));

	gl_Position = vec4(corners[gl_VertexIndex], 0.0, 1.0);
}
