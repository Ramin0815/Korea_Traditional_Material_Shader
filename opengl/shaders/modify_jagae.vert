#version 330 core
layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aUV;

uniform mat4 uModel;
uniform mat4 uViewProjection;

out JAGAE_VERTEX {
    vec3 worldPosition;
    vec3 worldNormal;
    vec2 uv;
} jagae;

void main()
{
    vec4 world = uModel * vec4(aPosition, 1.0);
    jagae.worldPosition = world.xyz;
    jagae.worldNormal = mat3(transpose(inverse(uModel))) * aNormal;
    jagae.uv = aUV;
    gl_Position = uViewProjection * world;
}
