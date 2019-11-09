//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#include "CubeMap.hlsli"

#define PI				3.141592654
#define MAX_LEVEL_COUNT	11

//--------------------------------------------------------------------------------------
// Structure
//--------------------------------------------------------------------------------------
struct PSIn
{
	float4 Pos : SV_POSITION;
	float2 Tex : TEXCOORD;
};

//--------------------------------------------------------------------------------------
// Constant buffer
//--------------------------------------------------------------------------------------
cbuffer cb
{
	float	g_mapSize;
	uint	g_numLevels;
	uint	g_level;
	uint	g_slice;
};

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
TextureCube<float3>	g_txCoarser;

//--------------------------------------------------------------------------------------
// Texture samplers
//--------------------------------------------------------------------------------------
SamplerState		g_smpLinear;

//--------------------------------------------------------------------------------------
// Compute shader
//--------------------------------------------------------------------------------------
float4 main(PSIn input) : SV_TARGET
{
	// Fetch the color of the current level and the resolved color at the coarser level
	const float3 tex = GetCubeTexcoord(g_slice, input.Tex);
	const float3 coarser = g_txCoarser.SampleLevel(g_smpLinear, tex, 0.0);

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
	float weight = numerator / denorminator;//saturate(numerator / denorminator);
#else
	float wsum = 0.0, weight = 0.0;
	for (uint i = g_level; i < g_numLevels; ++i)
	{
		//const float w = (1 << (i * 3)) * log(2.0) * a * sin((1 << i) * a);
		const float w = (1 << (i * 3)) * sin((1 << i) * a);
		weight = i == g_level ? w : weight;
		wsum += w;
	}

	weight = wsum > 0.0 ? weight / wsum : 1.0;
#endif

	return float4(coarser, 1.0 - weight);
}
