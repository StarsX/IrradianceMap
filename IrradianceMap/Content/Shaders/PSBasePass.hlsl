//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#ifndef PI
#define PI 3.1415926535897
#endif

//--------------------------------------------------------------------------------------
// Structs
//--------------------------------------------------------------------------------------
struct PSIn
{
	float4	Pos		: SV_POSITION;
	float3	WSPos	: POSWORLD;
	float4	CSPos	: POSCURRENT;
	float4	TSPos 	: POSHISTORY;
	float3	Norm	: NORMAL;
};

struct PSOut
{
	min16float4 Color		: SV_TARGET0;
	min16float4 Velocity	: SV_TARGET1;
};

//--------------------------------------------------------------------------------------
// Constant buffer
//--------------------------------------------------------------------------------------
cbuffer cbPerFrame
{
	float3 g_eyePt;
	float g_glossy;
};

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
TextureCube<float3>	g_txRadiance	: register (t0);
#ifndef SH_ORDER
TextureCube<float3>	g_txIrradiance	: register (t1);
#endif

//--------------------------------------------------------------------------------------
// Sampler
//--------------------------------------------------------------------------------------
SamplerState g_sampler;

//--------------------------------------------------------------------------------------
// Schlick's approximation
//--------------------------------------------------------------------------------------
min16float Fresnel(min16float viewAmt, min16float specRef, uint expLevel = 2)
{
	const min16float fresFactor = 1.0 - viewAmt;

	min16float fresnel = fresFactor;
	[unroll] for (uint i = 0; i < expLevel; ++i) fresnel *= fresnel;
	fresnel = expLevel > 0 ? fresnel * fresFactor : fresnel;

	return lerp(fresnel, 1.0, specRef);
}

PSOut Shade(PSIn input, min16float3 norm, float3 irradiance)
{
	PSOut output;

	const min16float3 viewDir = min16float3(normalize(g_eyePt - input.WSPos));
	const min16float3 lightDir = reflect(-viewDir, norm);
	float3 radiance = g_txRadiance.SampleBias(g_sampler, lightDir, 2.0);

	const float2 csPos = input.CSPos.xy / input.CSPos.w;
	const float2 tsPos = input.TSPos.xy / input.TSPos.w;
	const min16float2 velocity = min16float2(csPos - tsPos) * min16float2(0.5, -0.5);

	// Specular
	const min16float roughness = 0.4;
	const min16float viewAmt = saturate(dot(norm, viewDir));
#if _NO_PREINTEGRATED_
	const min16float a = roughness * roughness;
	const min16float k = a * 0.5;
	const min16float fresnel = Fresnel(viewAmt, 0.04);
	min16float visInv = viewAmt * (1 - k) + k;
	visInv *= visInv * 4.0;
	radiance *= fresnel * viewAmt * 4.0 * viewAmt / visInv;
#else
	const min16float4 c0 = { -1.0, -0.0275, -0.572, 0.022 };
	const min16float4 c1 = { 1.0, 0.0425, 1.04, -0.04 };
	const min16float4 r = roughness * c0 + c1;
	const min16float a004 = min(r.x * r.x, exp2(-9.28 * viewAmt)) * r.x + r.y;
	min16float2 ambient = min16float2(-1.04, 1.04) * a004 + r.zw;
	radiance *= 0.04 * ambient.x + ambient.y;
#endif

	//output.Color = min16float4(norm * 0.5 + 0.5, 1.0);
	output.Color = min16float4(irradiance / PI + radiance * g_glossy, 1.0);
	output.Velocity = min16float4(velocity, 0.0.xx);

	return output;
}

//--------------------------------------------------------------------------------------
// Base geometry-buffer pass
//--------------------------------------------------------------------------------------
#ifndef SH_ORDER
PSOut main(PSIn input)
{
	const min16float3 norm = min16float3(normalize(input.Norm));
	//float3 irradiance = g_txIrradiance.Sample(g_sampler, input.Norm);
	float3 irradiance = g_txIrradiance.SampleLevel(g_sampler, input.Norm, 0.0);
	irradiance *= PI;

	return Shade(input, norm, irradiance);
}
#endif
