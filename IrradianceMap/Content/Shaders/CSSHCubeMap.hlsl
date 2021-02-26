//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SHMath.hlsli"

cbuffer cb
{
	uint g_order;
};

Texture2DArray<float3> g_txCubeMap;
RWStructuredBuffer<float3> g_rwSHBuff;
RWStructuredBuffer<float> g_rwDiffSolid;

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
	uint3 mapSize;
	g_txCubeMap.GetDimensions(mapSize.x, mapSize.y, mapSize.z);

	const float size = mapSize.x;
	const float picSize = 1.0 / size;

	// index from [0,W-1], f(0) maps to -1 + 1/W, f(W-1) maps to 1 - 1/w
	// linear function x*S +B, 1st constraint means B is (-1+1/W), plug into
	// second and solve for S: S = 2*(1-1/W)/(W-1). The old code that did 
	// this was incorrect - but only for computing the differential solid
	// angle, where the final value was 1.0 instead of 1-1/w...
	const float fB = -1.0 + 1.0 / size;
	const float fS = (mapSize.x > 1) ? (2.0 * (1.0 - 1.0 / size) / (size - 1.0)) : 0.0;

	const float x = DTid.x;
	const float y = DTid.y;
	const float2 uv = float2(x, y) * fS + fB;

	float3 dir;
	const uint face = DTid.z;
	switch (face)
	{
	case 0: // Positive X
		dir.z = 1.0 - (2.0 * x + 1.0) * picSize;
		dir.y = 1.0 - (2.0 * y + 1.0) * picSize;
		dir.x = 1.0;
		break;
	case 1: // Negative X
		dir.z = -1.0 + (2.0 * x + 1.0) * picSize;
		dir.y = 1.0 - (2.0f * y + 1.0) * picSize;
		dir.x = -1.0;
		break;
	case 2: // Positive Y
		dir.z = -1.0f + (2.0f * float(y) + 1.0f) * picSize;
		dir.y = 1.0f;
		dir.x = -1.0f + (2.0f * float(x) + 1.0f) * picSize;
		break;
	case 3: // Negative Y
		dir.z = 1.0f - (2.0f * float(y) + 1.0f) * picSize;
		dir.y = -1.0f;
		dir.x = -1.0f + (2.0f * float(x) + 1.0f) * picSize;
		break;
	case 4: // Positive Z
		dir.z = 1.0f;
		dir.y = 1.0f - (2.0f * float(y) + 1.0f) * picSize;
		dir.x = -1.0f + (2.0f * float(x) + 1.0f) * picSize;
		break;
	case 5: // Negative Z
		dir.z = -1.0f;
		dir.y = 1.0f - (2.0f * float(y) + 1.0f) * picSize;
		dir.x = 1.0f - (2.0f * float(x) + 1.0f) * picSize;
		break;
	default:
		dir.x = dir.y = dir.z = 0.0;
		break;
	}

	dir = normalize(dir);

	const float diff = 1.0 + dot(uv, uv);
	const float diffSolid = 4.0 / (diff * sqrt(diff));

	//const uint p = DTid.z * mapSize.x * mapSize.y + DTid.y * mapSize.x + DTid.x;
	const uint p = dot(DTid, uint3(1, mapSize.x, mapSize.x * mapSize.y));
	g_rwDiffSolid[p] = diffSolid;

	float shBuff[SH_MAX_COEFF];
	float3 shBuffB[SH_MAX_COEFF];
	SHEvalDirection(shBuff, g_order, dir);

	const float3 color = g_txCubeMap[DTid];

	const uint n = g_order * g_order;
	SHScale(shBuffB, g_order, shBuff, color * diffSolid);
	for (uint i = 0; i < n; ++i)
		g_rwSHBuff[GetLocation(n, uint2(p, i))] = shBuffB[i];
}
