#version 450
layout(location=0) in vec3 a_Position;
layout(location=2) in vec2 a_TexCoord;
layout(location=4) in vec4 a_Color;
layout(location=0) out vec2 v_TexCoord;
layout(location=1) out float v_Alpha;
layout(location=2) flat out float v_Mirrored;
layout(set=0,binding=256,std140) uniform CameraData
{
    mat4 ViewProjection;
} u_Camera;
layout(push_constant,std430) uniform ObjectData
{
    mat4 Transform;
    vec4 AlbedoRoughness;
    vec4 EmissionMetallic;
} u_Object;
void main()
{
    gl_Position = u_Camera.ViewProjection * u_Object.Transform * vec4(a_Position,1.);
    v_TexCoord = a_TexCoord;
    v_Alpha = a_Color.a;
    v_Mirrored = determinant(mat3(u_Object.Transform)) < 0. ? 1. : 0.;
}
