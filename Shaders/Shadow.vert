#version 450
layout(location=0) in vec3 a_Position;
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
}
