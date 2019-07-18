//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#include "DXFrameworkHelper.h"
#include "ObjLoader.h"
#include "Renderer.h"

#define SizeOfInUint32(obj)	DIV_UP(sizeof(obj), sizeof(uint32_t))

using namespace std;
using namespace DirectX;
using namespace XUSG;

Renderer::Renderer(const Device& device) :
	m_device(device),
	m_frameParity(0)
{
	m_graphicsPipelineCache.SetDevice(device);
	m_computePipelineCache.SetDevice(device);
	m_pipelineLayoutCache.SetDevice(device);

	//m_descriptorTableCache.SetName(L"DescriptorTableCache");
}

Renderer::~Renderer()
{
}

bool Renderer::Init(const CommandList& commandList, uint32_t width, uint32_t height,
	const shared_ptr<DescriptorTableCache>& descriptorTableCache, vector<Resource>& uploaders,
	const char* fileName, Format rtFormat, const XMFLOAT4& posScale)
{
	m_descriptorTableCache = descriptorTableCache;

	m_viewport = XMUINT2(width, height);
	m_posScale = posScale;

	// Load inputs
	ObjLoader objLoader;
	if (!objLoader.Import(fileName, true, true)) return false;
	N_RETURN(createVB(commandList, objLoader.GetNumVertices(), objLoader.GetVertexStride(), objLoader.GetVertices(), uploaders), false);
	N_RETURN(createIB(commandList, objLoader.GetNumIndices(), objLoader.GetIndices(), uploaders), false);

	// Create output views
	// Render targets
	m_renderTargets[RT_COLOR].Create(m_device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, D3D12_RESOURCE_FLAG_NONE,
		1, 1, D3D12_RESOURCE_STATE_COMMON, nullptr, false, L"Color");
	m_renderTargets[RT_VELOCITY].Create(m_device, width, height, DXGI_FORMAT_R16G16_FLOAT, 1, D3D12_RESOURCE_FLAG_NONE,
		1, 1, D3D12_RESOURCE_STATE_COMMON, nullptr, false, L"Velocity");
	m_depth.Create(m_device, width, height, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, D3D12_RESOURCE_FLAG_NONE,
		1, 1, 1, D3D12_RESOURCE_STATE_COMMON, 1.0f, 0, false, L"Depth");

	// Temporal AA
	m_outputViews[UAV_PP_TAA].Create(m_device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
		1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, 1, 1, D3D12_HEAP_TYPE_DEFAULT,
		D3D12_RESOURCE_STATE_COMMON, false, L"TemporalAAOut0");
	m_outputViews[UAV_PP_TAA1].Create(m_device, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT,
		1, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, 1, 1, D3D12_HEAP_TYPE_DEFAULT,
		D3D12_RESOURCE_STATE_COMMON, false, L"TemporalAAOut1");

	// Create pipelines
	N_RETURN(createInputLayout(), false);
	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(rtFormat), false);
	N_RETURN(createDescriptorTables(), false);

	return true;
}

bool Renderer::SetLightProbes(const Descriptor& irradiance, const Descriptor& radiance)
{
	const Descriptor descriptors[] = { irradiance, radiance };
	Util::DescriptorTable descriptorTable;
	descriptorTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
	X_RETURN(m_srvTables[SRV_TABLE_BASE], descriptorTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);

	return true;
}

static const XMFLOAT2& IncrementalHalton()
{
	static auto haltonBase = XMUINT2(0, 0);
	static auto halton = XMFLOAT2(0.0f, 0.0f);

	// Base 2
	{
		// Bottom bit always changes, higher bits
		// Change less frequently.
		auto change = 0.5f;
		auto oldBase = haltonBase.x++;
		auto diff = haltonBase.x ^ oldBase;

		// Diff will be of the form 0*1+, i.e. one bits up until the last carry.
		// Expected iterations = 1 + 0.5 + 0.25 + ... = 2
		do
		{
			halton.x += (oldBase & 1) ? -change : change;
			change *= 0.5f;

			diff = diff >> 1;
			oldBase = oldBase >> 1;
		} while (diff);
	}

	// Base 3
	{
		const auto oneThird = 1.0f / 3.0f;
		auto mask = 0x3u;	// Also the max base 3 digit
		auto add = 0x1u;	// Amount to add to force carry once digit == 3
		auto change = oneThird;
		++haltonBase.y;

		// Expected iterations: 1.5
		while (true)
		{
			if ((haltonBase.y & mask) == mask)
			{
				haltonBase.y += add;	// Force carry into next 2-bit digit
				halton.y -= 2 * change;

				mask = mask << 2;
				add = add << 2;

				change *= oneThird;
			}
			else
			{
				halton.y += change;	// We know digit n has gone from a to a + 1
				break;
			}
		};
	}

	return halton;
}

void Renderer::UpdateFrame(uint32_t frameIndex, CXMVECTOR eyePt, CXMMATRIX viewProj, bool isPaused)
{
	auto projToWorld = XMMatrixInverse(nullptr, viewProj);
	projToWorld = XMMatrixTranspose(projToWorld);
	{
		static auto angle = 0.0f;
		angle += !isPaused ? 0.1f * XM_PI / 180.0f : 0.0f;
		const auto rot = XMMatrixRotationY(angle);

		//const auto n = 256u;
		//static auto i = 0u;
		const auto normalMatrix = XMMatrixTranspose(rot);
		//i = (i + 1) % n;

		const auto world = XMMatrixScaling(m_posScale.w, m_posScale.w, m_posScale.w) * rot *
			XMMatrixTranslation(m_posScale.x, m_posScale.y, m_posScale.z);

		const auto halton = IncrementalHalton();
		XMFLOAT2 jitter =
		{
			(halton.x * 2.0f - 1.0f) / m_viewport.x,
			(halton.y * 2.0f - 1.0f) / m_viewport.y
		};
		m_cbBasePass.ProjBias = jitter;
		m_cbBasePass.WorldViewProjPrev = m_cbBasePass.WorldViewProj;
		XMStoreFloat4x4(&m_world, XMMatrixTranspose(world));
		XMStoreFloat4x4(&m_cbBasePass.WorldViewProj, XMMatrixTranspose(world * viewProj));
		XMStoreFloat4x4(&m_cbBasePass.Normal, normalMatrix);
	}

	XMStoreFloat3(&m_eyePt, eyePt);
}

void Renderer::Render(const CommandList& commandList, uint32_t frameIndex)
{
	ResourceBarrier barriers[4];
	auto numBarriers = m_renderTargets[RT_COLOR].SetBarrier(barriers, D3D12_RESOURCE_STATE_RENDER_TARGET);
	numBarriers = m_renderTargets[RT_VELOCITY].SetBarrier(barriers, D3D12_RESOURCE_STATE_RENDER_TARGET, numBarriers);
	numBarriers = m_depth.SetBarrier(barriers, D3D12_RESOURCE_STATE_DEPTH_WRITE, numBarriers);
	numBarriers = m_outputViews[UAV_PP_TAA + !m_frameParity].SetBarrier(barriers, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, numBarriers, 0xffffffff, D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY);
	commandList.Barrier(numBarriers, barriers);
	render(commandList);

	temporalAA(commandList);
	m_frameParity = !m_frameParity;
}

void Renderer::ToneMap(const CommandList& commandList, const RenderTargetTable& rtvTable,
	uint32_t numBarriers, ResourceBarrier* pBarriers)
{
	numBarriers = m_outputViews[UAV_PP_TAA + m_frameParity].SetBarrier(
		pBarriers, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, numBarriers);
	commandList.Barrier(numBarriers, pBarriers);

	// Set render target
	commandList.OMSetRenderTargets(1, rtvTable, nullptr);

	// Set descriptor tables
	commandList.SetGraphicsPipelineLayout(m_pipelineLayouts[TONE_MAP]);
	commandList.SetGraphicsDescriptorTable(0, m_srvTables[SRV_TABLE_TM + m_frameParity]);

	// Set pipeline state
	commandList.SetPipelineState(m_pipelines[TONE_MAP]);

	// Set viewport
	Viewport viewport(0.0f, 0.0f, static_cast<float>(m_viewport.x), static_cast<float>(m_viewport.y));
	RectRange scissorRect(0, 0, m_viewport.x, m_viewport.y);
	commandList.RSSetViewports(1, &viewport);
	commandList.RSSetScissorRects(1, &scissorRect);

	commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	commandList.DrawIndexed(3, 1, 0, 0, 0);
}

bool Renderer::createVB(const CommandList& commandList, uint32_t numVert,
	uint32_t stride, const uint8_t* pData, vector<Resource>& uploaders)
{
	N_RETURN(m_vertexBuffer.Create(m_device, numVert, stride, D3D12_RESOURCE_FLAG_NONE,
		D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, 1, nullptr, 1, nullptr,
		1, nullptr, L"MeshVB"), false);
	uploaders.push_back(nullptr);

	return m_vertexBuffer.Upload(commandList, uploaders.back(), pData, stride * numVert,
		D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

bool Renderer::createIB(const CommandList& commandList, uint32_t numIndices,
	const uint32_t* pData, vector<Resource>& uploaders)
{
	m_numIndices = numIndices;

	const uint32_t byteWidth = sizeof(uint32_t) * numIndices;
	N_RETURN(m_indexBuffer.Create(m_device, byteWidth, DXGI_FORMAT_R32_UINT,
		D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST,
		1, nullptr, 1, nullptr, 1, nullptr, L"MeshIB"), false);
	uploaders.push_back(nullptr);

	return m_indexBuffer.Upload(commandList, uploaders.back(), pData,
		byteWidth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

bool Renderer::createInputLayout()
{
	const auto offset = D3D12_APPEND_ALIGNED_ELEMENT;

	// Define the vertex input layout.
	InputElementTable inputElementDescs =
	{
		{ "POSITION",	0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,		D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "NORMAL",		0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offset,	D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
	};

	X_RETURN(m_inputLayout, m_graphicsPipelineCache.CreateInputLayout(inputElementDescs), false);

	return true;
}

bool Renderer::createPipelineLayouts()
{
	// This is a pipeline layout for base pass
	{
		Util::PipelineLayout pipelineLayout;
		pipelineLayout.SetConstants(VS_CONSTANTS, SizeOfInUint32(BasePassConstants), 0, 0, Shader::Stage::VS);
		pipelineLayout.SetConstants(PS_CONSTANTS, SizeOfInUint32(XMFLOAT3), 0, 0, Shader::Stage::PS);
		pipelineLayout.SetRange(SHADER_RESOURCES, DescriptorType::SRV, 2, 0);
		pipelineLayout.SetShaderStage(SHADER_RESOURCES, Shader::PS);
		pipelineLayout.SetRange(SAMPLER, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout.SetShaderStage(SAMPLER, Shader::PS);
		X_RETURN(m_pipelineLayouts[BASE_PASS], pipelineLayout.GetPipelineLayout(m_pipelineLayoutCache,
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, L"BasePassLayout"), false);
	}

	// This is a pipeline layout for temporal AA
	{
		Util::PipelineLayout pipelineLayout;
		pipelineLayout.SetRange(OUTPUT_VIEW, DescriptorType::UAV, 1, 0, 0,
			D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout.SetRange(SHADER_RESOURCES, DescriptorType::SRV, 3, 0);
		pipelineLayout.SetRange(SAMPLER, DescriptorType::SAMPLER, 1, 0);
		X_RETURN(m_pipelineLayouts[TEMPORAL_AA], pipelineLayout.GetPipelineLayout(m_pipelineLayoutCache,
			D3D12_ROOT_SIGNATURE_FLAG_NONE, L"TemporalAALayout"), false);
	}

	// This is a pipeline layout for tone mapping
	{
		Util::PipelineLayout pipelineLayout;
		pipelineLayout.SetRange(0, DescriptorType::SRV, 1, 0);
		pipelineLayout.SetShaderStage(0, Shader::Stage::PS);
		X_RETURN(m_pipelineLayouts[TONE_MAP], pipelineLayout.GetPipelineLayout(m_pipelineLayoutCache,
			D3D12_ROOT_SIGNATURE_FLAG_NONE, L"ToneMappingLayout"), false);
	}

	return true;
}

bool Renderer::createPipelines(Format rtFormat)
{
	// Base pass
	{
		const auto vs = m_shaderPool.CreateShader(Shader::Stage::VS, 0, L"VSBasePass.cso");
		const auto ps = m_shaderPool.CreateShader(Shader::Stage::PS, 0, L"PSBasePass.cso");

		Graphics::State state;
		state.IASetInputLayout(m_inputLayout);
		state.SetPipelineLayout(m_pipelineLayouts[BASE_PASS]);
		state.SetShader(Shader::Stage::VS, vs);
		state.SetShader(Shader::Stage::PS, ps);
		state.IASetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
		state.OMSetNumRenderTargets(NUM_RENDER_TARGET);
		state.OMSetRTVFormat(RT_COLOR, DXGI_FORMAT_R16G16B16A16_FLOAT);
		state.OMSetRTVFormat(RT_VELOCITY, DXGI_FORMAT_R16G16_FLOAT);
		state.OMSetDSVFormat(DXGI_FORMAT_D24_UNORM_S8_UINT);
		X_RETURN(m_pipelines[BASE_PASS], state.GetPipeline(m_graphicsPipelineCache, L"BasePass"), false);
	}

	// Temporal AA
	{
		const auto shader = m_shaderPool.CreateShader(Shader::Stage::CS, 0, L"CSTemporalAA.cso");

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[TEMPORAL_AA]);
		state.SetShader(shader);
		X_RETURN(m_pipelines[TEMPORAL_AA], state.GetPipeline(m_computePipelineCache, L"TemporalAA"), false);
	}

	// Tone mapping
	{
		const auto vs = m_shaderPool.CreateShader(Shader::Stage::VS, 1, L"VSScreenQuad.cso");
		const auto ps = m_shaderPool.CreateShader(Shader::Stage::PS, 1, L"PSToneMap.cso");

		Graphics::State state;
		state.SetPipelineLayout(m_pipelineLayouts[TONE_MAP]);
		state.SetShader(Shader::Stage::VS, vs);
		state.SetShader(Shader::Stage::PS, ps);
		state.DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache);
		state.IASetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
		state.OMSetNumRenderTargets(1);
		state.OMSetRTVFormat(0, rtFormat);
		X_RETURN(m_pipelines[TONE_MAP], state.GetPipeline(m_graphicsPipelineCache, L"ToneMapping"), false);
	}

	return true;
}

bool Renderer::createDescriptorTables()
{
	// Temporal AA output UAVs
	for (auto i = 0u; i < 2; ++i)
	{
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, 1, &m_outputViews[UAV_PP_TAA + i].GetUAV());
		X_RETURN(m_uavTables[UAV_TABLE_TAA + i], descriptorTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Temporal AA input SRVs
	for (auto i = 0u; i < 2; ++i)
	{
		const Descriptor descriptors[] =
		{
			m_renderTargets[RT_COLOR].GetSRV(),
			m_outputViews[UAV_PP_TAA + !i].GetSRV(),
			m_renderTargets[RT_VELOCITY].GetSRV()
		};
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_srvTables[SRV_TABLE_TAA + i], descriptorTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Tone mapping SRVs
	for (auto i = 0u; i < 2; ++i)
	{
		Util::DescriptorTable descriptorTable;
		descriptorTable.SetDescriptors(0, 1, &m_outputViews[UAV_PP_TAA + i].GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_TM + i], descriptorTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// RTV table
	{
		const Descriptor descriptors[] =
		{
			m_renderTargets[RT_COLOR].GetRTV(),
			m_renderTargets[RT_VELOCITY].GetRTV()
		};
		Util::DescriptorTable rtvTable;
		rtvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_rtvTable, rtvTable.GetRtvTable(*m_descriptorTableCache), false);
	}

	// Create the sampler
	{
		Util::DescriptorTable samplerTable;
		const auto samplerAnisoWrap = SamplerPreset::ANISOTROPIC_WRAP;
		samplerTable.SetSamplers(0, 1, &samplerAnisoWrap, *m_descriptorTableCache);
		X_RETURN(m_samplerTable, samplerTable.GetSamplerTable(*m_descriptorTableCache), false);
	}

	return true;
}

void Renderer::render(const CommandList& commandList)
{
	// Set render target
	commandList.OMSetRenderTargets(NUM_RENDER_TARGET, m_rtvTable, &m_depth.GetDSV(), true);

	// Clear render target
	const float clearColor[4] = {};
	commandList.ClearRenderTargetView(m_renderTargets[RT_COLOR].GetRTV(), clearColor);
	commandList.ClearRenderTargetView(m_renderTargets[RT_VELOCITY].GetRTV(), clearColor);
	commandList.ClearDepthStencilView(m_depth.GetDSV(), D3D12_CLEAR_FLAG_DEPTH, 1.0f);

	// Set pipeline state
	commandList.SetGraphicsPipelineLayout(m_pipelineLayouts[BASE_PASS]);
	commandList.SetPipelineState(m_pipelines[BASE_PASS]);

	// Set viewport
	Viewport viewport(0.0f, 0.0f, static_cast<float>(m_viewport.x), static_cast<float>(m_viewport.y));
	RectRange scissorRect(0, 0, m_viewport.x, m_viewport.y);
	commandList.RSSetViewports(1, &viewport);
	commandList.RSSetScissorRects(1, &scissorRect);

	commandList.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	// Set descriptor tables
	commandList.SetGraphics32BitConstants(VS_CONSTANTS, SizeOfInUint32(m_cbBasePass), &m_cbBasePass);
	commandList.SetGraphics32BitConstants(PS_CONSTANTS, SizeOfInUint32(m_eyePt), &m_eyePt);
	commandList.SetGraphicsDescriptorTable(SHADER_RESOURCES, m_srvTables[SRV_TABLE_BASE]);
	commandList.SetGraphicsDescriptorTable(SAMPLER, m_samplerTable);

	commandList.IASetVertexBuffers(0, 1, &m_vertexBuffer.GetVBV());
	commandList.IASetIndexBuffer(m_indexBuffer.GetIBV());

	commandList.DrawIndexed(m_numIndices, 1, 0, 0, 0);
}

void Renderer::temporalAA(const CommandList& commandList)
{
	// Bind the heaps, acceleration structure and dispatch rays.
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache->GetDescriptorPool(SAMPLER_POOL)
	};
	commandList.SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	ResourceBarrier barriers[4];
	auto numBarriers = m_outputViews[UAV_PP_TAA + m_frameParity].SetBarrier(barriers, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	numBarriers = m_renderTargets[RT_COLOR].SetBarrier(barriers, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, numBarriers);
	numBarriers = m_outputViews[UAV_PP_TAA + !m_frameParity].SetBarrier(barriers, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, numBarriers, 0xffffffff, D3D12_RESOURCE_BARRIER_FLAG_END_ONLY);
	numBarriers = m_renderTargets[RT_VELOCITY].SetBarrier(barriers, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, numBarriers);
	commandList.Barrier(numBarriers, barriers);

	// Set descriptor tables
	commandList.SetComputePipelineLayout(m_pipelineLayouts[TEMPORAL_AA]);
	commandList.SetComputeDescriptorTable(OUTPUT_VIEW, m_uavTables[UAV_TABLE_TAA + m_frameParity]);
	commandList.SetComputeDescriptorTable(SHADER_RESOURCES, m_srvTables[SRV_TABLE_TAA + m_frameParity]);
	commandList.SetComputeDescriptorTable(SAMPLER, m_samplerTable);

	// Set pipeline state
	commandList.SetPipelineState(m_pipelines[TEMPORAL_AA]);
	commandList.Dispatch(DIV_UP(m_viewport.x, 8), DIV_UP(m_viewport.y, 8), 1);
}
