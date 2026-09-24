#version 450

layout(set = 0, binding = 0) uniform samplerCubeArray cube_texture;
layout(location = 0) out vec4 color;

void main()
{
    int cell = int(gl_FragCoord.x) / 16;
    int face = cell % 6;
    float cube = float(cell / 6);
    vec3 direction;
    switch (face) {
    case 0: direction = vec3( 1.0,  0.0,  0.0); break;
    case 1: direction = vec3(-1.0,  0.0,  0.0); break;
    case 2: direction = vec3( 0.0,  1.0,  0.0); break;
    case 3: direction = vec3( 0.0, -1.0,  0.0); break;
    case 4: direction = vec3( 0.0,  0.0,  1.0); break;
    default: direction = vec3( 0.0,  0.0, -1.0); break;
    }
    color = textureLod(cube_texture, vec4(direction, cube), 0.0);
}
