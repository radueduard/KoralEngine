#version 460 core

module default;

import entity;
import koral_debug;     // what the editor's scene view shows of the material: its colour, its normal

struct Vertex {
    vec3 position : POSITION;
    vec3 normal : NORMAL;
};

struct RasterData {
    vec4 position : SV_Position;
    vec3 worldPosition;
    vec3 worldNormal;
};

struct Camera {
    mat4 view;
    mat4 projection;
    vec3 eye;
};
ConstantBuffer<Camera> camera;

[shader("vertex")]
RasterData vertex(
    in Vertex vertex,
    uniform mat4 model 
) {
    vec4 worldPosition = model * vec4(vertex.position, 1.f);
    vec3 worldNormal = mat3(inverse(transpose(model))) * vertex.normal; 
    return {
        camera.projection * camera.view * worldPosition,
        worldPosition.xyz,
        worldNormal
    };
}

struct DirectionalLight {
    vec3 direction;
    float intensity;
    vec3 color;
};
ConstantBuffer<DirectionalLight> light;

[shader("fragment")]
vec4 fragment(
    in RasterData raster,
    uniform mat4 model,
    uniform float3 color
) : SV_Target {
    vec3 L = normalize(camera.eye - raster.worldPosition);
    vec3 N = normalize(raster.worldNormal);
    if (koralDebugShowing()) return vec4(koralDebugColor(KoralSurface(color, N, 1.f, 0.f)), 1.f);

    return vec4(color * light.color * light.intensity * max(0.f, dot(N, L)), 1.f);
}