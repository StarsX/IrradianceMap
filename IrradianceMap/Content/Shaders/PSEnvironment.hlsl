//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Definitions
//--------------------------------------------------------------------------------------		
#define ENVCUBE_RADIUS	(768.0 / 1.414)

//--------------------------------------------------------------------------------------
// Structure
//--------------------------------------------------------------------------------------
struct PSIn
{
	float4 Pos : SV_POSITION;
	float2 UV : TEXCOORD;
};

//--------------------------------------------------------------------------------------
// Constant buffer
//--------------------------------------------------------------------------------------
cbuffer cbPerFrame
{
	float3 g_eyePt;
	matrix g_screenToWorld;
};

//--------------------------------------------------------------------------------------
// Constant
//--------------------------------------------------------------------------------------		
static const float g_2EnvBoxRadSq = 2.0 * ENVCUBE_RADIUS * ENVCUBE_RADIUS;

//--------------------------------------------------------------------------------------
// Texture
//--------------------------------------------------------------------------------------
TextureCube<float3> g_txEnv;

//--------------------------------------------------------------------------------------
// Texture sampler
//--------------------------------------------------------------------------------------
SamplerState g_smpLinear;

//--------------------------------------------------------------------------------------
// Pixel shader that maps the cubic sky texture into screen space
//--------------------------------------------------------------------------------------
min16float4 main(PSIn input) : SV_TARGET
{
	float2 xy = input.UV * 2.0 - 1.0;
	xy.y = -xy.y;

	// Calculate cube mapping
	float4 pos = mul(float4(xy, 1.0.xx), g_screenToWorld);
	const min16float3 viewDir = min16float3(normalize(g_eyePt - pos.xyz / pos.w));

	const float3 start = g_eyePt;
	const float3 rayDir = -viewDir;

#if _FINITE_SIZE_
	// Calculate cube mapping
	const float proj = dot(start, rayDir);
	const float startSq = dot(start, start);
	const float dist = sqrt(proj * proj - startSq + g_2EnvBoxRadSq) - proj;	// Solve equation
	pos.xyz = start + dist * rayDir;

	return min16float4(g_txEnv.Sample(g_smpLinear, pos.zyx), 0.0);
#else
	return min16float4(g_txEnv.SampleLevel(g_smpLinear, rayDir, 0.0), 0.0);
#endif
}
