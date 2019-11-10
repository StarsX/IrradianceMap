//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#include "CubeMap.hlsli"
#include "MipCosine.hlsli"

//--------------------------------------------------------------------------------------
// Structure
//--------------------------------------------------------------------------------------
struct PSIn
{
	float4 Pos : SV_POSITION;
	float2 Tex : TEXCOORD;
};

//--------------------------------------------------------------------------------------
// Texture
//--------------------------------------------------------------------------------------
TextureCube<float3>	g_txCoarser;

//--------------------------------------------------------------------------------------
// Texture sampler
//--------------------------------------------------------------------------------------
SamplerState		g_smpLinear;

//--------------------------------------------------------------------------------------
// Pixel shader
//--------------------------------------------------------------------------------------
float4 main(PSIn input) : SV_TARGET
{
	// Fetch the color of the current level and the resolved color at the coarser level
	const float3 tex = GetCubeTexcoord(g_slice, input.Tex);
	const float3 coarser = g_txCoarser.SampleLevel(g_smpLinear, tex, 0.0);

	// Cosine-approximating Haar coefficients (weights of box filters)
	const float weight = MipCosineBlendWeight();

	return float4(coarser, 1.0 - weight);
}
