#version 310 es

uniform vec4 UBO[4];
in vec4 aVertex;
out vec3 vNormal;
in vec3 aNormal;

void main()
{
    gl_Position = mat4(UBO[0],UBO[1],UBO[2],UBO[3]) * aVertex;
    vNormal = aNormal;
}

