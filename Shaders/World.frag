#version 450
layout(location=0) in vec3 v_WorldPosition;
layout(location=1) in vec3 v_Normal;
layout(location=0) out vec4 o_Color;
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
const float Pi = 3.14159265359;
vec3 Tonemap(vec3 value)
{
    return clamp((value*(2.51*value+.03))/(value*(2.43*value+.59)+.14),0.,1.);
}
void main()
{
    vec3 albedo = pow(u_Object.AlbedoRoughness.rgb, vec3(2.2));
    vec3 normal = normalize(v_Normal);
    vec3 viewDirection = normalize(u_Camera.CameraPosition.xyz-v_WorldPosition);
    vec3 lightDirection = normalize(vec3(-.6,.85,-.35));
    vec3 halfwayDirection = normalize(lightDirection+viewDirection);
    float normalLight = max(dot(normal,lightDirection),0.);
    float normalView = max(dot(normal,viewDirection),.001);
    float normalHalfway = max(dot(normal,halfwayDirection),0.);
    float roughness = max(.15,u_Object.AlbedoRoughness.a);
    float alphaSquared = pow(roughness,4.);
    float distribution = alphaSquared / (Pi*pow(normalHalfway*normalHalfway*(alphaSquared-1.)+1.,2.)+.00001);
    float geometryK = pow(roughness+1.,2.)/8.;
    float geometry = normalLight/(normalLight*(1.-geometryK)+geometryK)*normalView/(normalView*(1.-geometryK)+geometryK);
    float metallic = u_Object.EmissionMetallic.a;
    vec3 reflection = mix(vec3(.04),albedo,metallic);
    vec3 fresnel = reflection+(1.-reflection)*pow(1.-max(dot(halfwayDirection,viewDirection),0.),5.);
    vec3 specular = distribution*geometry*fresnel/(4.*normalLight*normalView+.001);
    vec3 ambient = albedo*mix(vec3(.10,.15,.23),vec3(.32,.43,.59),normal.y*.5+.5);
    vec3 color = ambient+((1.-fresnel)*(1.-metallic)*albedo/Pi+specular)*vec3(4.,3.3,2.5)*normalLight;
    color += u_Object.EmissionMetallic.rgb;
    float fog = 1.-exp(-length(u_Camera.CameraPosition.xyz-v_WorldPosition)*.003);
    color = mix(color,vec3(.25,.36,.5),fog);
    o_Color = vec4(pow(Tonemap(color),vec3(1./2.2)),1.);
}
