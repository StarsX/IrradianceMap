//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "DXFrameworkHelper.h"
#include "Optional/XUSGObjLoader.h"
#include "Renderer.h"

using namespace std;
using namespace DirectX;
using namespace XUSG;

struct CBBasePass
{
	XMFLOAT4X4	WorldViewProj;
	XMFLOAT4X4	WorldViewProjPrev;
	XMFLOAT3X4	World;
	XMFLOAT2	ProjBias;
};

struct CBPerFrame
{
	XMFLOAT4	EyePtGlossy;
	XMFLOAT4X4	ScreenToWorld;
};

Renderer::Renderer(const Device::sptr& device) :
	m_device(device),
	m_frameParity(0)
{
	m_shaderPool = ShaderPool::MakeUnique();
	m_graphicsPipelineCache = Graphics::PipelineCache::MakeUnique(device.get());
	m_computePipelineCache = Compute::PipelineCache::MakeUnique(device.get());
	m_pipelineLayoutCache = PipelineLayoutCache::MakeUnique(device.get());
}

Renderer::~Renderer()
{
}

bool Renderer::Init(CommandList* pCommandList, uint32_t width, uint32_t height,
	const DescriptorTableCache::sptr& descriptorTableCache, vector<Resource::uptr>& uploaders,
	const char* fileName, Format rtFormat, const XMFLOAT4& posScale)
{
	m_descriptorTableCache = descriptorTableCache;

	m_viewport = XMUINT2(width, height);
	m_posScale = posScale;

	// Load inputs
	ObjLoader objLoader;
	if (!objLoader.Import(fileName, true, true)) return false;
	N_RETURN(createVB(pCommandList, objLoader.GetNumVertices(), objLoader.GetVertexStride(), objLoader.GetVertices(), uploaders), false);
	N_RETURN(createIB(pCommandList, objLoader.GetNumIndices(), objLoader.GetIndices(), uploaders), false);

	// Create output views
	// Render targets
	for (auto& renderTarget : m_renderTargets) renderTarget = RenderTarget::MakeUnique();
	m_renderTargets[RT_COLOR]->Create(m_device.get(), width, height, Format::R16G16B16A16_FLOAT,
		1, ResourceFlag::NONE, 1, 1, nullptr, false, MemoryFlag::NONE, L"Color");
	m_renderTargets[RT_VELOCITY]->Create(m_device.get(), width, height, Format::R16G16_FLOAT,
		1, ResourceFlag::NONE, 1, 1, nullptr, false, MemoryFlag::NONE, L"Velocity");

	m_depth = DepthStencil::MakeUnique();
	m_depth->Create(m_device.get(), width, height, Format::D24_UNORM_S8_UINT,
		ResourceFlag::DENY_SHADER_RESOURCE, 1, 1, 1, 1.0f, 0, false,
		MemoryFlag::NONE, L"Depth");

	// Temporal AA
	for (auto& outView : m_outputViews) outView = Texture2D::MakeUnique();
	m_outputViews[UAV_PP_TAA]->Create(m_device.get(), width, height, Format::R16G16B16A16_FLOAT, 1,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, 1, false, MemoryFlag::NONE, L"TemporalAAOut0");
	m_outputViews[UAV_PP_TAA1]->Create(m_device.get(), width, height, Format::R16G16B16A16_FLOAT, 1,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, 1, false, MemoryFlag::NONE, L"TemporalAAOut1");

	// Create constant buffers
	m_cbBasePass = ConstantBuffer::MakeUnique();
	N_RETURN(m_cbBasePass->Create(m_device.get(), sizeof(CBBasePass[FrameCount]), FrameCount,
		nullptr, MemoryType::UPLOAD, MemoryFlag::NONE, L"CBBasePass"), false);

	m_cbPerFrame = ConstantBuffer::MakeUnique();
	N_RETURN(m_cbPerFrame->Create(m_device.get(), sizeof(CBPerFrame[FrameCount]), FrameCount,
		nullptr, MemoryType::UPLOAD, MemoryFlag::NONE, L"CBPerFrame"), false);

	// Create pipelines
	N_RETURN(createInputLayout(), false);
	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(rtFormat), false);
	N_RETURN(createDescriptorTables(), false);

	return true;
}

bool Renderer::SetLightProbes(const Descriptor& irradiance, const Descriptor& radiance)
{
	const Descriptor descriptors[] = { radiance, irradiance };
	const auto descriptorTable = Util::DescriptorTable::MakeUnique();
	descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
	X_RETURN(m_srvTables[SRV_TABLE_BASE], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);

	return true;
}

bool Renderer::SetLightProbesGT(const Descriptor& irradiance, const Descriptor& radiance)
{
	const Descriptor descriptors[] = { radiance, irradiance };
	const auto descriptorTable = Util::DescriptorTable::MakeUnique();
	descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
	X_RETURN(m_srvTables[SRV_TABLE_GT], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);

	return true;
}

bool Renderer::SetLightProbesSH(const StructuredBuffer::sptr& coeffSH)
{
	m_coeffSH = coeffSH;

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

void Renderer::UpdateFrame(uint8_t frameIndex, CXMVECTOR eyePt, CXMMATRIX viewProj, float glossy, bool isPaused)
{
	{
		static auto angle = 0.0f;
		angle += !isPaused ? 0.1f * XM_PI / 180.0f : 0.0f;
		const auto rot = XMMatrixRotationY(angle);

		const auto world = XMMatrixScaling(m_posScale.w, m_posScale.w, m_posScale.w) * rot *
			XMMatrixTranslation(m_posScale.x, m_posScale.y, m_posScale.z);

		const auto halton = IncrementalHalton();
		XMFLOAT2 jitter =
		{
			(halton.x * 2.0f - 1.0f) / m_viewport.x,
			(halton.y * 2.0f - 1.0f) / m_viewport.y
		};

		const auto pCbData = reinterpret_cast<CBBasePass*>(m_cbBasePass->Map(frameIndex));
		pCbData->ProjBias = jitter;
		pCbData->WorldViewProjPrev = m_worldViewProj;
		XMStoreFloat4x4(&pCbData->WorldViewProj, XMMatrixTranspose(world * viewProj));
		XMStoreFloat3x4(&pCbData->World, world);
		m_worldViewProj = pCbData->WorldViewProj;
	}

	{
		const auto pCbData = reinterpret_cast<CBPerFrame*>(m_cbPerFrame->Map(frameIndex));
		const auto projToWorld = XMMatrixInverse(nullptr, viewProj);
		XMStoreFloat4x4(&pCbData->ScreenToWorld, XMMatrixTranspose(projToWorld));
		XMStoreFloat4(&pCbData->EyePtGlossy, eyePt);
		pCbData->EyePtGlossy.w = glossy;
	}

	m_frameParity = !m_frameParity;
}

void Renderer::Render(const CommandList* pCommandList, uint8_t frameIndex, ResourceBarrier* barriers,
	uint32_t numBarriers, RenderMode mode, bool needClear)
{
	numBarriers = m_renderTargets[RT_COLOR]->SetBarrier(barriers, ResourceState::RENDER_TARGET, numBarriers);
	numBarriers = m_renderTargets[RT_VELOCITY]->SetBarrier(barriers, ResourceState::RENDER_TARGET, numBarriers);
	numBarriers = m_depth->SetBarrier(barriers, ResourceState::DEPTH_WRITE, numBarriers);
	numBarriers = m_outputViews[UAV_PP_TAA + !m_frameParity]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE |
		ResourceState::PIXEL_SHADER_RESOURCE, numBarriers, BARRIER_ALL_SUBRESOURCES, BarrierFlag::BEGIN_ONLY);
	if (mode == SH_APPROX) numBarriers = m_coeffSH->SetBarrier(barriers, ResourceState::PIXEL_SHADER_RESOURCE, numBarriers);
	pCommandList->Barrier(numBarriers, barriers);
	render(pCommandList, frameIndex, mode, needClear);

	numBarriers = m_renderTargets[RT_VELOCITY]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE,
		0, BARRIER_ALL_SUBRESOURCES, BarrierFlag::BEGIN_ONLY);
	pCommandList->Barrier(numBarriers, barriers);

	environment(pCommandList, frameIndex);
	temporalAA(pCommandList);
}

void Renderer::Postprocess(const CommandList* pCommandList, const Descriptor& rtv,
	uint32_t numBarriers, ResourceBarrier* pBarriers)
{
	numBarriers = m_outputViews[UAV_PP_TAA + m_frameParity]->SetBarrier(
		pBarriers, ResourceState::NON_PIXEL_SHADER_RESOURCE |
		ResourceState::PIXEL_SHADER_RESOURCE, numBarriers);
	pCommandList->Barrier(numBarriers, pBarriers);

	// Set render target
	pCommandList->OMSetRenderTargets(1, &rtv);

	// Set descriptor tables
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[POSTPROCESS]);
	pCommandList->SetGraphicsDescriptorTable(0, m_srvTables[SRV_TABLE_PP + m_frameParity]);

	// Set pipeline state
	pCommandList->SetPipelineState(m_pipelines[POSTPROCESS]);

	// Set viewport
	Viewport viewport(0.0f, 0.0f, static_cast<float>(m_viewport.x), static_cast<float>(m_viewport.y));
	RectRange scissorRect(0, 0, m_viewport.x, m_viewport.y);
	pCommandList->RSSetViewports(1, &viewport);
	pCommandList->RSSetScissorRects(1, &scissorRect);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);
	pCommandList->Draw(3, 1, 0, 0);
}

bool Renderer::createVB(CommandList* pCommandList, uint32_t numVert,
	uint32_t stride, const uint8_t* pData, vector<Resource::uptr>& uploaders)
{
	m_vertexBuffer = VertexBuffer::MakeUnique();
	N_RETURN(m_vertexBuffer->Create(m_device.get(), numVert, stride,
		ResourceFlag::NONE, MemoryType::DEFAULT, 1, nullptr, 1, nullptr,
		1, nullptr, MemoryFlag::NONE, L"MeshVB"), false);
	uploaders.emplace_back(Resource::MakeUnique());

	return m_vertexBuffer->Upload(pCommandList, uploaders.back().get(), pData, stride * numVert);
}

bool Renderer::createIB(CommandList* pCommandList, uint32_t numIndices,
	const uint32_t* pData, vector<Resource::uptr>& uploaders)
{
	m_numIndices = numIndices;

	const uint32_t byteWidth = sizeof(uint32_t) * numIndices;
	m_indexBuffer = IndexBuffer::MakeUnique();
	N_RETURN(m_indexBuffer->Create(m_device.get(), byteWidth, Format::R32_UINT, ResourceFlag::NONE,
		MemoryType::DEFAULT, 1, nullptr, 1, nullptr, 1, nullptr, MemoryFlag::NONE, L"MeshIB"), false);
	uploaders.emplace_back(Resource::MakeUnique());

	return m_indexBuffer->Upload(pCommandList, uploaders.back().get(), pData, byteWidth);
}

bool Renderer::createInputLayout()
{
	// Define the vertex input layout.
	const InputElement inputElements[] =
	{
		{ "POSITION",	0, Format::R32G32B32_FLOAT, 0, 0,								InputClassification::PER_VERTEX_DATA, 0 },
		{ "NORMAL",		0, Format::R32G32B32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,	InputClassification::PER_VERTEX_DATA, 0 }
	};

	X_RETURN(m_pInputLayout, m_graphicsPipelineCache->CreateInputLayout(inputElements, static_cast<uint32_t>(size(inputElements))), false);

	return true;
}

bool Renderer::createPipelineLayouts()
{
	// This is a pipeline layout for base pass
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRootCBV(VS_CONSTANTS, 0, 0, Shader::Stage::VS);
		pipelineLayout->SetRootCBV(PS_CONSTANTS, 0, 0, Shader::Stage::PS);
		pipelineLayout->SetRange(SHADER_RESOURCES, DescriptorType::SRV, 2, 0);
		pipelineLayout->SetShaderStage(SHADER_RESOURCES, Shader::PS);
		pipelineLayout->SetRange(SAMPLER, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout->SetShaderStage(SAMPLER, Shader::PS);
		X_RETURN(m_pipelineLayouts[BASE_PASS], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, L"BasePassLayout"), false);
	}

	// This is a pipeline layout for base pass SH
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRootCBV(VS_CONSTANTS, 0, 0, Shader::Stage::VS);
		pipelineLayout->SetRootCBV(PS_CONSTANTS, 0, 0, Shader::Stage::PS);
		pipelineLayout->SetRange(SHADER_RESOURCES, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetShaderStage(SHADER_RESOURCES, Shader::PS);
		pipelineLayout->SetRootSRV(BUFFER, 1, 0, DescriptorFlag::NONE, Shader::Stage::PS);
		pipelineLayout->SetRange(SAMPLER, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout->SetShaderStage(SAMPLER, Shader::PS);
		X_RETURN(m_pipelineLayouts[BASE_PASS_SH], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT, L"BasePassLayout"), false);
	}

	// This is a pipeline layout for drawing environment
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRootCBV(PS_CONSTANTS, 0, 0, Shader::Stage::PS);
		pipelineLayout->SetRange(SHADER_RESOURCES, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetShaderStage(SHADER_RESOURCES, Shader::Stage::PS);
		pipelineLayout->SetRange(SAMPLER, DescriptorType::SAMPLER, 1, 0);
		pipelineLayout->SetShaderStage(SAMPLER, Shader::PS);
		X_RETURN(m_pipelineLayouts[ENVIRONMENT], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"EnvironmentLayout"), false);
	}

	// This is a pipeline layout for temporal AA
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(OUTPUT_VIEW, DescriptorType::UAV, 1, 0, 0,
			DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		pipelineLayout->SetRange(SHADER_RESOURCES, DescriptorType::SRV, 3, 0);
		pipelineLayout->SetRange(SAMPLER, DescriptorType::SAMPLER, 1, 0);
		X_RETURN(m_pipelineLayouts[TEMPORAL_AA], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"TemporalAALayout"), false);
	}

	// This is a pipeline layout for postprocess
	{
		const auto pipelineLayout = Util::PipelineLayout::MakeUnique();
		pipelineLayout->SetRange(0, DescriptorType::SRV, 1, 0);
		pipelineLayout->SetShaderStage(0, Shader::Stage::PS);
		X_RETURN(m_pipelineLayouts[POSTPROCESS], pipelineLayout->GetPipelineLayout(m_pipelineLayoutCache.get(),
			PipelineLayoutFlag::NONE, L"PostprocessLayout"), false);
	}

	return true;
}

bool Renderer::createPipelines(Format rtFormat)
{
	auto vsIndex = 0u;
	auto psIndex = 0u;
	auto csIndex = 0u;

	// Base pass
	N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSBasePass.cso"), false);
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSBasePass.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->IASetInputLayout(m_pInputLayout);
		state->SetPipelineLayout(m_pipelineLayouts[BASE_PASS]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->OMSetNumRenderTargets(NUM_RENDER_TARGET);
		state->OMSetRTVFormat(RT_COLOR, Format::R16G16B16A16_FLOAT);
		state->OMSetRTVFormat(RT_VELOCITY, Format::R16G16_FLOAT);
		state->OMSetDSVFormat(Format::D24_UNORM_S8_UINT);
		X_RETURN(m_pipelines[BASE_PASS], state->GetPipeline(m_graphicsPipelineCache.get(), L"BasePass"), false);
	}

	// Base pass SH
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSBasePassSH.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->IASetInputLayout(m_pInputLayout);
		state->SetPipelineLayout(m_pipelineLayouts[BASE_PASS_SH]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex++));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->OMSetNumRenderTargets(NUM_RENDER_TARGET);
		state->OMSetRTVFormat(RT_COLOR, Format::R16G16B16A16_FLOAT);
		state->OMSetRTVFormat(RT_VELOCITY, Format::R16G16_FLOAT);
		state->OMSetDSVFormat(Format::D24_UNORM_S8_UINT);
		X_RETURN(m_pipelines[BASE_PASS_SH], state->GetPipeline(m_graphicsPipelineCache.get(), L"BasePass"), false);
	}

	// Environment
	N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSEnvironment.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[ENVIRONMENT]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->DSSetState(Graphics::DEPTH_READ_LESS_EQUAL, m_graphicsPipelineCache.get());
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->OMSetNumRenderTargets(1);
		state->OMSetRTVFormat(RT_COLOR, Format::R16G16B16A16_FLOAT);
		state->OMSetDSVFormat(Format::D24_UNORM_S8_UINT);
		X_RETURN(m_pipelines[ENVIRONMENT], state->GetPipeline(m_graphicsPipelineCache.get(), L"Environment"), false);
	}

	// Temporal AA
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSTemporalAA.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[TEMPORAL_AA]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[TEMPORAL_AA], state->GetPipeline(m_computePipelineCache.get(), L"TemporalAA"), false);
	}

	// Postprocess
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSPostprocess.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[POSTPROCESS]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex));
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->OMSetNumRenderTargets(1);
		state->OMSetRTVFormat(0, rtFormat);
		X_RETURN(m_pipelines[POSTPROCESS], state->GetPipeline(m_graphicsPipelineCache.get(), L"Postprocess"), false);
	}

	return true;
}

bool Renderer::createDescriptorTables()
{
	// Temporal AA output UAVs
	for (auto i = 0u; i < 2; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_outputViews[UAV_PP_TAA + i]->GetUAV());
		X_RETURN(m_uavTables[UAV_TABLE_TAA + i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Temporal AA input SRVs
	for (auto i = 0u; i < 2; ++i)
	{
		const Descriptor descriptors[] =
		{
			m_renderTargets[RT_COLOR]->GetSRV(),
			m_outputViews[UAV_PP_TAA + !i]->GetSRV(),
			m_renderTargets[RT_VELOCITY]->GetSRV()
		};
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_srvTables[SRV_TABLE_TAA + i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Postprocess SRVs
	for (auto i = 0u; i < 2; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_outputViews[UAV_PP_TAA + i]->GetSRV());
		X_RETURN(m_srvTables[SRV_TABLE_PP + i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// RTV table
	{
		const Descriptor descriptors[] =
		{
			m_renderTargets[RT_COLOR]->GetRTV(),
			m_renderTargets[RT_VELOCITY]->GetRTV()
		};
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		m_framebuffer = descriptorTable->GetFramebuffer(m_descriptorTableCache.get(), &m_depth->GetDSV());
	}

	// Create the sampler
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		const auto samplerAnisoWrap = SamplerPreset::ANISOTROPIC_WRAP;
		descriptorTable->SetSamplers(0, 1, &samplerAnisoWrap, m_descriptorTableCache.get());
		X_RETURN(m_samplerTable, descriptorTable->GetSamplerTable(m_descriptorTableCache.get()), false);
	}

	return true;
}

void Renderer::render(const CommandList* pCommandList, uint8_t frameIndex, RenderMode mode, bool needClear)
{
	// Set framebuffer
	pCommandList->OMSetFramebuffer(m_framebuffer);

	// Clear render target
	const float clearColor[4] = { 0.2f, 0.2f, 0.7f, 0.0f };
	const float clearColorNull[4] = {};
	if (needClear) pCommandList->ClearRenderTargetView(m_renderTargets[RT_COLOR]->GetRTV(), clearColor);
	pCommandList->ClearRenderTargetView(m_renderTargets[RT_VELOCITY]->GetRTV(), clearColorNull);
	pCommandList->ClearDepthStencilView(m_depth->GetDSV(), ClearFlag::DEPTH, 1.0f);

	// Set pipeline state
	const auto pipeIdx = mode == SH_APPROX ? BASE_PASS_SH : BASE_PASS;
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[pipeIdx]);
	pCommandList->SetPipelineState(m_pipelines[pipeIdx]);

	// Set viewport
	Viewport viewport(0.0f, 0.0f, static_cast<float>(m_viewport.x), static_cast<float>(m_viewport.y));
	RectRange scissorRect(0, 0, m_viewport.x, m_viewport.y);
	pCommandList->RSSetViewports(1, &viewport);
	pCommandList->RSSetScissorRects(1, &scissorRect);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);

	// Set descriptor tables
	pCommandList->SetGraphicsRootConstantBufferView(VS_CONSTANTS, m_cbBasePass.get(), m_cbBasePass->GetCBVOffset(frameIndex));
	pCommandList->SetGraphicsRootConstantBufferView(PS_CONSTANTS, m_cbPerFrame.get(), m_cbPerFrame->GetCBVOffset(frameIndex));
	pCommandList->SetGraphicsDescriptorTable(SHADER_RESOURCES, m_srvTables[mode == GROUND_TRUTH ? SRV_TABLE_GT : SRV_TABLE_BASE]);
	pCommandList->SetGraphicsDescriptorTable(SAMPLER, m_samplerTable);
	if (mode == SH_APPROX) pCommandList->SetGraphicsRootShaderResourceView(BUFFER, m_coeffSH.get());

	pCommandList->IASetVertexBuffers(0, 1, &m_vertexBuffer->GetVBV());
	pCommandList->IASetIndexBuffer(m_indexBuffer->GetIBV());

	pCommandList->DrawIndexed(m_numIndices, 1, 0, 0, 0);
}

void Renderer::environment(const CommandList* pCommandList, uint8_t frameIndex)
{
	// Set render target
	pCommandList->OMSetRenderTargets(1, &m_renderTargets[RT_COLOR]->GetRTV(), &m_depth->GetDSV());

	// Set descriptor tables
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[ENVIRONMENT]);
	pCommandList->SetGraphicsRootConstantBufferView(PS_CONSTANTS, m_cbPerFrame.get(), m_cbPerFrame->GetCBVOffset(frameIndex));
	pCommandList->SetGraphicsDescriptorTable(SHADER_RESOURCES, m_srvTables[SRV_TABLE_BASE]);
	pCommandList->SetGraphicsDescriptorTable(SAMPLER, m_samplerTable);

	// Set pipeline state
	pCommandList->SetPipelineState(m_pipelines[ENVIRONMENT]);

	pCommandList->IASetPrimitiveTopology(PrimitiveTopology::TRIANGLELIST);
	pCommandList->Draw(3, 1, 0, 0);
}

void Renderer::temporalAA(const CommandList* pCommandList)
{
	// Bind the heaps, acceleration structure and dispatch rays.
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache->GetDescriptorPool(SAMPLER_POOL)
	};
	pCommandList->SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	ResourceBarrier barriers[4];
	auto numBarriers = m_outputViews[UAV_PP_TAA + m_frameParity]->SetBarrier(barriers, ResourceState::UNORDERED_ACCESS);
	numBarriers = m_renderTargets[RT_COLOR]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE, numBarriers);
	numBarriers = m_outputViews[UAV_PP_TAA + !m_frameParity]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE |
		ResourceState::PIXEL_SHADER_RESOURCE, numBarriers, BARRIER_ALL_SUBRESOURCES, BarrierFlag::END_ONLY);
	numBarriers = m_renderTargets[RT_VELOCITY]->SetBarrier(barriers, ResourceState::NON_PIXEL_SHADER_RESOURCE,
		numBarriers, BARRIER_ALL_SUBRESOURCES, BarrierFlag::END_ONLY);
	pCommandList->Barrier(numBarriers, barriers);

	// Set descriptor tables
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[TEMPORAL_AA]);
	pCommandList->SetComputeDescriptorTable(OUTPUT_VIEW, m_uavTables[UAV_TABLE_TAA + m_frameParity]);
	pCommandList->SetComputeDescriptorTable(SHADER_RESOURCES, m_srvTables[SRV_TABLE_TAA + m_frameParity]);
	pCommandList->SetComputeDescriptorTable(SAMPLER, m_samplerTable);

	// Set pipeline state
	pCommandList->SetPipelineState(m_pipelines[TEMPORAL_AA]);
	pCommandList->Dispatch(DIV_UP(m_viewport.x, 8), DIV_UP(m_viewport.y, 8), 1);
}
