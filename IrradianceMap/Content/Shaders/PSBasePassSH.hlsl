//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#define SH_ORDER 3
#include "SHIrradiance.hlsli"
#include "PSBasePass.hlsl"

//--------------------------------------------------------------------------------------
// Buffer
//--------------------------------------------------------------------------------------
StructuredBuffer<float3> g_roSHBuff;

//--------------------------------------------------------------------------------------
// Base geometry-buffer pass
//--------------------------------------------------------------------------------------
PSOut main(PSIn input)
{
	const min16float3 norm = min16float3(normalize(input.Norm));
	float3 irradiance = EvaluateSHIrradiance(g_roSHBuff, norm);

	return Shade(input, norm, irradiance);
}
