//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#include "CSCubeMap.hlsli"

#define PI				3.141592654
#define MAX_LEVEL_COUNT	12

//--------------------------------------------------------------------------------------
// Constant buffer
//--------------------------------------------------------------------------------------
cbuffer cb
{
	float	g_mapSize;
	uint	g_numLevels;
	uint	g_level;
};

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
TextureCube					g_txSource;
TextureCube					g_txCoarser;
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
	// Fetch the color of the current level and the resolved color at the coarser level
	const float3 tex = GetCubeTexcoord(DTid, g_rwDest);
	const float4 src = g_txSource.SampleLevel(g_smpLinear, tex, 0.0);
	const float4 coarser = g_txCoarser.SampleLevel(g_smpLinear, tex, 0.0);

	// Cosine-approximating Haar coefficients (weights of box filters)
#if 1
//#ifdef _PREINTEGRATED_
	// Compute deviation
	const float sigma = 64.0;
	const float sigma2 = sigma * sigma;

	const float c = 2.0 * PI * sigma2;
	//const float numerator = pow(16.0, g_level) * log(4.0);
	//const float denorminator = c * (pow(4.0, g_level) + c);
	//const float numerator = pow(2.0, g_level * 4.0) * log(4.0);
	//const float denorminator = c * (pow(2.0, g_level * 2.0) + c);
	//const float numerator = (1 << (g_level * 4)) * log(4.0);
	//const float denorminator = c * ((1 << (g_level * 2)) + c);
	const float numerator = (1 << (g_level << 2)) * log(4.0);
	const float denorminator = c * ((1 << (g_level << 1)) + c);
	const float weight = saturate(numerator / denorminator);
#else
	const float c = PI / (g_mapSize * 4.0);

	float wsum = 0.0, weight = 0.0;
	for (uint i = g_level; i < g_numLevels; ++i)
	{
		const float w = (1 << (i * 3)) * log(2.0) * c * sin((1 << i) * c);
		weight = i == g_level ? w : weight;
		wsum += w;
	}

	weight = wsum > 0.0 ? weight / wsum : 1.0;
#endif

	g_rwDest[DTid] = lerp(coarser, src, weight);
}
