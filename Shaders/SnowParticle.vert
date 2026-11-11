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

layout(push_constant) uniform ParticlePush
{
	vec4 VolumeCenter;
	vec4 Params; // x = aspect, y = intensity, z = boxHeight, w = particleCount
}
u_Push;

layout(location = 0) out vec2 v_LocalUV;
layout(location = 1) out float v_EdgeFade;
layout(location = 2) out float v_Seed;
layout(location = 3) out float v_Stretch;

vec3 Hash3(uint index)
{
	uint x = index * 747796405u + 2891336453u;
	uint y = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
	uint z = (y ^ (y >> 22u)) * 1597334677u;
	uint w = (z ^ (z >> 17u)) * 2246822519u;
	return vec3(float(y & 0x00FFFFFFu), float(z & 0x00FFFFFFu), float(w & 0x00FFFFFFu)) / 16777216.0;
}

void main()
{
	const vec2 quadCorners[6] = vec2[](
		vec2(-1.0, -1.0),
		vec2(1.0, -1.0),
		vec2(1.0, 1.0),
		vec2(-1.0, -1.0),
		vec2(1.0, 1.0),
		vec2(-1.0, 1.0)
	);

	vec2 local = quadCorners[gl_VertexIndex];
	vec3 seed = Hash3(uint(gl_InstanceIndex));

	float boxXZ = max(u_Push.VolumeCenter.w, 8.0);
	float boxY = max(u_Push.Params.z, 4.0);
	vec3 boxDims = vec3(boxXZ, boxY, boxXZ);

	float time = u_Camera.RenderParameters.y;
	vec3 wind = u_Camera.WindParameters.xyz;
	float fallSpeed = 2.8 + seed.z * 2.1;
	vec3 vel = wind * (0.8 + 0.4 * seed.y) + vec3(0.0, -fallSpeed, 0.0);

	vec3 flutter = vec3(
		sin(time * 1.9 + seed.x * 6.2831853 + seed.z * 3.1) * 0.45,
		0.0,
		cos(time * 1.6 + seed.y * 6.2831853 + seed.x * 4.2) * 0.45
	);

	vec3 rel = fract(seed + (vel * time + flutter) / boxDims) - 0.5;
	vec3 center = u_Push.VolumeCenter.xyz + vec3(0.0, boxY * 0.35, 0.0) + rel * boxDims;

	vec3 boundaryDist = 0.5 - abs(rel);
	float edgeFade = clamp(min(min(boundaryDist.x, boundaryDist.z) * 10.0, boundaryDist.y * 8.0), 0.0, 1.0);

	vec4 clipCenter = u_Camera.ViewProjection * vec4(center, 1.0);
	if (clipCenter.w <= 0.1 || edgeFade <= 0.001)
	{
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
		v_LocalUV = local;
		v_EdgeFade = 0.0;
		v_Seed = seed.x;
		v_Stretch = 1.0;
		return;
	}

	float velLen = length(vel);
	vec3 velDir = velLen > 0.001 ? vel / velLen : vec3(0.0, -1.0, 0.0);
	float streakLen = (0.08 + 0.10 * seed.x) * (1.0 + clamp(length(wind.xz) * 0.28, 0.0, 2.5));
	vec4 clipTip = u_Camera.ViewProjection * vec4(center + velDir * streakLen, 1.0);

	vec2 ndc0 = clipCenter.xy / clipCenter.w;
	vec2 ndc1 = clipTip.xy / max(clipTip.w, 0.1);
	float aspect = max(u_Push.Params.x, 0.1);
	vec2 deltaIso = vec2((ndc1.x - ndc0.x) * aspect, ndc1.y - ndc0.y);

	vec2 forwardIso = length(deltaIso) > 1e-4 ? normalize(deltaIso) : vec2(0.0, -1.0);
	vec2 rightIso = vec2(-forwardIso.y, forwardIso.x);

	float stretch = clamp(1.0 + length(deltaIso) * 18.0, 1.0, 3.0);
	float baseRadius = (0.0085 + 0.0115 * seed.y) / max(clipCenter.w * 0.045, 0.55);

	vec2 offsetIso = rightIso * (local.x * baseRadius) + forwardIso * (local.y * baseRadius * stretch);
	vec2 offsetNDC = vec2(offsetIso.x / aspect, offsetIso.y);

	gl_Position = clipCenter + vec4(offsetNDC * clipCenter.w, 0.0, 0.0);
	v_LocalUV = local;
	v_EdgeFade = edgeFade;
	v_Seed = fract(seed.x * 7.13 + seed.y * 13.7 + seed.z * 19.3);
	v_Stretch = stretch;
}
