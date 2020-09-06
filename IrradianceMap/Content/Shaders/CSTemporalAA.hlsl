//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

//--------------------------------------------------------------------------------------
// Definitions
//--------------------------------------------------------------------------------------
#define	_VARIANCE_AABB_		1

#define	NUM_NEIGHBORS		8
#define	NUM_SAMPLES			(NUM_NEIGHBORS + 1)
#define	NUM_NEIGHBORS_H		4

#define GET_LUMA(v)			dot(v, g_lumBase)

//--------------------------------------------------------------------------------------
// Constants
//--------------------------------------------------------------------------------------
static const uint g_historyBits = 4;
static const uint g_historyMask = (1 << g_historyBits) - 1;
static const float g_historyMax = g_historyMask;

static const min16float3 g_lumBase = { 0.25, 0.5, 0.25 };
static const int2 g_texOffsets[] =
{
	int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1),
	int2(-1, -1), int2(1, -1), int2(1, 1), int2(-1, 1)
};

//--------------------------------------------------------------------------------------
// Texture and buffers
//--------------------------------------------------------------------------------------
RWTexture2D<float4>	g_rwRenderTarget;
Texture2D			g_txCurrent		: register (t0);
Texture2D			g_txHistory		: register (t1);
Texture2D<float2>	g_txVelocity	: register (t2);

//--------------------------------------------------------------------------------------
// Sampler
//--------------------------------------------------------------------------------------
SamplerState g_smpLinear;

// --------------------------------------------------------------------------------------
// A fast invertible tone map that preserves color (Reinhard)
// --------------------------------------------------------------------------------------
min16float3 TM(min16float3 rgb)
{
	return rgb / (1.0 + dot(rgb, g_lumBase));
}

// --------------------------------------------------------------------------------------
// Inverse of preceding function
// --------------------------------------------------------------------------------------
min16float3 ITM(min16float3 rgb)
{
	return rgb / max(1.0 - dot(rgb, g_lumBase), 1e-4);
}

//--------------------------------------------------------------------------------------
// Maxinum velocity of 3x3
//--------------------------------------------------------------------------------------
min16float4 VelocityMax(int2 tex)
{
	const float2 velocity = g_txVelocity[tex];

	float2 velocities[NUM_NEIGHBORS_H];
	[unroll]
	for (uint i = 0; i < NUM_NEIGHBORS_H; ++i)
		velocities[i] = g_txVelocity[tex + g_texOffsets[i + NUM_NEIGHBORS_H]];

	min16float4 velocityMax = min16float2(velocity).xyxy;
	min16float speedSq = dot(velocityMax.xy, velocityMax.xy);
	//[unroll]
	for (i = 0; i < NUM_NEIGHBORS_H; ++i)
	{
		const min16float2 neighbor = min16float2(velocities[i]);
		const min16float speedSqN = dot(neighbor, neighbor);
		if (speedSqN > speedSq)
		{
			velocityMax.xy = neighbor;
			speedSq = speedSqN;
		}
	}

	return velocityMax;
}

//--------------------------------------------------------------------------------------
// Minimum and maxinum of the neighbor samples, returning Gaussian blurred color
//--------------------------------------------------------------------------------------
min16float4 NeighborMinMax(out min16float4 neighborMin, out min16float4 neighborMax,
	min16float4 current, int2 pos, min16float gamma = 1.0)
{
	static const min16float weights[] =
	{
		0.5, 0.5, 0.5, 0.5,
		0.25, 0.25, 0.25, 0.25
	};

	float4 neighbors[NUM_NEIGHBORS];
	[unroll]
	for (uint i = 0; i < NUM_NEIGHBORS; ++i)
		neighbors[i] = g_txCurrent[pos + g_texOffsets[i]];

	min16float3 mu = current.xyz;
	current.w = current.w < 0.5 ? 0.0 : 1.0;

#if	_VARIANCE_AABB_
#define	m1	mu
	min16float3 m2 = m1 * m1;
#else
	neighborMin.xyz = neighborMax.xyz = mu;
#endif

	//[unroll]
	for (i = 0; i < NUM_NEIGHBORS; ++i)
	{
		min16float4 neighbor = min16float4(neighbors[i]);
		neighbor.xyz = TM(neighbor.xyz);
		neighbor.w = neighbor.w < 0.5 ? 0.0 : 1.0;
		current += neighbor * weights[i];

#if	_VARIANCE_AABB_
		m1 += neighbor.xyz;
		m2 += neighbor.xyz * neighbor.xyz;
#else
		neighborMin.xyz = min(neighbor, neighborMin.xyz);
		neighborMax.xyz = max(neighbor, neighborMax.xyz);
#endif
	}

#if	_VARIANCE_AABB_
	mu /= NUM_SAMPLES;
	const min16float3 sigma = sqrt(abs(m2 / NUM_SAMPLES - mu * mu));
	const min16float3 gsigma = gamma * sigma;
	neighborMin.xyz = mu - gsigma;
	neighborMax.xyz = mu + gsigma;
	neighborMin.w = GET_LUMA(mu - sigma);
	neighborMax.w = GET_LUMA(mu + sigma);
#else
	neighborMin.w = GET_LUMA(neighborMin.xyz);
	neighborMax.w = GET_LUMA(neighborMax.xyz);
#endif

	current /= 4.0;

	return current;
}

//--------------------------------------------------------------------------------------
// Clip color
//--------------------------------------------------------------------------------------
min16float3 clipColor(min16float3 color, min16float3 minColor, min16float3 maxColor)
{
	const min16float3 cent = 0.5 * (maxColor + minColor);
	const min16float3 dist = 0.5 * (maxColor - minColor);

	const min16float3 disp = color - cent;
	const min16float3 dir = abs(disp / dist);
	const min16float maxComp = max(dir.x, max(dir.y, dir.z));

	if (maxComp > 1.0) return cent + disp / maxComp;
	else return color;
}

[numthreads(8, 8, 1)]
void main(uint2 DTid : SV_DispatchThreadID)
{
	float2 texSize;
	g_txHistory.GetDimensions(texSize.x, texSize.y);
	const float2 tex = (DTid + 0.5) / texSize;

	// Load G-buffers
	const float4 current = g_txCurrent[DTid];
	const min16float4 velocity = VelocityMax(DTid);
	const float2 texBack = tex - velocity.xy;
	float4 history = g_txHistory.SampleLevel(g_smpLinear, texBack, 0);

	// Speed to history blur
	const float2 historyBlurAmp = 4.0 * texSize;
	const min16float2 historyBlurs = min16float2(abs(velocity.xy) * historyBlurAmp);
	min16float historyBlurMin = saturate(historyBlurs.x + historyBlurs.y);

	// Mask and historical mask
	min16float historyBlur = min16float(1.0 - history.w);
	historyBlur = max(historyBlur, historyBlurMin);
	history.w = history.w * g_historyMax + 1.0;

	min16float4 neighborMin, neighborMax;
	min16float4 curColor = min16float4(current);
	curColor.xyz = TM(curColor.xyz);
	const min16float gamma = historyBlur > 0.0 || current.w <= 0.0 ? 1.0 : 16.0;
	min16float4 filtered = NeighborMinMax(neighborMin, neighborMax, curColor, DTid, gamma);
	
	min16float3 histColor = TM(min16float3(history.xyz));
	histColor = clipColor(histColor, neighborMin.xyz, neighborMax.xyz);
	const min16float contrast = neighborMax.w - neighborMin.w;

	// Add aliasing
	static const min16float lumContrastFactor = 32.0;
	min16float addAlias = historyBlur * 0.5 + 0.25;
	addAlias = saturate(addAlias + 1.0 / (1.0 + contrast * lumContrastFactor));
	filtered.xyz = lerp(filtered.xyz, curColor.xyz, addAlias);

	// Calculate Blend factor
	const min16float lumHist = GET_LUMA(histColor);
	const min16float distToClamp = min(abs(neighborMin.w - lumHist), abs(neighborMax.w - lumHist));
#if 0
	const float historyAmt = 1.0 / history.w + historyBlur / 8.0;
	const min16float historyFactor = min16float(distToClamp * historyAmt * (1.0 + historyBlur * historyAmt * 8.0));
	min16float blend = historyFactor / (distToClamp + contrast);
	blend = historyBlur > 0.0 && current.w > 0.0 ? blend : min16float(historyAmt);
	blend = min(blend, 0.25);
#else
	const float historyFactor = min(distToClamp - contrast, 0.0);
	min16float blend = min16float(1.0 / history.w + historyBlurMin * historyFactor);
	blend = historyBlur > 0.0 || filtered.w < 1.0 ? blend : 0.0;
	blend = clamp(blend, 0.0, 0.2);
#endif

	const min16float3 result = ITM(lerp(histColor, filtered.xyz, blend));
	history.w = min(history.w / g_historyMax, max(1.0 - historyBlurMin, 0.0));

	g_rwRenderTarget[DTid] = float4(result, history.w);
}
