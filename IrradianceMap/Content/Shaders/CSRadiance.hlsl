//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#include "CSCubeMap.hlsli"

//--------------------------------------------------------------------------------------
// Constant buffer
//--------------------------------------------------------------------------------------
cbuffer cb
{
	float g_blend;
};

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
TextureCube					g_txSources[2];
RWTexture2DArray<float4>	g_rwDest;

//--------------------------------------------------------------------------------------
// Texture samplers
//--------------------------------------------------------------------------------------
SamplerState	g_smpLinear;

//--------------------------------------------------------------------------------------
// Compute shader
//--------------------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	const float3 tex = GetCubeTexcoord(DTid, g_rwDest);
	const float4 source1 = g_txSources[0].SampleLevel(g_smpLinear, tex, 0.0);
	const float4 source2 = g_txSources[1].SampleLevel(g_smpLinear, tex, 0.0);

	g_rwDest[DTid] = lerp(source1, source2, g_blend);
}
