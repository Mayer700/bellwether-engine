#version 450
layout(location=0) in vec2 v_TexCoord;
layout(location=0) out vec4 o_Color;
layout(set=0,binding=0) uniform texture2D u_HDR;
layout(set=0,binding=128) uniform sampler u_LinearSampler;
layout(push_constant,std430) uniform PostSettings
{
    vec4 Parameters;
} u_Settings;
vec3 Tonemap(vec3 value)
{
    return clamp((value*(2.51*value+.03))/(value*(2.43*value+.59)+.14),0.,1.);
}
void main()
{
    vec3 linearColor = texture(sampler2D(u_HDR,u_LinearSampler),v_TexCoord).rgb;
    o_Color = vec4(pow(Tonemap(linearColor*u_Settings.Parameters.x),vec3(1./2.2)),1.);
}
