#version 450
layout(location=0) out vec2 v_TexCoord;
void main()
{
    vec2 position = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2. - 1.;
    v_TexCoord = vec2(position.x * .5 + .5, .5 - position.y * .5);
    gl_Position = vec4(position,0.,1.);
}
