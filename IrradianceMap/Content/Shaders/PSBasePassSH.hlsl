//--------------------------------------------------------------------------------------
// By XU, Tianchen
//--------------------------------------------------------------------------------------

#define PI	3.141592654

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
cbuffer cbPerFrame	: register (b0)
{
	float3 g_eyePt;
	float g_glossy;
};

cbuffer cbSH		: register (b1)
{
	float3 g_coeffSH[9];
};

//--------------------------------------------------------------------------------------
// Textures
//--------------------------------------------------------------------------------------
TextureCube<float3>	g_txRadiance;

//--------------------------------------------------------------------------------------
// Samplers
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

//--------------------------------------------------------------------------------------
// Spherical harmonics
//--------------------------------------------------------------------------------------
float3 EvaluateSHIrradiance(float3 norm)
{
	const float c1 = 0.42904276540489171563379376569857;    // 4 * A2.Y22 = 1/4 * sqrt(15.PI)
	const float c2 = 0.51166335397324424423977581244463;    // 0.5 * A1.Y10 = 1/2 * sqrt(PI/3)
	const float c3 = 0.24770795610037568833406429782001;    // A2.Y20 = 1/16 * sqrt(5.PI)
	const float c4 = 0.88622692545275801364908374167057;    // A0.Y00 = 1/2 * sqrt(PI)

	const float x = norm.x;
	const float y = -norm.y;
	const float z = norm.z;

	const float3 irradiance = max(0.0,
		(c1 * (x * x - y * y)) * g_coeffSH[8]												// c1.L22.(x²-y²)
		+ (c3 * (3.0 * z * z - 1)) * g_coeffSH[6]											// c3.L20.(3.z² - 1)
		+ c4 * g_coeffSH[0]																	// c4.L00 
		+ 2.0 * c1 * (g_coeffSH[4] * x * y + g_coeffSH[7] * x * z + g_coeffSH[5] * y * z)	// 2.c1.(L2-2.xy + L21.xz + L2-1.yz)
		+ 2.0 * c2 * (g_coeffSH[3] * x + g_coeffSH[1] * y + g_coeffSH[2] * z));				// 2.c2.(L11.x + L1-1.y + L10.z)

	return irradiance / PI;
}

//--------------------------------------------------------------------------------------
// Base geometry-buffer pass
//--------------------------------------------------------------------------------------
PSOut main(PSIn input)
{
	PSOut output;

	const min16float3 norm = min16float3(normalize(input.Norm));
	float3 irradiance = EvaluateSHIrradiance(norm);//ComputeSHIrradiance(norm);

	const min16float3 viewDir = min16float3(normalize(g_eyePt - input.WSPos));
	const min16float3 lightDir = reflect(-viewDir, norm);
	//float3 radiance = g_txRadiance.SampleBias(g_sampler, lightDir, 2.0);
	float3 radiance = g_txRadiance.SampleLevel(g_sampler, lightDir, 2.0);

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

	output.Color = min16float4(irradiance + radiance * g_glossy, 1.0);
	output.Velocity = min16float4(velocity, 0.0.xx);

	return output;
}
