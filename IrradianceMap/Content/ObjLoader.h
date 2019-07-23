//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#pragma once

class ObjLoader
{
public:
	struct float3
	{
		float x;
		float y;
		float z;

		float3() = default;
		constexpr float3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
		explicit float3(const float* pArray) : x(pArray[0]), y(pArray[1]), z(pArray[2]) {}

		float3& operator= (const float3& Float3) { x = Float3.x; y = Float3.y; z = Float3.z; return *this; }
	};

	struct Vertex
	{
		float3	m_position;
		float3	m_normal;
	};

	ObjLoader();
	virtual ~ObjLoader();

	bool Import(const char* pszFilename, bool recomputeNorm = true,
		bool needBound = true, bool forDX = true);

	const uint32_t GetNumVertices() const;
	const uint32_t GetNumIndices() const;
	const uint32_t GetVertexStride() const;
	const uint8_t* GetVertices() const;
	const uint32_t* GetIndices() const;

	const float3& GetCenter() const;
	const float GetRadius() const;

protected:
	void importGeometryFirstPass(FILE* pFile);
	void importGeometrySecondPass(FILE* pFile, bool forDX);
	void loadIndex(FILE* pFile, uint32_t& numTri);
	void computeNormal();
	void computeBound();

	std::vector<Vertex>		m_vertices;
	std::vector<uint32_t>	m_indices;
	std::vector<uint32_t>	m_tIndices;
	std::vector<uint32_t>	m_nIndices;

	float3		m_center;
	float		m_radius;
};
