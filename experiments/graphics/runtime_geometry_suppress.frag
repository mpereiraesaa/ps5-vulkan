#version 450
/* Synthetic suppress diagnostic fragment stage: it reads no input, so a
 * geometry program that emits nothing still forms a legal pipeline under this
 * profile's draw-ABI rule (every fragment input must match a pre-raster
 * output, and a stage that emits nothing exports none). It is used only for the
 * witness's suppress case; every other case keeps the shared fragment stage. */
layout(location = 0) out vec4 o_color;
void main() { o_color = vec4(0.0, 0.0, 0.0, 1.0); }
