// Copies the game's motion vectors into texMotionVectors for third-party shaders.
// Put it at the top of the list and turn off any motion estimation effect.
// The vectors are zero in menus and while DLSS is off.

#include "ACVDLSS.fxh"

// The shared texture other effects declare the same way.
texture texMotionVectors { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG16F; };

float2 copy_motion(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return ACVDLSS::motion(uv);
}

technique ACVDLSS_MotionVectors <
    ui_label = "ACV DLSS Motion Vectors (for third-party shaders)";
    ui_tooltip = "Changes nothing on screen by itself. It gives your other shaders that read\n"
                 "texMotionVectors the game's own motion vectors, in place of a motion estimation shader.\n"
                 "Keep it at the top of the list.";
>
{
    pass
    {
        VertexShader = PostProcessVS;
        PixelShader = copy_motion;
        RenderTarget = texMotionVectors;
    }
}
