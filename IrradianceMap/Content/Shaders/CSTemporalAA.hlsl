//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Definitions
//--------------------------------------------------------------------------------------
#ifdef _FORCE_FP32_
typedef float		HALF;
typedef float2		HALF2;
typedef float3		HALF3;
typedef float4		HALF4;
#else
typedef min16float	HALF;
typedef min16float2	HALF2;
typedef min16float3	HALF3;
typedef min16float4	HALF4;
#endif

#define _VARIANCE_AABB_	1
#define _USE_YCOCG_		1

#define NUM_NEIGHBORS	8
#define NUM_SAMPLES		(NUM_NEIGHBORS + 1)
#define NUM_NEIGHBORS_H	4

#ifndef ALPHA_BOUND
#define ALPHA_BOUND		0.5
#endif

// Use YCoCg dependently
#if	_USE_YCOCG_
#define GET_LUMA4(v)	((v).x)
#else
#define GET_LUMA4(v)	dot(v, g_luma4Base)
#endif

//--------------------------------------------------------------------------------------
// Constants
//--------------------------------------------------------------------------------------
static const uint g_historyBits = 4;
static const uint g_historyMask = (1 << g_historyBits) - 1;
static const float g_historyMax = g_historyMask;

static const HALF3 g_luma4Base = { 1.0, 2.0, 1.0 };
static const int2 g_texOffsets[] =
{
	int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1),
	int2(-1, -1), int2(1, -1), int2(1, 1), int2(-1, 1)
};

//--------------------------------------------------------------------------------------
// Texture and buffers
//--------------------------------------------------------------------------------------
#ifdef _R11G11B10_
RWTexture2D<float3>	g_rwRenderTarget;
RWTexture2D<float>	g_rwMetaData;
Texture2D<float3>	g_txCurrent;
Texture2D<float3>	g_txHistory;
Texture2D<float2>	g_txVelocity;
Texture2D<float>	g_txMasks;
Texture2D<float>	g_txHistMeta;
#else
RWTexture2D<float4>	g_rwRenderTarget;
Texture2D			g_txCurrent;
Texture2D			g_txHistory;
Texture2D<float2>	g_txVelocity;
#endif

//--------------------------------------------------------------------------------------
// Sampler
//--------------------------------------------------------------------------------------
SamplerState g_smpLinear;

//--------------------------------------------------------------------------------------
// RGB to YCgCo
//--------------------------------------------------------------------------------------
HALF3 rgbToYCoCg(HALF3 rgb)
{
	const HALF y = dot(rgb, HALF3(1.0, 2.0, 1.0));
	const HALF co = dot(rgb, HALF3(2.0, 0.0, -2.0));
	const HALF cg = dot(rgb, HALF3(-1.0, 2.0, -1.0));

	return HALF3(y, co, cg);
}

//--------------------------------------------------------------------------------------
// YCgCo to RGB
//--------------------------------------------------------------------------------------
HALF3 yCoCgToRGB(HALF3 yCoCg)
{
	const HALF y = yCoCg.x * 0.25;
	const HALF co = yCoCg.y * 0.25;
	const HALF cg = yCoCg.z * 0.25;

	const HALF r = y + co - cg;
	const HALF g = y + cg;
	const HALF b = y - co - cg;

	return HALF3(r, g, b);
}

//--------------------------------------------------------------------------------------
// A fast invertible tone map that preserves color (Reinhard)
//--------------------------------------------------------------------------------------
HALF3 TM(float3 hdr)
{
	HALF3 color = HALF3(hdr);
#if _USE_YCOCG_
	color = rgbToYCoCg(color);
#endif

	return color / (4.0 + GET_LUMA4(color));
}

//--------------------------------------------------------------------------------------
// Inverse of preceding function
//--------------------------------------------------------------------------------------
HALF3 ITM(HALF3 color)
{
	color *= 4.0 / (1.0 - GET_LUMA4(color));

#if _USE_YCOCG_
	return yCoCgToRGB(color);
#else
	return color;
#endif
}

//--------------------------------------------------------------------------------------
// Maxinum velocity of 3x3
//--------------------------------------------------------------------------------------
HALF4 VelocityMax(int2 pos)
{
	const float2 velocity = g_txVelocity[pos];

	float2 velocities[NUM_NEIGHBORS_H];
	[unroll]
	for (uint i = 0; i < NUM_NEIGHBORS_H; ++i)
		velocities[i] = g_txVelocity[pos + g_texOffsets[i + NUM_NEIGHBORS_H]];

	HALF4 velocityMax = HALF2(velocity).xyxy;
	HALF speedSq = dot(velocityMax.xy, velocityMax.xy);
	//[unroll]
	for (i = 0; i < NUM_NEIGHBORS_H; ++i)
	{
		const HALF2 neighbor = HALF2(velocities[i]);
#if 0
		velocityMax.xy = max(neighbor, velocityMax.xy);
#else
		const HALF speedSqN = dot(neighbor, neighbor);
		if (speedSqN > speedSq)
		{
			velocityMax.xy = neighbor;
			speedSq = speedSqN;
		}
#endif
	}

	return velocityMax;
}

//--------------------------------------------------------------------------------------
// Minimum and maxinum of the neighbor samples, returning Gaussian blurred color
//--------------------------------------------------------------------------------------
HALF4 NeighborMinMax(out HALF4 neighborMin, out HALF4 neighborMax,
	HALF4 current, int2 pos, HALF gamma = 1.0)
{
	static const HALF weights[] =
	{
		0.5, 0.5, 0.5, 0.5,
		0.25, 0.25, 0.25, 0.25
	};

	float4 neighbors[NUM_NEIGHBORS];
	[unroll]
	for (uint i = 0; i < NUM_NEIGHBORS; ++i)
		neighbors[i] = g_txCurrent[pos + g_texOffsets[i]];

	HALF3 mu = current.xyz;
#ifndef _ALPHA_AS_ID_
	current.w = current.w < ALPHA_BOUND ? 0.0 : 1.0;
#elif _VARIANCE_AABB_ && defined(_DENOISER_)
	const HALF alpha = current.w;
#endif

#if	_VARIANCE_AABB_
#define	m1	mu
	HALF3 m2 = m1 * m1;
#else
	neighborMin.xyz = neighborMax.xyz = mu;
#endif

	//[unroll]
	for (i = 0; i < NUM_NEIGHBORS; ++i)
	{
		HALF4 neighbor;
		neighbor.xyz = TM(neighbors[i].xyz);
#ifndef _ALPHA_AS_ID_
		neighbor.w = neighbors[i].w < ALPHA_BOUND ? 0.0 : 1.0;
#endif
		current += neighbor * weights[i];

#if	_VARIANCE_AABB_
		m1 += neighbor.xyz;
		m2 += neighbor.xyz * neighbor.xyz;
#else
		neighborMin.xyz = min(neighbor, neighborMin.xyz);
		neighborMax.xyz = max(neighbor, neighborMax.xyz);
#endif
	}

	current /= 4.0;

#if	_VARIANCE_AABB_
#if defined(_DENOISER_) && defined(_ALPHA_AS_ID_)
	gamma = abs(alpha - current.w) < 1.0 / 255.0 ? gamma : 1.0;
#endif
	mu /= NUM_SAMPLES;
	const HALF3 sigma = sqrt(abs(m2 / NUM_SAMPLES - mu * mu));
	const HALF3 gsigma = gamma * sigma;
	neighborMin.xyz = mu - gsigma;
	neighborMax.xyz = mu + gsigma;
	neighborMin.xyz = min(neighborMin.xyz, current.xyz);
	neighborMax.xyz = max(neighborMax.xyz, current.xyz);
	neighborMin.w = GET_LUMA4(mu - sigma);
	neighborMax.w = GET_LUMA4(mu + sigma);
#else
	neighborMin.w = GET_LUMA4(neighborMin.xyz);
	neighborMax.w = GET_LUMA4(neighborMax.xyz);
#endif

	return current;
}

//--------------------------------------------------------------------------------------
// Clip color
//--------------------------------------------------------------------------------------
HALF3 clipColor(HALF3 color, HALF3 minColor, HALF3 maxColor)
{
	const HALF3 cent = 0.5 * (maxColor + minColor);
	const HALF3 dist = 0.5 * (maxColor - minColor);

	const HALF3 disp = color - cent;
	const HALF3 dir = abs(disp / dist);
	const HALF maxComp = max(dir.x, max(dir.y, dir.z));

	if (maxComp > 1.0) return cent + disp / maxComp;
	else return color;
}

[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
	float2 texSize;
	g_txHistory.GetDimensions(texSize.x, texSize.y);
	const float2 uv = (DTid + 0.5) / texSize;

	// Load G-buffers
	const float4 current = g_txCurrent[DTid];
	const HALF4 velocity = VelocityMax(DTid);
	const float2 uvBack = uv - velocity.xy;
	float4 history = g_txHistory.SampleLevel(g_smpLinear, uvBack, 0);

	// Speed to history blur
	const float2 historyBlurAmp = 4.0 * texSize;
	const HALF2 historyBlurs = HALF2(abs(velocity.xy) * historyBlurAmp);
	HALF curHistoryBlur = historyBlurs.x + historyBlurs.y;

	// Evaluate history weight that indicates the convergence from metadata
	HALF historyBlur = HALF(1.0 - history.w);
	historyBlur = max(historyBlur, curHistoryBlur);
	history.w = history.w * g_historyMax + 1.0;

	// Compute color-space AABB
	HALF4 neighborMin, neighborMax;
	const HALF4 currentTM = HALF4(TM(current.xyz), current.w);
#ifdef _DENOISE_
	const HALF gamma = current.w <= 0.0 ? 1.0 : clamp(8.0 / historyBlur, 1.0, 32.0);
#elif defined(_ALPHA_AS_ID_)
	const HALF gamma = historyBlur > 0.0 || current.w <= 0.0 ? 1.0 : 16.0;
#else
	const HALF gamma = historyBlur > 0.0 || current.w < ALPHA_BOUND ? 1.0 : 16.0;
#endif
	HALF4 filtered = NeighborMinMax(neighborMin, neighborMax, currentTM, DTid, gamma);
	
	// Saturate history blurs
	curHistoryBlur = saturate(curHistoryBlur);
	historyBlur = saturate(historyBlur);

	// Clip historical color
	HALF3 historyTM = TM(history.xyz);
#if _USE_YCOCG_
	historyTM = clamp(historyTM, neighborMin.xyz, neighborMax.xyz);
#else
	historyTM = clipColor(historyTM, neighborMin.xyz, neighborMax.xyz);
#endif
	const HALF contrast = neighborMax.w - neighborMin.w;

	// Add aliasing
#if _USE_YCOCG_
	static const HALF lumContrastFactor = 32.0 * 4.0;
#else
	static const HALF lumContrastFactor = 32.0;
#endif
	HALF addAlias = historyBlur * 0.5 + 0.25;
	addAlias = saturate(addAlias + 1.0 / (1.0 + contrast * lumContrastFactor));
	filtered.xyz = lerp(filtered.xyz, currentTM.xyz, addAlias);

	// Calculate blend factor
	const HALF lumHist = GET_LUMA4(historyTM);
	const HALF distToClamp = min(abs(neighborMin.w - lumHist), abs(neighborMax.w - lumHist));
#if 0
	const float historyAmt = 1.0 / history.w + historyBlur / 8.0;
	const HALF historyFactor = HALF(distToClamp * historyAmt * (1.0 + historyBlur * historyAmt * 8.0));
	HALF blend = historyFactor / (distToClamp + contrast);
#else
	const HALF historyAmt = min(HALF(1.0 / history.w + historyBlur / 8.0), 1.0);
	HALF blend = 0.25 / lerp(8.0, distToClamp + contrast, historyAmt);
#endif
	blend = min(blend, 0.25);
	blend = filtered.w > 0.0 ? blend : 1.0;

	HALF3 result = ITM(lerp(historyTM, filtered.xyz, blend));
	result = any(isnan(result)) ? ITM(filtered.xyz) : result;
	history.w = min(history.w / g_historyMax, 1.0 - curHistoryBlur);

#ifdef _R11G11B10_
	g_rwRenderTarget[DTid] = result;
	g_rwMetaData[DTid] = history.w;
#else
	g_rwRenderTarget[DTid] = float4(result, history.w);
#endif
}
