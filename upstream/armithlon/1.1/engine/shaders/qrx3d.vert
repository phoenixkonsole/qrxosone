#version 450
layout(location=0) in vec3 positionPx;
layout(location=1) in vec4 vertexColor;
layout(location=0) out vec4 fragmentColor;
// Vulkan viewport with positive height: NDC -1 is top, +1 is bottom.
void main() {
    gl_Position=vec4(positionPx.x*(2.0/800.0)-1.0,
                     positionPx.y*(2.0/450.0)-1.0,
                     positionPx.z,1.0);
    fragmentColor=vertexColor;
}
