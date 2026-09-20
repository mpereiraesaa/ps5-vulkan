#version 450
/* The fragment position built-in. gl_FragCoord is core Vulkan and needs no
 * feature, and the hardware delivers it in the position VGPRs the pixel stage
 * is launched with (SPI_PS_INPUT_ENA POS_*_FLOAT_ENA), so it is an input the
 * profile can carry without a new export from the pre-raster stage.
 *
 * It is a T05 prerequisite rather than a new feature of its own: the upstream
 * oracle for depthClamp
 * (dEQP-VK.clipping.clip_volume.depth_clamp.*, vktClippingTests.cpp:565-660)
 * colours each pixel with gl_FragCoord.z, so a profile that refuses the
 * built-in cannot run the only applicable depthClamp leaves. This fixture is
 * that exact read: the z component, written to the green channel. */
layout(location = 0) out vec4 o_color;
void main()
{
    o_color = vec4(1.0, gl_FragCoord.z, 0.0, 1.0);
}
