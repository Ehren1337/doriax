#version 450

// Vertexless fullscreen triangle: drawn with draw(3, 1) and no vertex buffer.
// gl_VertexIndex 0,1,2 -> uv (0,0),(2,0),(0,2) -> clip triangle covering the screen.
out vec2 v_texcoord;

void main() {
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_texcoord = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    #ifdef IS_VULKAN
        // Sokol uses a negative Vulkan viewport to keep GL-style clip-space Y.
        // Vulkan texture coordinates still start at the top, so flip the varying.
        v_texcoord.y = 1.0 - v_texcoord.y;
        // GL [-1,1] to Vulkan [0,1] depth range
        gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
    #endif
}
