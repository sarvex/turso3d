#include "Uniforms.glsl"

#ifdef COMPILEVS

#include "Transform.glsl"

in vec3 position;
in vec3 normal;

out vec4 vWorldPos;
out vec3 vNormal;
out vec3 vViewNormal;
noperspective out vec2 vScreenPos;

#else

#include "Lighting.glsl"

in vec4 vWorldPos;
in vec3 vNormal;
in vec3 vViewNormal;
noperspective in vec2 vScreenPos;
out vec4 fragColor[2];

uniform vec4 matDiffColor;
#endif

void vert()
{
    mat3x4 modelMatrix = GetWorldMatrix();

    vWorldPos.xyz = vec4(position, 1.0) * modelMatrix;
    vNormal = normalize((vec4(normal, 0.0) * modelMatrix));
    vViewNormal = (vec4(vNormal, 0.0) * viewMatrix) * 0.5 + 0.5;
    gl_Position = vec4(vWorldPos.xyz, 1.0) * viewProjMatrix;
    vWorldPos.w = CalculateDepth(gl_Position);
    vScreenPos = CalculateScreenPos(gl_Position);
}

void frag()
{
    fragColor[0] = vec4(matDiffColor.rgb * CalculateLighting(vWorldPos, vNormal, vScreenPos), matDiffColor.a);
    fragColor[1] = vec4(vViewNormal, 1.0);
}
