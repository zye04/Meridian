#version 460

layout(location = 0) in vec3 inWorldPosition;
layout(location = 1) in vec3 inNormal;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 normal = normalize(inNormal);
    vec3 grass = vec3(0.10, 0.25, 0.05);
    vec3 rock = vec3(0.25, 0.22, 0.20);
    vec3 albedo = mix(rock, grass, smoothstep(0.7, 0.9, normal.y));
    // Placeholder shading until the sun is added: brighter where flatter.
    outColor = vec4(albedo * (0.3 + 0.7 * normal.y), 1.0);
}
