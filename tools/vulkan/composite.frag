#version 450
/* One texture, sampled. The pixel that comes out is the pixel that went in:
 * the source is a checker whose two colours are told apart by every channel,
 * so a channel swap, a stride mistake or a staging copy that never arrived is
 * visible on the panel and countable in the readback. */
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D source;

void main()
{
	color = texture(source, uv);
}
