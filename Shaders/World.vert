#version 450
layout(location=0) in vec3 a_Position;
layout(location=1) in vec3 a_Normal;
layout(location=2) in vec2 a_TexCoord;
layout(set=0,binding=256,std140) uniform CameraData
{
    mat4 ViewProjection;
    vec4 CameraPosition;
} u_Camera;
layout(push_constant,std430) uniform ObjectData
{
    mat4 Transform;
    vec4 AlbedoRoughness;
    vec4 EmissionMetallic;
} u_Object;
layout(location=0) out vec3 v_WorldPosition;
layout(location=1) out vec3 v_Normal;
void main()
{
    vec4 worldPosition = u_Object.Transform * vec4(a_Position, 1.0);
    v_WorldPosition = worldPosition.xyz;
    v_Normal = normalize(transpose(inverse(mat3(u_Object.Transform))) * a_Normal);
    gl_Position = u_Camera.ViewProjection * worldPosition;
}
