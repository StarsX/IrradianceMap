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
	const float s = g_mapSize;
	const float a = PI / (s * 4.0);

#ifdef _PREINTEGRATED_
	const float pi2 = PI * PI;
	const float pi3 = pi2 * PI;
	const float s2 = s * s;
	const float s3 = s2 * s;

	float2 sinCos;
	sincos((1 << g_level) * a, sinCos.x, sinCos.y);
	const float numerator = (1 << (g_level * 3)) * pi3 * sinCos.x * log(2.0);
	const float denormC = (128.0 * s3 - (1 << (g_level * 2 + 4)) * s * pi2) * sinCos.y;
	const float denormS = (1 << (g_level + 5)) * s2 * PI * sinCos.x;
	const float denorminator = denormC - denormS + 64.0 * s3 * PI;
	const float weight = numerator / denorminator;//saturate(numerator / denorminator);
#else
	float wsum = 0.0, weight = 0.0;
	for (uint i = g_level; i < g_numLevels; ++i)
	{
		const float w = (1 << (i * 3)) * log(2.0) * a * sin((1 << i) * a);
		weight = i == g_level ? w : weight;
		wsum += w;
	}

	weight = wsum > 0.0 ? weight / wsum : 1.0;
#endif

	g_rwDest[DTid] = lerp(coarser, src, weight);
}
