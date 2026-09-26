#version 450
/* EDS witness, adjacency with a geometry stage: the triangle is vertices 0, 2
 * and 4; the colour comes from adjacency vertex 1, so the column shows its
 * colour only if the stage received the adjacency vertices. */
layout(triangles_adjacency) in;
layout(triangle_strip, max_vertices = 3) out;
layout(location = 0) in vec4 color_in[];
layout(location = 0) flat out vec4 color;
void main()
{
    for (int i = 0; i < 3; ++i) {
        gl_Position = gl_in[2 * i].gl_Position;
        color = color_in[1];
        EmitVertex();
    }
    EndPrimitive();
}
