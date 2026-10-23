#version 450
layout(location=0) in vec3 v_WorldPosition;
layout(location=1) in vec3 v_Normal;
layout(location=0) out vec4 o_Color;
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
const float Pi = 3.14159265359;
layout(set=0,binding=0) uniform texture2D u_ShadowNear;
layout(set=0,binding=1) uniform texture2D u_ShadowFar;
layout(set=0,binding=128) uniform sampler u_ShadowSampler;
float CascadeVisibility(texture2D depthTexture,mat4 lightMatrix,vec3 position,vec3 normal)
{
    vec4 projected = lightMatrix * vec4(position+normal*.02,1.);
    vec3 clip = projected.xyz/projected.w;
    vec2 uv = vec2(clip.x*.5+.5,.5-clip.y*.5);
    if(any(lessThan(uv,vec2(0.))) || any(greaterThan(uv,vec2(1.))) || clip.z<0. || clip.z>1.)
        return 1.;
    vec2 texel = 1./vec2(textureSize(sampler2D(depthTexture,u_ShadowSampler),0));
    float bias = .00008+.00010*(1.-max(dot(normal,normalize(u_Camera.SunDirection.xyz)),0.));
    float visibility = 0.;
    for(int y=-2;y<=2;y++)
        for(int x=-2;x<=2;x++)
            visibility += clip.z-bias <= texture(sampler2D(depthTexture,u_ShadowSampler),uv+vec2(x,y)*texel).r ? 1. : 0.;
    return visibility/25.;
}
float SunVisibility(vec3 position,vec3 normal)
{
    if(u_Camera.RenderParameters.x<.5)
        return 1.;
    vec4 nearClip = u_Camera.LightViewProjection[0]*vec4(position,1.);
    float edge = max(abs(nearClip.x),abs(nearClip.y))/nearClip.w;
    float farVisibility = CascadeVisibility(u_ShadowFar,u_Camera.LightViewProjection[1],position,normal);
    if(edge>=.98)
        return farVisibility;
    return mix(CascadeVisibility(u_ShadowNear,u_Camera.LightViewProjection[0],position,normal),farVisibility,smoothstep(.78,.98,edge));
}
void main()
{
    vec3 albedo = pow(u_Object.AlbedoRoughness.rgb, vec3(2.2));
    vec3 normal = normalize(v_Normal);
    vec3 viewDirection = normalize(u_Camera.CameraPosition.xyz-v_WorldPosition);
    vec3 lightDirection = normalize(u_Camera.SunDirection.xyz);
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
    vec3 color = ambient+((1.-fresnel)*(1.-metallic)*albedo/Pi+specular)*u_Camera.SunRadianceFog.rgb*normalLight*SunVisibility(v_WorldPosition,normal);
    color += u_Object.EmissionMetallic.rgb;
    float fog = 1.-exp(-length(u_Camera.CameraPosition.xyz-v_WorldPosition)*u_Camera.SunRadianceFog.w);
    color = mix(color,vec3(.25,.36,.5),fog);
    o_Color = vec4(color,1.);
}
