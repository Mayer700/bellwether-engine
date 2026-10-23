#version 450
layout(location=0) in vec3 a_Position;
layout(location=1) in vec3 a_Normal;
layout(location=2) in vec2 a_TexCoord;
layout(location=3) in vec4 a_Tangent;
layout(location=4) in vec4 a_Color;
layout(set=0,binding=256,std140) uniform CameraData
{
    mat4 ViewProjection;
    mat4 LightViewProjection[2];
    vec4 CameraPosition;
    vec4 SunDirection;
    vec4 SunRadianceFog;
    vec4 RenderParameters;
} u_Camera;
layout(push_constant,std430) uniform ObjectData
{
    mat4 Transform;
    vec4 AlbedoRoughness;
    vec4 EmissionMetallic;
} u_Object;
layout(location=0) out vec3 v_WorldPosition;
layout(location=1) out vec3 v_Normal;
layout(location=2) out vec2 v_TexCoord;
layout(location=3) out vec4 v_Tangent;
layout(location=4) out vec4 v_Color;
void main()
{
    vec4 worldPosition = u_Object.Transform * vec4(a_Position, 1.0);
    v_WorldPosition = worldPosition.xyz;
    v_Normal = normalize(transpose(inverse(mat3(u_Object.Transform))) * a_Normal);
    v_TexCoord = a_TexCoord;
    v_Tangent = vec4(mat3(u_Object.Transform)*a_Tangent.xyz,a_Tangent.w*(determinant(mat3(u_Object.Transform))<0. ? -1. : 1.));
    v_Color = a_Color;
    gl_Position = u_Camera.ViewProjection * worldPosition;
}
