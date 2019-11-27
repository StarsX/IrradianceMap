//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
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
static const min16float g_historyMax = min16float(1 << 8) - 1.0;

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
min16float3 NeighborMinMax(out min16float4 neighborMin, out min16float4 neighborMax,
	min16float3 mu, int2 tex, min16float gamma = 1.0)
{
	static float weights[] =
	{
		0.5, 0.5, 0.5, 0.5,
		0.25, 0.25, 0.25, 0.25
	};

	float3 neighbors[NUM_NEIGHBORS];
	[unroll]
	for (uint i = 0; i < NUM_NEIGHBORS; ++i)
		neighbors[i] = g_txCurrent[tex + g_texOffsets[i]].xyz;

	float3 gaussian = mu;

#if	_VARIANCE_AABB_
#define m1	mu
	float3 m2 = m1 * m1;
#else
	neighborMin.xyz = neighborMax.xyz = mu;
#endif

	//[unroll]
	for (i = 0; i < NUM_NEIGHBORS; ++i)
	{
		const min16float3 neighbor = min16float3(neighbors[i]);
		gaussian += neighbors[i] * weights[i];

#if	_VARIANCE_AABB_
		m1 += neighbor;
		m2 = m2 + neighbors[i] * neighbors[i];
#else
		neighborMin.xyz = min(neighbor, neighborMin.xyz);
		neighborMax.xyz = max(neighbor, neighborMax.xyz);
#endif
	}

#if	_VARIANCE_AABB_
	mu /= NUM_SAMPLES;
	const min16float3 variance = min16float3(m2 / NUM_SAMPLES - (float3)mu * mu);
	const min16float3 sigma = sqrt(abs(variance));
	const min16float3 gsigma = gamma * sigma;
	neighborMin.xyz = mu - gsigma;
	neighborMax.xyz = mu + gsigma;
	neighborMin.w = GET_LUMA(mu - sigma);
	neighborMax.w = GET_LUMA(mu + sigma);
#else
	neighborMin.w = GET_LUMA(neighborMin.xyz);
	neighborMax.w = GET_LUMA(neighborMax.xyz);
#endif

	gaussian /= 4.0;

	return min16float3(gaussian);
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
	const float4 history = g_txHistory.SampleLevel(g_smpLinear, texBack, 0);

	// Speed to history blur
	const float2 historyBlurAmp = 4.0 * texSize;
	const min16float2 historyBlurs = min16float2(abs(velocity.xy) * historyBlurAmp);
	const min16float historyBlur = saturate(historyBlurs.x + historyBlurs.y);
	
	min16float weight = min16float(history.w);
	const min16float historyDiv = max(1.0 - weight, historyBlur);
	weight = weight * g_historyMax + 1.0;

	min16float4 neighborMin, neighborMax;
	const min16float3 curColor = min16float3(current.xyz);
	const min16float gamma = historyDiv > 0.0 || current.w < 1.0 ? 1.0 : 16.0;
	min16float3 filtered = NeighborMinMax(neighborMin, neighborMax, curColor, DTid, gamma);
	
	min16float3 histColor = min16float3(history.xyz);
	const min16float lumHist = GET_LUMA(histColor);
	histColor = clipColor(histColor, neighborMin.xyz, neighborMax.xyz);

	const min16float contrast = neighborMax.w - neighborMin.w;

	// Add aliasing
	static const min16float lumContrastFactor = 32.0;
	min16float addAlias = historyBlur * 0.5 + 0.25;
	addAlias = saturate(addAlias + 1.0 / (1.0 + contrast * lumContrastFactor));
	filtered = lerp(filtered, curColor, addAlias);

	// Calculate Blend factor
	const min16float distToClamp = min(abs(neighborMin.w - lumHist), abs(neighborMax.w - lumHist));
	const min16float historyAmt = 1.0 / weight + historyBlur / 8.0;
	const min16float historyFactor = distToClamp * historyAmt * (1.0 + historyBlur * historyAmt * 8.0);
	min16float blend = clamp(historyFactor / (distToClamp + contrast), 0.03125, 0.25);
	//min16float blend = clamp(1.0 / weight, 0.125, 0.25);

	const min16float3 result = lerp(histColor, filtered, blend);
	weight = min(weight / g_historyMax, 1.0 - historyBlur);

	g_rwRenderTarget[DTid] = float4(result, weight);
}
