//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SHMath.hlsli"

cbuffer cb
{
	uint g_order;
	uint g_pixelCount;
};

//--------------------------------------------------------------------------------------
// Buffers
//--------------------------------------------------------------------------------------
RWStructuredBuffer<float3> g_rwSHBuff;
RWStructuredBuffer<float> g_rwWeight;
StructuredBuffer<float3> g_roSHBuff;
StructuredBuffer<float> g_roWeight;

//--------------------------------------------------------------------------------------
// Compute shader
//--------------------------------------------------------------------------------------
[numthreads(32, 1, 1)]
void main(uint2 DTid : SV_DispatchThreadID, uint Gid : SV_GroupID)
{
	const uint n = g_order * g_order;
	float sumSH = 0.0, weight = 0.0;

	if (DTid.x < g_pixelCount)
	{
		float3 sh = g_roSHBuff[GetLocation(n, DTid)];
		sh = WaveActiveSum(sh);
		if (WaveIsFirstLane()) g_rwSHBuff[GetLocation(n, uint2(Gid, DTid.y))] = sh;

		if (DTid.y == 0)
		{
			float wt = g_roWeight[DTid.x];
			wt = WaveActiveSum(wt);
			if (WaveIsFirstLane()) g_rwWeight[Gid] = wt;
		}
	}
}
