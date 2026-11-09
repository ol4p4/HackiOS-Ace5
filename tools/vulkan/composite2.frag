#version 450
/* Two layers, blended the way a compositor blends them: the upper layer's
 * alpha decides how much of the lower one survives. The upper layer is a red
 * wash whose alpha runs left to right, so every pixel of the picture is a
 * statement about the blend -- the left edge is pure under, the right edge pure
 * over, and the readback can compute the expected byte at each sample. */
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D under;
layout(set = 0, binding = 1) uniform sampler2D over;

void main()
{
	vec4 u = texture(under, uv);
	vec4 o = texture(over, uv);

	color = vec4(o.rgb * o.a + u.rgb * (1.0 - o.a), 1.0);
}
