#version 450
layout(location=0) in vec2 v_TexCoord;
layout(location=1) in float v_Alpha;
layout(location=2) flat in float v_Mirrored;
layout(set=1,binding=256,std140) uniform MaterialData { vec4 Parameters; vec4 Coverage; } u_Material;
layout(set=1,binding=0) uniform texture2D u_BaseColor;
layout(set=1,binding=128) uniform sampler u_BaseSampler;
void main()
{
    bool front = gl_FrontFacing != (v_Mirrored > .5);
    if(u_Material.Coverage.z < .5 && !front)
        discard;
    float alpha = texture(sampler2D(u_BaseColor,u_BaseSampler),v_TexCoord).a*v_Alpha*u_Material.Coverage.x;
    if(u_Material.Coverage.y > .5 && alpha < u_Material.Parameters.z)
        discard;
}
