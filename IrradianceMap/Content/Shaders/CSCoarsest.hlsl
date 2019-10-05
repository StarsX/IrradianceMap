//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#include "CSCubeMap.hlsli"

#define PI	3.141592654

//--------------------------------------------------------------------------------------
// Constant buffer
//--------------------------------------------------------------------------------------
cbuffer cb
{
	float	g_mapSize;
	uint	g_level;
};

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
TextureCube					g_txSource;
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
	const float3 posNeighbors[] =
	{
		{ -1.0, 0.0, 0.5 },
		{ 1.0, 0.0, 0.5 },
		{ 0.0, -1.0, 0.5 },
		{ 0.0, 1.0, 0.5 }
	};

	float3 texNeighbors[4];
	[unroll] for (uint i = 0; i < 4; ++i)
		texNeighbors[i] = GetCubeTexcoord(DTid.z, posNeighbors[i]);

	const float3 tex = GetCubeTexcoord(DTid, g_rwDest);
	float4 src = g_txSource.SampleLevel(g_smpLinear, tex, 0.0);

	float4 neighbors[4];
	[unroll] for (i = 0; i < 4; ++i)
		neighbors[i] = g_txSource.SampleLevel(g_smpLinear, texNeighbors[i], 0.0);

	float4 coarser = src;
	coarser += (neighbors[0] + neighbors[1] + neighbors[2] + neighbors[3]) * 0.5;
	coarser /= 1.0 + 0.5 * 4.0;

	// Cosine-approximating Haar coefficients (weights of box filters)
	const float s = g_mapSize;
	const float a = PI / (s * 4.0);

	float wsum = 0.0, weight = 0.0;
	[unroll]
	for (i = g_level; i <= g_level + 1; ++i)
	{
		//const float w = (1 << (i * 3)) * log(2.0) * a * sin((1 << i) * a);
		const float w = (1 << (i * 3)) * sin((1 << i) * a);
		weight = i == g_level ? w : weight;
		wsum += w;
	}

	weight = wsum > 0.0 ? weight / wsum : 1.0;

	g_rwDest[DTid] = lerp(coarser, src, weight);
}
