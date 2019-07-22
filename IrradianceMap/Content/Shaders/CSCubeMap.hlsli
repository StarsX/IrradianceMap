//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

float3 GetCubeTexcoord(uint3 index, float3 dim)
{
	const float2 radii = dim.xy * 0.5;
	float2 xy = index.xy - radii + 0.5;
	xy.y = -xy.y;

	const float3 pos = { xy, radii.x };

	switch (index.z)
	{
	case 0:
		return float3(pos.z, pos.y, -pos.x);
	case 1:
		return float3(-pos.z, pos.y, pos.x);
	case 2:
		return float3(pos.x, pos.z, -pos.y);
	case 3:
		return float3(pos.x, -pos.z, pos.y);
	case 4:
		return float3(pos.x, pos.y, pos.z);
	case 5:
		return float3(-pos.x, pos.y, -pos.z);
	default:
		return pos;
	}
}
