#version 450
layout(location=0) in vec3 v_WorldPosition;
layout(location=1) in vec3 v_Normal;
layout(location=2) in vec2 v_TexCoord;
layout(location=3) in vec4 v_Tangent;
layout(location=4) in vec4 v_Color;
layout(location=0) out vec4 o_Color;
layout(set=0,binding=256,std140) uniform CameraData
{
    mat4 ViewProjection;
    mat4 LightViewProjection[2];
    vec4 CameraPosition;
    vec4 SunDirection;
    vec4 SunRadianceFog;
    vec4 RenderParameters;
    vec4 SnowParameters;
    vec4 WindParameters;
} u_Camera;
layout(push_constant,std430) uniform ObjectData
{
    mat4 Transform;
    vec4 AlbedoRoughness;
    vec4 EmissionMetallic;
    vec4 SurfaceFlags;
} u_Object;
layout(set=1,binding=256,std140) uniform MaterialData { vec4 Parameters; vec4 Coverage; } u_Material;
layout(set=1,binding=0) uniform texture2D u_BaseColor;
layout(set=1,binding=1) uniform texture2D u_MetalRough;
layout(set=1,binding=2) uniform texture2D u_NormalMap;
layout(set=1,binding=3) uniform texture2D u_Occlusion;
layout(set=1,binding=4) uniform texture2D u_Emissive;
layout(set=1,binding=128) uniform sampler u_BaseSampler;
layout(set=1,binding=129) uniform sampler u_MetalSampler;
layout(set=1,binding=130) uniform sampler u_NormalSampler;
layout(set=1,binding=131) uniform sampler u_OcclusionSampler;
layout(set=1,binding=132) uniform sampler u_EmissiveSampler;
const float Pi = 3.14159265359;
layout(set=0,binding=0) uniform texture2D u_ShadowNear;
layout(set=0,binding=1) uniform texture2D u_ShadowFar;
layout(set=0,binding=2) uniform texture2D u_SnowMap;
layout(set=0,binding=128) uniform sampler u_ShadowSampler;
layout(set=0,binding=129) uniform sampler u_SnowSampler;

vec3 Hash33(vec3 p)
{
    p = vec3(dot(p, vec3(127.1, 311.7, 74.7)),
             dot(p, vec3(269.5, 183.3, 246.1)),
             dot(p, vec3(113.5, 271.9, 124.6)));
    return fract(sin(p) * 43758.5453123);
}

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
    bool front = gl_FrontFacing != (determinant(mat3(u_Object.Transform)) < 0.);
    if(u_Material.Coverage.z < .5 && !front)
        discard;
    vec4 base = texture(sampler2D(u_BaseColor,u_BaseSampler),v_TexCoord)*v_Color;
    if(u_Material.Coverage.y > .5 && base.a*u_Material.Coverage.x < u_Material.Parameters.z)
        discard;
    vec3 factor = u_Material.Coverage.w > .5 ? pow(u_Object.AlbedoRoughness.rgb,vec3(2.2)) : u_Object.AlbedoRoughness.rgb;
    vec3 albedo = factor*base.rgb;
    vec3 normal = normalize(v_Normal);
    if(u_Material.Parameters.w > .5)
    {
        vec3 tangent = normalize(v_Tangent.xyz-normal*dot(normal,v_Tangent.xyz));
        vec3 bitangent = v_Tangent.w*cross(normal,tangent);
        vec3 mapped = texture(sampler2D(u_NormalMap,u_NormalSampler),v_TexCoord).xyz*2.-1.;
        mapped.xy *= u_Material.Parameters.x;
        normal = normalize(mat3(tangent,bitangent,normal)*mapped);
    }
    if(u_Material.Coverage.z > .5 && u_Material.Coverage.w < .5 && !front)
        normal = -normal;

    vec4 metalRough = texture(sampler2D(u_MetalRough,u_MetalSampler),v_TexCoord);
    float roughness = max(.045,u_Object.AlbedoRoughness.a*metalRough.g);
    float metallic = u_Object.EmissionMetallic.a*metalRough.b;

    bool hasSnowMap = u_Camera.WindParameters.w > 0.5 && u_Camera.RenderParameters.z > 0.0;
    bool snowActive = hasSnowMap || u_Camera.SnowParameters.x > 0.001 ||
                      u_Camera.SnowParameters.y > 0.001 || u_Camera.SnowParameters.z > 0.001;
    float localSnowMask = 0.0;
    float iceFraction = 0.0;

    float trenchAO = 1.0;

    if (snowActive)
    {
        float slopeCover = smoothstep(0.35, 0.85, normal.y);
        if (u_Object.SurfaceFlags.x > 0.5 && hasSnowMap)
        {
            vec2 rawUV = clamp(v_WorldPosition.xz / u_Camera.RenderParameters.z + 0.5, vec2(0.0), vec2(1.0));
            vec2 snowSize = vec2(textureSize(sampler2D(u_SnowMap, u_SnowSampler), 0));
            vec2 snowUV = (rawUV * max(snowSize - 1.0, vec2(1.0)) + 0.5) / max(snowSize, vec2(1.0));
            vec4 snowState = texture(sampler2D(u_SnowMap, u_SnowSampler), snowUV);
            vec2 snowTexel = 1.0 / max(snowSize, vec2(1.0));
            float cellWorld = u_Camera.RenderParameters.z * snowTexel.x;
            float hC = snowState.r * u_Camera.RenderParameters.w;
            float hL = texture(sampler2D(u_SnowMap, u_SnowSampler), snowUV - vec2(snowTexel.x, 0.0)).r * u_Camera.RenderParameters.w;
            float hR = texture(sampler2D(u_SnowMap, u_SnowSampler), snowUV + vec2(snowTexel.x, 0.0)).r * u_Camera.RenderParameters.w;
            float hD = texture(sampler2D(u_SnowMap, u_SnowSampler), snowUV - vec2(0.0, snowTexel.y)).r * u_Camera.RenderParameters.w;
            float hU = texture(sampler2D(u_SnowMap, u_SnowSampler), snowUV + vec2(0.0, snowTexel.y)).r * u_Camera.RenderParameters.w;
            vec3 trackNormal = normalize(vec3(hL - hR, max(0.08, 2.0 * cellWorld / 0.65), hD - hU));
            normal = normalize(mix(normal, trackNormal, 0.72 * snowState.a));

            float coverage = snowState.a;
            float compression = clamp(snowState.g * 3.0, 0.0, 1.0);
            iceFraction = snowState.b;
            float bermBoost = clamp((hC - min(min(hL, hR), min(hD, hU))) * 4.5, 0.0, 0.25);
            trenchAO = mix(1.0, 0.76, compression * (1.0 - iceFraction));

            vec3 bareRock = mix(albedo * 0.45, vec3(0.18, 0.20, 0.24), 0.65);
            vec3 virginPowder = vec3(0.91, 0.95, 1.00) + vec3(0.04, 0.05, 0.06) * bermBoost;
            vec3 packedTrail = vec3(0.58, 0.72, 0.88);
            vec3 snowAlbedo = mix(virginPowder, packedTrail, compression * 0.78);
            vec3 iceAlbedo = vec3(0.46, 0.70, 0.89);

            float terrainSnowCover = coverage;
            albedo = mix(bareRock, snowAlbedo, clamp(terrainSnowCover, 0.0, 1.0));
            albedo = mix(albedo, iceAlbedo, iceFraction);

            float snowRough = mix(0.84, 0.46, compression);
            roughness = mix(roughness, snowRough, clamp(terrainSnowCover, 0.0, 1.0));
            roughness = mix(roughness, 0.12, iceFraction);
            metallic = mix(metallic, 0.0, max(clamp(terrainSnowCover, 0.0, 1.0), iceFraction));
            localSnowMask = clamp(terrainSnowCover * (1.0 - iceFraction * 0.6), 0.0, 1.0);
        }
        else if (u_Camera.SnowParameters.x > 0.001)
        {
            float windLen = length(u_Camera.WindParameters.xz);
            vec2 windDir = windLen > 0.05 ? u_Camera.WindParameters.xz / windLen : vec2(0.0);
            float windwardBias = windLen > 0.05 ? max(0.0, dot(normalize(vec3(normal.x, 0.0, normal.z) + vec3(0.0, 0.001, 0.0)).xz, -windDir)) : 0.0;
            float meshSnow = clamp((slopeCover + windwardBias * 0.18 * (1.0 - slopeCover)) * u_Camera.SnowParameters.x, 0.0, 1.0);
            albedo = mix(albedo, vec3(0.89, 0.94, 0.99), meshSnow);
            roughness = mix(roughness, 0.80, meshSnow);
            metallic = mix(metallic, 0.0, meshSnow);
            localSnowMask = meshSnow;
        }
        else if (u_Camera.SnowParameters.y > 0.001 || u_Camera.SnowParameters.z > 0.001)
        {
            localSnowMask = slopeCover * 0.85;
        }
    }

    vec3 viewDirection = normalize(u_Camera.CameraPosition.xyz-v_WorldPosition);
    vec3 lightDirection = normalize(u_Camera.SunDirection.xyz);
    vec3 halfwayDirection = normalize(lightDirection+viewDirection);
    float normalLight = max(dot(normal,lightDirection),0.);
    float normalView = max(dot(normal,viewDirection),.001);
    float normalHalfway = max(dot(normal,halfwayDirection),0.);
    float alphaSquared = pow(roughness,4.);
    float distribution = alphaSquared / (Pi*pow(normalHalfway*normalHalfway*(alphaSquared-1.)+1.,2.)+.00001);
    float geometryK = pow(roughness+1.,2.)/8.;
    float geometry = normalLight/(normalLight*(1.-geometryK)+geometryK)*normalView/(normalView*(1.-geometryK)+geometryK);
    vec3 reflection = mix(vec3(.04),albedo,metallic);
    vec3 fresnel = reflection+(1.-reflection)*pow(1.-max(dot(halfwayDirection,viewDirection),0.),5.);
    vec3 specular = distribution*geometry*fresnel/(4.*normalLight*normalView+.001);
    vec3 ambient = albedo*mix(vec3(.10,.15,.23),vec3(.32,.43,.59),normal.y*.5+.5) * trenchAO;
    ambient *= mix(1.,texture(sampler2D(u_Occlusion,u_OcclusionSampler),v_TexCoord).r,u_Material.Parameters.y);
    float sunVis = SunVisibility(v_WorldPosition,normal);
    vec3 color = ambient+((1.-fresnel)*(1.-metallic)*albedo/Pi+specular)*u_Camera.SunRadianceFog.rgb*normalLight*sunVis;

    if (localSnowMask > 0.001)
    {
        if (u_Camera.SnowParameters.z > 0.001)
        {
            float wrapNdotL = clamp((dot(normal, lightDirection) + 0.55) / 1.55, 0.0, 1.0);
            float forwardScatter = pow(clamp(dot(-viewDirection, normalize(lightDirection + normal * 0.35)), 0.0, 1.0), 3.0);
            vec3 sssTint = vec3(0.42, 0.68, 0.96) * wrapNdotL * 0.24 + vec3(0.60, 0.84, 1.0) * forwardScatter * 0.28;
            color += sssTint * u_Camera.SunRadianceFog.rgb * (0.35 + 0.65 * sunVis) * localSnowMask * u_Camera.SnowParameters.z;
        }
        if (u_Camera.SnowParameters.y > 0.001)
        {
            vec3 cell = floor(v_WorldPosition * 48.0);
            vec3 h = Hash33(cell);
            float activeCrystal = step(0.915, h.x);
            vec3 rnd = Hash33(cell + 19.7) * 2.0 - 1.0;
            vec3 crystalNormal = normalize(halfwayDirection * 0.55 + normal * 0.45 + rnd * 0.35);
            float align = max(dot(crystalNormal, halfwayDirection), 0.0);
            float glintTerm = pow(align, 220.0) * smoothstep(0.955, 0.996, align);
            float viewSpark = step(0.82, fract(h.y * 13.37 + dot(viewDirection, vec3(0.37, 0.53, 0.71))));
            color += vec3(3.4, 3.7, 4.3) * glintTerm * activeCrystal * viewSpark * sunVis * u_Camera.SunRadianceFog.rgb * localSnowMask * u_Camera.SnowParameters.y;
        }
    }

    color += u_Object.EmissionMetallic.rgb*texture(sampler2D(u_Emissive,u_EmissiveSampler),v_TexCoord).rgb;
    float fog = 1.-exp(-length(u_Camera.CameraPosition.xyz-v_WorldPosition)*u_Camera.SunRadianceFog.w);
    vec3 fogColor = mix(vec3(.25,.36,.5), vec3(.55,.65,.78), clamp(u_Camera.SnowParameters.w, 0.0, 1.0));
    color = mix(color,fogColor,fog);
    o_Color = vec4(color,1.);
}
