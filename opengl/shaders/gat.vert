#version 330 core
layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec3 aNormal;

uniform mat4 uModel;
uniform mat4 uViewProjection;

out VS_OUT {
    vec3 localPosition;
    vec3 localNormal;
    vec3 worldPosition;
    vec3 worldNormal;
} vs;

void main()
{
    vec4 world = uModel * vec4(aPosition, 1.0);
    vs.localPosition = aPosition;
    vs.localNormal = aNormal;
    vs.worldPosition = world.xyz;
    vs.worldNormal = mat3(transpose(inverse(uModel))) * aNormal;
    gl_Position = uViewProjection * world;
}
