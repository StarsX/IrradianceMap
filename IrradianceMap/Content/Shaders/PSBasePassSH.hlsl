//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "SHMath.hlsli"
#include "PSBasePass.hlsl"

#define PI	3.141592654

//--------------------------------------------------------------------------------------
// Buffer
//--------------------------------------------------------------------------------------
StructuredBuffer<float3> g_roSHBuff;

//--------------------------------------------------------------------------------------
// Spherical harmonics
//--------------------------------------------------------------------------------------
float3 EvaluateSHIrradiance(float3 norm)
{
	const float c1 = 0.42904276540489171563379376569857;	// 4 * A2.Y22 = 1/4 * sqrt(15.PI)
	const float c2 = 0.51166335397324424423977581244463;	// 0.5 * A1.Y10 = 1/2 * sqrt(PI/3)
	const float c3 = 0.24770795610037568833406429782001;	// A2.Y20 = 1/16 * sqrt(5.PI)
	const float c4 = 0.88622692545275801364908374167057;	// A0.Y00 = 1/2 * sqrt(PI)

	const float x = -norm.x;
	const float y = -norm.y;
	const float z = norm.z;

	const float3 irradiance = max(0.0,
		(c1 * (x * x - y * y)) * g_roSHBuff[8]													// c1.L22.(x²-y²)
		+ (c3 * (3.0 * z * z - 1.0)) * g_roSHBuff[6]											// c3.L20.(3.z² - 1)
		+ c4 * g_roSHBuff[0]																	// c4.L00 
		+ 2.0 * c1 * (g_roSHBuff[4] * x * y + g_roSHBuff[7] * x * z + g_roSHBuff[5] * y * z)	// 2.c1.(L2-2.xy + L21.xz + L2-1.yz)
		+ 2.0 * c2 * (g_roSHBuff[3] * x + g_roSHBuff[1] * y + g_roSHBuff[2] * z));				// 2.c2.(L11.x + L1-1.y + L10.z)

	return irradiance / PI;
}

//--------------------------------------------------------------------------------------
// Base geometry-buffer pass
//--------------------------------------------------------------------------------------
PSOut main(PSIn input)
{
	const min16float3 norm = min16float3(normalize(input.Norm));
	float3 irradiance = EvaluateSHIrradiance(norm);

	return Shade(input, norm, irradiance);
}
