#version 450
/* One flat colour, so a pixel that is not the clear colour can only have come
 * from the draw. */
layout(location = 0) out vec4 color;

void main()
{
	color = vec4(1.0, 0.0, 0.0, 1.0);
}
