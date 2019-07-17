//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#include "ObjLoader.h"

#define VEC_ALLOC(v, i)			{ v.resize(i); v.shrink_to_fit(); }

using namespace std;

ObjLoader::ObjLoader()
{
}

ObjLoader::~ObjLoader()
{
}

bool ObjLoader::Import(const char* pszFilename, const bool recomputeNorm, const bool needBound)
{
	FILE* pFile;
	fopen_s(&pFile, pszFilename, "r");

	if (!pFile) return false;

	// Import the OBJ file.
	importGeometryFirstPass(pFile);
	rewind(pFile);
	importGeometrySecondPass(pFile);
	fclose(pFile);

	// Perform post import tasks.
	if (recomputeNorm) computeNormal();
	if (needBound) computeBound();

	return true;
}

const uint32_t ObjLoader::GetNumVertices() const
{
	return static_cast<uint32_t>(m_vertices.size());
}

const uint32_t ObjLoader::GetNumIndices() const
{
	return static_cast<uint32_t>(m_indices.size());
}

const uint32_t ObjLoader::GetVertexStride() const
{
	return static_cast<uint32_t>(sizeof(Vertex));
}

const uint8_t* ObjLoader::GetVertices() const
{
	return reinterpret_cast<const uint8_t*>(m_vertices.data());
}

const uint32_t* ObjLoader::GetIndices() const
{
	return m_indices.data();
}

const ObjLoader::float3& ObjLoader::GetCenter() const
{
	return m_center;
}

const float ObjLoader::GetRadius() const
{
	return m_radius;
}

void ObjLoader::importGeometryFirstPass(FILE* pFile)
{
	auto v = 0u;
	auto vt = 0u;
	auto vn = 0u;
	char buffer[256] = { 0 };

	auto numVert = 0u;
	auto numTri = 0u;
	auto hasTexcoord = false;
	auto hasNormal = false;

	while (fscanf_s(pFile, "%s", buffer, static_cast<uint32_t>(sizeof(buffer))) != EOF)
	{
		switch (buffer[0])
		{
		case 'f':   // v, v//vn, v/vt, v/vt/vn.
			fscanf_s(pFile, "%s", buffer, static_cast<uint32_t>(sizeof(buffer)));

			if (strstr(buffer, "//")) // v//vn
			{
				sscanf_s(buffer, "%u//%u", &v, &vn);
				fscanf_s(pFile, "%u//%u", &v, &vn);
				fscanf_s(pFile, "%u//%u", &v, &vn);
				++numTri;

				while (fscanf_s(pFile, "%u//%u", &v, &vn) > 0) ++numTri;

				hasNormal = true;
			}
			else if (sscanf_s(buffer, "%u/%u/%u", &v, &vt, &vn) == 3) // v/vt/vn
			{
				fscanf_s(pFile, "%u/%u/%u", &v, &vt, &vn);
				fscanf_s(pFile, "%u/%u/%u", &v, &vt, &vn);
				++numTri;

				while (fscanf_s(pFile, "%u/%u/%u", &v, &vt, &vn) > 0) ++numTri;

				hasTexcoord = true;
				hasNormal = true;
			}
			else if (sscanf_s(buffer, "%u/%u", &v, &vt) == 2) // v/vt
			{
				fscanf_s(pFile, "%u/%u", &v, &vt);
				fscanf_s(pFile, "%u/%u", &v, &vt);
				++numTri;

				while (fscanf_s(pFile, "%u/%u", &v, &vt) > 0) ++numTri;

				hasTexcoord = true;
			}
			else // v
			{
				fscanf_s(pFile, "%u", &v);
				fscanf_s(pFile, "%u", &v);
				++numTri;

				while (fscanf_s(pFile, "%u", &v) > 0)
					++numTri;
			}
			break;

		case 'v':   // v, vt, or vn
			switch (buffer[1])
			{
			case '\0':
				fgets(buffer, sizeof(buffer), pFile);
				++numVert;
				break;
			default:
				break;
			}
			break;

		default:
			fgets(buffer, sizeof(buffer), pFile);
			break;
		}
	}

	// Allocate memory for the OBJ model data.
	const auto numIdx = numTri * 3;
	VEC_ALLOC(m_vertices, numVert);
	VEC_ALLOC(m_indices, numIdx);
	if (hasTexcoord) VEC_ALLOC(m_tIndices, numIdx);
	if (hasNormal) VEC_ALLOC(m_nIndices, numIdx);
}

void ObjLoader::importGeometrySecondPass(FILE* pFile)
{
	auto numVert = 0u;
	auto numTri = 0u;
	char buffer[256] = { 0 };

	while (fscanf_s(pFile, "%s", buffer, static_cast<uint32_t>(sizeof(buffer))) != EOF)
	{
		switch (buffer[0])
		{
		case 'f': // v, v//vn, v/vt, or v/vt/vn.
			loadIndex(pFile, numTri);
			break;
		case 'v': // v, vn, or vt.
			switch (buffer[1])
			{
			case '\0': // v
				fscanf_s(pFile, "%f %f %f",
					&m_vertices[numVert].m_position.x,
					&m_vertices[numVert].m_position.y,
					&m_vertices[numVert].m_position.z);
				++numVert;
				break;
			default:
				break;
			}
			break;

		default:
			fgets(buffer, sizeof(buffer), pFile);
			break;
		}
	}
}

void ObjLoader::loadIndex(FILE* pFile, uint32_t& numTri)
{
	uint32_t v[3] = { 0 };
	uint32_t vt[3] = { 0 };
	uint32_t vn[3] = { 0 };

	const auto numVert = static_cast<uint32_t>(m_vertices.size());

	for (auto i = 0ui8; i < 3u; ++i)
	{
		fscanf_s(pFile, "%u", &v[i]);
		v[i] = (v[i] < 0) ? v[i] + numVert - 1 : v[i] - 1;
		m_indices[numTri * 3 + i] = v[i];

		if (m_tIndices.size() > 0)
		{
			fscanf_s(pFile, "/%u", &vt[i]);
			vt[i] = (vt[i] < 0) ? vt[i] + numVert - 1 : vt[i] - 1;
			m_tIndices[numTri * 3 + i] = vt[i];
		}
		else if (m_nIndices.size() > 0) fscanf_s(pFile, "/");

		if (m_nIndices.size() > 0)
		{
			fscanf_s(pFile, "/%u", &vn[i]);
			vn[i] = (vn[i] < 0) ? vn[i] + numVert - 1 : vn[i] - 1;
			m_nIndices[numTri * 3 + i] = vn[i];
		}
	}
	++numTri;

	v[1] = v[2];
	vt[1] = vt[2];
	vn[1] = vn[2];

	while (fscanf_s(pFile, "%u", &v[2]) > 0)
	{
		v[2] = (v[2] < 0) ? v[2] + numVert - 1 : v[2] - 1;
		m_indices[numTri * 3] = v[0];
		m_indices[numTri * 3 + 1] = v[1];
		m_indices[numTri * 3 + 2] = v[2];
		v[1] = v[2];

		if (m_tIndices.size() > 0)
		{
			fscanf_s(pFile, "/%u", &vt[2]);
			vt[2] = (vt[2] < 0) ? vt[2] + numVert - 1 : vt[2] - 1;
			m_tIndices[numTri * 3] = vt[0];
			m_tIndices[numTri * 3 + 1] = vt[1];
			m_tIndices[numTri * 3 + 2] = vt[2];
			vt[1] = vt[2];
		}
		else if (m_nIndices.size() > 0) fscanf_s(pFile, "/");

		if (m_nIndices.size() > 0)
		{
			fscanf_s(pFile, "/%u", &vn[2]);
			vn[2] = (vn[2] < 0) ? vn[2] + numVert - 1 : vn[2] - 1;
			m_nIndices[numTri * 3] = vn[0];
			m_nIndices[numTri * 3 + 1] = vn[1];
			m_nIndices[numTri * 3 + 2] = vn[2];
			vn[1] = vn[2];
		}

		++numTri;
	}
}

void ObjLoader::computeNormal()
{
	float3 e1, e2, n;

	const auto uNumTri = static_cast<uint32_t>(m_indices.size()) / 3;
	for (auto i = 0u; i < uNumTri; i++)
	{
		const auto pv0 = &m_vertices[m_indices[i * 3]].m_position;
		const auto pv1 = &m_vertices[m_indices[i * 3 + 1]].m_position;
		const auto pv2 = &m_vertices[m_indices[i * 3 + 2]].m_position;
		e1.x = pv1->x - pv0->x;
		e1.y = pv1->y - pv0->y;
		e1.z = pv1->z - pv0->z;
		e2.x = pv2->x - pv1->x;
		e2.y = pv2->y - pv1->y;
		e2.z = pv2->z - pv1->z;
		n.x = e1.y * e2.z - e1.z * e2.y;
		n.y = e1.z * e2.x - e1.x * e2.z;
		n.z = e1.x * e2.y - e1.y * e2.x;
		const auto l = sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
		n.x /= l;
		n.y /= l;
		n.z /= l;

		const auto pvn0 = &m_vertices[m_indices[i * 3]].m_normal;
		const auto pvn1 = &m_vertices[m_indices[i * 3 + 1]].m_normal;
		const auto pvn2 = &m_vertices[m_indices[i * 3 + 2]].m_normal;
		pvn0->x += n.x;
		pvn0->y += n.y;
		pvn0->z += n.z;
		pvn1->x += n.x;
		pvn1->y += n.y;
		pvn1->z += n.z;
		pvn2->x += n.x;
		pvn2->y += n.y;
		pvn2->z += n.z;
	}

	const auto uNumVert = static_cast<uint32_t>(m_vertices.size());
	for (auto i = 0u; i < uNumVert; ++i)
	{
		const auto pn = &m_vertices[i].m_normal;
		const auto l = sqrt(pn->x * pn->x + pn->y * pn->y + pn->z * pn->z);
		pn->x /= l;
		pn->y /= l;
		pn->z /= l;
	}
}

void ObjLoader::computeBound()
{
	float xMax, xMin, yMax, yMin, zMax, zMin;
	xMax = xMin = m_vertices[0].m_position.x;
	yMax = yMin = m_vertices[0].m_position.y;
	zMax = zMin = m_vertices[0].m_position.z;

	auto x = 0.0f, y = 0.0f, z = 0.0f;

	for (auto i = 1u; i < m_vertices.size(); ++i)
	{
		x = m_vertices[i].m_position.x;
		y = m_vertices[i].m_position.y;
		z = m_vertices[i].m_position.z;

		if (x < xMin) xMin = x;
		else if (x > xMax) xMax = x;

		if (y < yMin) yMin = y;
		else if (y > yMax) yMax = y;

		if (z < zMin) zMin = z;
		else if (z > zMax) zMax = z;
	}

	m_center.x = (xMin + xMax) / 2.0f;
	m_center.y = (yMin + yMax) / 2.0f;
	m_center.z = (zMin + zMax) / 2.0f;

	const auto fWidth = xMax - xMin;
	const auto fHeight = yMax - yMin;
	const auto fLength = zMax - zMin;

	m_radius = max(max(fWidth, fHeight), fLength) * 0.5f;
}
