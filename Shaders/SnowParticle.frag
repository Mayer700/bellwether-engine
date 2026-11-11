#version 450

layout(std140, set = 0, binding = 256) uniform CameraData
{
	mat4 ViewProjection;
	mat4 LightViewProjection[2];
	vec4 CameraPosition;
	vec4 SunDirection;
	vec4 SunRadianceFog;
	vec4 RenderParameters;
	vec4 SnowParameters;
	vec4 WindParameters;
}
u_Camera;

layout(location = 0) in vec2 v_LocalUV;
layout(location = 1) in float v_EdgeFade;
layout(location = 2) in float v_Seed;
layout(location = 3) in float v_Stretch;

layout(location = 0) out vec4 o_Color;

void main()
{
	float radial = length(vec2(v_LocalUV.x, v_LocalUV.y / max(1.0, v_Stretch * 0.75)));
	float softCore = smoothstep(1.0, 0.25, radial);
	float intensity = clamp(u_Camera.SnowParameters.w, 0.0, 2.0);
	float alpha = softCore * v_EdgeFade * clamp(0.55 + 0.35 * intensity, 0.0, 0.94);
	if (alpha < 0.01)
		discard;

	float sparkleBoost = 1.0 + step(0.88, v_Seed) * (0.65 + 0.85 * clamp(u_Camera.SnowParameters.y, 0.0, 2.0));
	vec3 skyFill = vec3(0.92, 0.98, 1.12) * 0.95;
	vec3 sunScatter = max(u_Camera.SunRadianceFog.rgb, vec3(0.0)) * 0.32;
	vec3 color = (skyFill + sunScatter) * sparkleBoost;

	o_Color = vec4(color, alpha);
}
