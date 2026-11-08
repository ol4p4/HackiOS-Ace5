#version 450
/* The swapchain step's frame-1 colour: one flat colour that differs from the
 * step-8 draw, so a stale or swapped present shows up in the pixels. */
layout(location = 0) out vec4 color;

void main()
{
	color = vec4(0.0, 1.0, 0.0, 1.0);
}
