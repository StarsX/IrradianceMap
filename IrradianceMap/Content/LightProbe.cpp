//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "LightProbe.h"
#include "Advanced/XUSGSHSharedConsts.h"
#define _INDEPENDENT_DDS_LOADER_
#include "Advanced/XUSGDDSLoader.h"
#undef _INDEPENDENT_DDS_LOADER_

using namespace std;
using namespace DirectX;
using namespace XUSG;

struct CBImmutable
{
	float		MapSize;
	uint32_t	NumLevels;
};

LightProbe::LightProbe() :
	m_groundTruth(nullptr)
{
	m_shaderPool = ShaderPool::MakeShared();
}

LightProbe::~LightProbe()
{
}

bool LightProbe::Init(CommandList* pCommandList, const DescriptorTableCache::sptr& descriptorTableCache,
	vector<Resource::uptr>& uploaders, const wstring pFileNames[], uint32_t numFiles, bool typedUAV)
{
	const auto pDevice = pCommandList->GetDevice();
	m_graphicsPipelineCache = Graphics::PipelineCache::MakeUnique(pDevice);
	m_computePipelineCache = Compute::PipelineCache::MakeShared(pDevice);
	m_pipelineLayoutCache = PipelineLayoutCache::MakeShared(pDevice);
	m_descriptorTableCache = descriptorTableCache;

	// Load input image
	auto texWidth = 1u, texHeight = 1u;
	m_sources.resize(numFiles);
	for (auto i = 0u; i < numFiles; ++i)
	{
		DDS::Loader textureLoader;
		DDS::AlphaMode alphaMode;

		uploaders.emplace_back(Resource::MakeUnique());
		XUSG_N_RETURN(textureLoader.CreateTextureFromFile(pCommandList, pFileNames[i].c_str(),
			8192, false, m_sources[i], uploaders.back().get(), &alphaMode), false);

		texWidth = (max)(static_cast<uint32_t>(m_sources[i]->GetWidth()), texWidth);
		texHeight = (max)(m_sources[i]->GetHeight(), texHeight);
	}

	// Create resources and pipelines
	CBImmutable cb;
	cb.NumLevels = CalculateMipLevels(texWidth, texHeight);
	cb.MapSize = (texWidth + texHeight) * 0.5f;

	const auto format = Format::R11G11B10_FLOAT;
	m_irradiance = RenderTarget::MakeUnique();
	XUSG_N_RETURN(m_irradiance->Create(pDevice, texWidth, texHeight, format, 6,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, cb.NumLevels, 1,
		nullptr, true, MemoryFlag::NONE, L"Irradiance"), false);

	m_radiance = RenderTarget::MakeUnique();
	XUSG_N_RETURN(m_radiance->Create(pDevice, texWidth, texHeight, format, 6,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, 1, 1, nullptr, true,
		MemoryFlag::NONE, L"Radiance"), false);

	// Create constant buffers
	m_cbImmutable = ConstantBuffer::MakeUnique();
	XUSG_N_RETURN(m_cbImmutable->Create(pDevice, sizeof(CBImmutable), 1,
		nullptr, MemoryType::UPLOAD, MemoryFlag::NONE, L"CBImmutable"), false);
	*reinterpret_cast<CBImmutable*>(m_cbImmutable->Map()) = cb;

	m_cbPerFrame = ConstantBuffer::MakeUnique();
	XUSG_N_RETURN(m_cbPerFrame->Create(pDevice, sizeof(float[FrameCount]), FrameCount,
		nullptr, MemoryType::UPLOAD, MemoryFlag::NONE, L"CBPerFrame"), false);

	XUSG_N_RETURN(createPipelineLayouts(), false);
	XUSG_N_RETURN(createPipelines(format, typedUAV), false);

	return true;
}

bool LightProbe::CreateDescriptorTables(Device* pDevice)
{
	m_sphericalHarmonics = SphericalHarmonics::MakeUnique();
	XUSG_N_RETURN(m_sphericalHarmonics->Init(pDevice, m_shaderPool, m_computePipelineCache,
		m_pipelineLayoutCache, m_descriptorTableCache, 0), false);

	return createDescriptorTables();
}

void LightProbe::UpdateFrame(double time, uint8_t frameIndex)
{
	// Update per-frame CB
	{
		static const auto period = 3.0;
		const auto numSources = static_cast<uint32_t>(m_srvTables[TABLE_RADIANCE].size());
		auto blend = static_cast<float>(time / period);
		m_inputProbeIdx = static_cast<uint32_t>(time / period);
		blend = numSources > 1 ? blend - m_inputProbeIdx : 0.0f;
		m_inputProbeIdx %= numSources;
		*reinterpret_cast<float*>(m_cbPerFrame->Map(frameIndex)) = blend;
	}
}

void LightProbe::Process(CommandList* pCommandList, uint8_t frameIndex, PipelineType pipelineType)
{
	// Set Descriptor pools
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache->GetDescriptorPool(SAMPLER_POOL)
	};
	pCommandList->SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	ResourceBarrier barriers[13];
	uint32_t numBarriers;

	switch (pipelineType)
	{
	case GRAPHICS:
		generateRadianceGraphics(pCommandList, frameIndex);
		numBarriers = generateMipsGraphics(pCommandList, barriers);
		upsampleGraphics(pCommandList, barriers, numBarriers);
		break;
	case COMPUTE:
		if (m_pipelines[UP_SAMPLE_INPLACE])
		{
			generateRadianceCompute(pCommandList, frameIndex);
			numBarriers = generateMipsCompute(pCommandList, barriers);
			upsampleCompute(pCommandList, barriers, numBarriers);
			break;
		}
	case SH:
	{
		generateRadianceCompute(pCommandList, frameIndex);
		m_sphericalHarmonics->Transform(pCommandList, m_radiance.get(), m_srvTables[TABLE_RESAMPLE][0]);
		break;
	}
	default:
		generateRadianceCompute(pCommandList, frameIndex);
		numBarriers = generateMipsCompute(pCommandList, barriers);
		upsampleGraphics(pCommandList, barriers, numBarriers);
	}
}

const ShaderResource* LightProbe::GetIrradianceGT(CommandList* pCommandList,
	const wchar_t* fileName, vector<Resource::uptr>* pUploaders)
{
	if (!m_groundTruth && fileName && pUploaders)
	{
		DDS::Loader textureLoader;
		DDS::AlphaMode alphaMode;

		pUploaders->emplace_back(Resource::MakeUnique());
		XUSG_N_RETURN(textureLoader.CreateTextureFromFile(pCommandList, fileName,
			8192, false, m_groundTruth, pUploaders->back().get(), &alphaMode), nullptr);
	}

	return m_groundTruth.get();
}

Texture2D* LightProbe::GetIrradiance() const
{
	return m_irradiance.get();
}

ShaderResource* LightProbe::GetRadiance() const
{
	return m_radiance.get();
}

StructuredBuffer::sptr LightProbe::GetSH() const
{
	return m_sphericalHarmonics->GetSHCoefficients();
}

bool LightProbe::createPipelineLayouts()
{
	// Generate Radiance graphics
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRootCBV(1, 0, 0, Shader::PS);
		utilPipelineLayout->SetConstants(2, XUSG_SizeOfInUint32(uint32_t), 1, 0, Shader::PS);
		utilPipelineLayout->SetRange(3, DescriptorType::SRV, 2, 0);
		utilPipelineLayout->SetShaderStage(0, Shader::PS);
		utilPipelineLayout->SetShaderStage(3, Shader::PS);
		XUSG_X_RETURN(m_pipelineLayouts[GEN_RADIANCE_GRAPHICS], utilPipelineLayout->GetPipelineLayout(
			m_pipelineLayoutCache.get(), PipelineLayoutFlag::NONE, L"RadianceGenerationLayout"), false);
	}

	// Generate Radiance compute
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRootCBV(1, 0);
		utilPipelineLayout->SetRange(2, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout->SetRange(3, DescriptorType::SRV, 2, 0);
		XUSG_X_RETURN(m_pipelineLayouts[GEN_RADIANCE_COMPUTE], utilPipelineLayout->GetPipelineLayout(
			m_pipelineLayoutCache.get(), PipelineLayoutFlag::NONE, L"RadianceGenerationLayout"), false);
	}

	// Resampling graphics
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRange(1, DescriptorType::SRV, 1, 0);
		utilPipelineLayout->SetConstants(2, XUSG_SizeOfInUint32(uint32_t), 0, 0, Shader::PS);
		utilPipelineLayout->SetShaderStage(0, Shader::PS);
		utilPipelineLayout->SetShaderStage(1, Shader::PS);
		XUSG_X_RETURN(m_pipelineLayouts[RESAMPLE_GRAPHICS], utilPipelineLayout->GetPipelineLayout(
			m_pipelineLayoutCache.get(), PipelineLayoutFlag::NONE, L"ResamplingGraphicsLayout"), false);
	}

	// Resampling compute
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRange(1, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout->SetRange(2, DescriptorType::SRV, 1, 0);
		XUSG_X_RETURN(m_pipelineLayouts[RESAMPLE_COMPUTE], utilPipelineLayout->GetPipelineLayout(
			m_pipelineLayoutCache.get(), PipelineLayoutFlag::NONE, L"ResamplingComputeLayout"), false);
	}

	// Up sampling graphics, with alpha blending
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRange(1, DescriptorType::SRV, 1, 0);
		utilPipelineLayout->SetConstants(2, XUSG_SizeOfInUint32(uint32_t[2]), 0, 0, Shader::PS);
		utilPipelineLayout->SetRootCBV(3, 1, 0, Shader::PS);
		utilPipelineLayout->SetShaderStage(0, Shader::PS);
		utilPipelineLayout->SetShaderStage(1, Shader::PS);
		XUSG_X_RETURN(m_pipelineLayouts[UP_SAMPLE_BLEND], utilPipelineLayout->GetPipelineLayout(
			m_pipelineLayoutCache.get(), PipelineLayoutFlag::NONE, L"UpSamplingGraphicsLayout"), false);
	}

	// Up sampling compute, in-place
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRange(1, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout->SetRange(2, DescriptorType::SRV, 1, 0);
		utilPipelineLayout->SetConstants(3, XUSG_SizeOfInUint32(uint32_t), 0);
		utilPipelineLayout->SetRootCBV(4, 1);
		XUSG_X_RETURN(m_pipelineLayouts[UP_SAMPLE_INPLACE], utilPipelineLayout->GetPipelineLayout(
			m_pipelineLayoutCache.get(), PipelineLayoutFlag::NONE, L"UpSamplingComputeLayout"), false);
	}

	// Up sampling graphics, for the final pass
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRange(1, DescriptorType::SRV, 2, 0);
		utilPipelineLayout->SetConstants(2, XUSG_SizeOfInUint32(uint32_t[2]), 0, 0, Shader::PS);
		utilPipelineLayout->SetRootCBV(3, 1, 0, Shader::PS);
		utilPipelineLayout->SetShaderStage(0, Shader::PS);
		utilPipelineLayout->SetShaderStage(1, Shader::PS);
		XUSG_X_RETURN(m_pipelineLayouts[FINAL_G], utilPipelineLayout->GetPipelineLayout(
			m_pipelineLayoutCache.get(), PipelineLayoutFlag::NONE, L"FinalPassGraphicsLayout"), false);
	}

	// Up sampling compute, for the final pass
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRange(1, DescriptorType::UAV, 1, 0, 0, DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout->SetRange(2, DescriptorType::SRV, 2, 0);
		utilPipelineLayout->SetConstants(3, XUSG_SizeOfInUint32(uint32_t), 0);
		utilPipelineLayout->SetRootCBV(4, 1);
		XUSG_X_RETURN(m_pipelineLayouts[FINAL_C], utilPipelineLayout->GetPipelineLayout(
			m_pipelineLayoutCache.get(), PipelineLayoutFlag::NONE, L"FinalPassComputeLayout"), false);
	}

	return true;
}

bool LightProbe::createPipelines(Format rtFormat, bool typedUAV)
{
	auto vsIndex = 0u;
	auto psIndex = 0u;

	// Generate Radiance graphics
	XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSGenRadiance.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[GEN_RADIANCE_GRAPHICS]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->OMSetNumRenderTargets(1);
		state->OMSetRTVFormat(0, rtFormat);
		XUSG_X_RETURN(m_pipelines[GEN_RADIANCE_GRAPHICS], state->GetPipeline(m_graphicsPipelineCache.get(), L"RadianceGeneration_graphics"), false);
	}

	// Generate Radiance compute
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, CS_GEN_RADIANCE, L"CSGenRadiance.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[GEN_RADIANCE_COMPUTE]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, CS_GEN_RADIANCE));
		XUSG_X_RETURN(m_pipelines[GEN_RADIANCE_COMPUTE], state->GetPipeline(m_computePipelineCache.get(), L"RadianceGeneration_compute"), false);
	}

	// Resampling graphics
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSResample.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RESAMPLE_GRAPHICS]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->OMSetNumRenderTargets(1);
		state->OMSetRTVFormat(0, rtFormat);
		XUSG_X_RETURN(m_pipelines[RESAMPLE_GRAPHICS], state->GetPipeline(m_graphicsPipelineCache.get(), L"Resampling)graphics"), false);
	}

	// Resampling compute
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, CS_RESAMPLE, L"CSResample.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RESAMPLE_COMPUTE]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, CS_RESAMPLE));
		XUSG_X_RETURN(m_pipelines[RESAMPLE_COMPUTE], state->GetPipeline(m_computePipelineCache.get(), L"Resampling_compute"), false);
	}

	// Up sampling graphics, with alpha blending
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSCosUp_blend.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[UP_SAMPLE_BLEND]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->OMSetBlendState(Graphics::NON_PRE_MUL, m_graphicsPipelineCache.get());
		state->OMSetNumRenderTargets(1);
		state->OMSetRTVFormat(0, rtFormat);
		XUSG_X_RETURN(m_pipelines[UP_SAMPLE_BLEND], state->GetPipeline(m_graphicsPipelineCache.get(), L"UpSampling_alpha_blend"), false);
	}

	// Up sampling compute
	if (typedUAV)
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, CS_UP_SAMPLE, L"CSCosUp_in_place.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[UP_SAMPLE_INPLACE]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, CS_UP_SAMPLE));
		XUSG_X_RETURN(m_pipelines[UP_SAMPLE_INPLACE], state->GetPipeline(m_computePipelineCache.get(), L"UpSampling_in_place"), false);
	}

	// Up sampling graphics, for the final pass
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSCosineUp.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[FINAL_G]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex));
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache.get());
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->OMSetNumRenderTargets(1);
		state->OMSetRTVFormat(0, rtFormat);
		XUSG_X_RETURN(m_pipelines[FINAL_G], state->GetPipeline(m_graphicsPipelineCache.get(), L"UpSampling_graphics"), false);
	}

	// Up sampling compute, for the final pass
	{
		XUSG_N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, CS_FINAL, L"CSCosineUp.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[FINAL_C]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, CS_FINAL));
		XUSG_X_RETURN(m_pipelines[FINAL_C], state->GetPipeline(m_computePipelineCache.get(), L"UpSampling_compute"), false);
	}

	return true;
}

bool LightProbe::createDescriptorTables()
{
	const auto numMips = m_irradiance->GetNumMips();

	// Get UAV table for radiance generation
	m_uavTables[TABLE_RADIANCE].resize(1);
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_radiance->GetUAV());
		XUSG_X_RETURN(m_uavTables[TABLE_RADIANCE][0], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Get SRV tables for radiance generation
	const auto numSources = static_cast<uint32_t>(m_sources.size());
	m_srvTables[TABLE_RADIANCE].resize(m_sources.size());
	for (auto i = 0u; i + 1 < numSources; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_sources[i]->GetSRV());
		XUSG_X_RETURN(m_srvTables[TABLE_RADIANCE][i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}
	{
		const auto i = numSources - 1;
		const Descriptor descriptors[] =
		{
			m_sources[i]->GetSRV(),
			m_sources[0]->GetSRV()
		};
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		XUSG_X_RETURN(m_srvTables[TABLE_RADIANCE][i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Get UAVs for resampling
	m_uavTables[TABLE_RESAMPLE].resize(numMips);
	for (uint8_t i = 0; i < numMips; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_irradiance->GetUAV(i));
		XUSG_X_RETURN(m_uavTables[TABLE_RESAMPLE][i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Get SRVs for resampling
	m_srvTables[TABLE_RESAMPLE].resize(numMips);
	for (uint8_t i = 0; i < numMips; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, i ? &m_irradiance->GetSRVLevel(i) : &m_radiance->GetSRV());
		XUSG_X_RETURN(m_srvTables[TABLE_RESAMPLE][i], descriptorTable->GetCbvSrvUavTable(m_descriptorTableCache.get()), false);
	}

	// Create the sampler table
	const auto descriptorTable = Util::DescriptorTable::MakeUnique();
	const auto sampler = LINEAR_WRAP;
	descriptorTable->SetSamplers(0, 1, &sampler, m_descriptorTableCache.get());
	XUSG_X_RETURN(m_samplerTable, descriptorTable->GetSamplerTable(m_descriptorTableCache.get()), false);

	return true;
}

uint32_t LightProbe::generateMipsGraphics(CommandList* pCommandList, ResourceBarrier* pBarriers)
{
	const auto numBarriers = m_radiance->SetBarrier(pBarriers, ResourceState::PIXEL_SHADER_RESOURCE);
	return m_irradiance->GenerateMips(pCommandList, pBarriers,
		ResourceState::PIXEL_SHADER_RESOURCE, m_pipelineLayouts[RESAMPLE_GRAPHICS],
		m_pipelines[RESAMPLE_GRAPHICS], &m_srvTables[TABLE_RESAMPLE][0],
		1, m_samplerTable, 0, numBarriers);
}

uint32_t LightProbe::generateMipsCompute(CommandList* pCommandList, ResourceBarrier* pBarriers)
{
	auto numBarriers = m_radiance->SetBarrier(pBarriers,
		ResourceState::NON_PIXEL_SHADER_RESOURCE | ResourceState::PIXEL_SHADER_RESOURCE);
	numBarriers = m_irradiance->AsTexture()->GenerateMips(pCommandList, pBarriers, 8, 8, 1,
		ResourceState::NON_PIXEL_SHADER_RESOURCE, m_pipelineLayouts[RESAMPLE_COMPUTE],
		m_pipelines[RESAMPLE_COMPUTE], &m_uavTables[TABLE_RESAMPLE][1], 1, m_samplerTable,
		0, numBarriers, &m_srvTables[TABLE_RESAMPLE][0], 2);

	// Handle inconsistent barrier states
	assert(numBarriers >= CubeMapFaceCount);
	numBarriers -= CubeMapFaceCount;
	for (uint8_t i = 0; i < CubeMapFaceCount; ++i)
		// Adjust the state record only
		m_irradiance->SetBarrier(pBarriers, m_irradiance->GetNumMips() - 1,
			ResourceState::UNORDERED_ACCESS, numBarriers, i);

	return numBarriers;
}

void LightProbe::upsampleGraphics(CommandList* pCommandList, ResourceBarrier* pBarriers, uint32_t numBarriers)
{
	// Up sampling
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[UP_SAMPLE_BLEND]);
	pCommandList->SetGraphicsDescriptorTable(0, m_samplerTable);
	pCommandList->SetGraphicsRootConstantBufferView(3, m_cbImmutable.get());
	pCommandList->SetPipelineState(m_pipelines[UP_SAMPLE_BLEND]);

	const uint8_t numPasses = m_irradiance->GetNumMips() - 1;
	for (uint8_t i = 0; i + 1 < numPasses; ++i)
	{
		const auto c = numPasses - i;
		const auto level = c - 1;
		pCommandList->SetGraphics32BitConstant(2, level);
		numBarriers = m_irradiance->Blit(pCommandList, pBarriers, level, c,
			ResourceState::PIXEL_SHADER_RESOURCE, m_srvTables[TABLE_RESAMPLE][c],
			1, numBarriers, 0, 0, XUSG_SizeOfInUint32(level));
	}

	// Final pass
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[FINAL_G]);
	pCommandList->SetGraphicsDescriptorTable(0, m_samplerTable);
	pCommandList->SetGraphicsRootConstantBufferView(3, m_cbImmutable.get());
	pCommandList->SetGraphics32BitConstant(2, 0);
	pCommandList->SetPipelineState(m_pipelines[FINAL_G]);
	numBarriers = m_irradiance->Blit(pCommandList, pBarriers, 0, 1,
		ResourceState::PIXEL_SHADER_RESOURCE, m_srvTables[TABLE_RESAMPLE][0],
		1, numBarriers, 0, 0, XUSG_SizeOfInUint32(uint32_t));
}

void LightProbe::upsampleCompute(CommandList* pCommandList, ResourceBarrier* pBarriers, uint32_t numBarriers)
{
	// Up sampling
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[UP_SAMPLE_INPLACE]);
	pCommandList->SetComputeDescriptorTable(0, m_samplerTable);
	pCommandList->SetComputeRootConstantBufferView(4, m_cbImmutable.get());
	pCommandList->SetPipelineState(m_pipelines[UP_SAMPLE_INPLACE]);

	const uint8_t numPasses = m_irradiance->GetNumMips() - 1;
	for (uint8_t i = 0; i + 1 < numPasses; ++i)
	{
		const auto c = numPasses - i;
		const auto level = c - 1;
		pCommandList->SetCompute32BitConstant(3, level);
		numBarriers = m_irradiance->AsTexture()->Blit(pCommandList, pBarriers, 8, 8, 1, level, c,
			ResourceState::NON_PIXEL_SHADER_RESOURCE | ResourceState::PIXEL_SHADER_RESOURCE,
			m_uavTables[TABLE_RESAMPLE][level], 1, numBarriers,
			m_srvTables[TABLE_RESAMPLE][c], 2);
	}

	// Final pass
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[FINAL_C]);
	pCommandList->SetComputeDescriptorTable(0, m_samplerTable);
	pCommandList->SetComputeRootConstantBufferView(3, m_cbImmutable.get());
	pCommandList->SetCompute32BitConstant(3, 0);
	pCommandList->SetPipelineState(m_pipelines[FINAL_C]);
	numBarriers = m_irradiance->AsTexture()->Blit(pCommandList, pBarriers, 8, 8, 1, 0, 1,
		ResourceState::NON_PIXEL_SHADER_RESOURCE | ResourceState::PIXEL_SHADER_RESOURCE,
		m_uavTables[TABLE_RESAMPLE][0], 1, numBarriers,
		m_srvTables[TABLE_RESAMPLE][0], 2);
}

void LightProbe::generateRadianceGraphics(CommandList* pCommandList, uint8_t frameIndex)
{
	ResourceBarrier barrier;
	const auto numBarriers = m_radiance->SetBarrier(&barrier, ResourceState::RENDER_TARGET);
	pCommandList->Barrier(numBarriers, &barrier);

	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[GEN_RADIANCE_GRAPHICS]);
	pCommandList->SetGraphicsRootConstantBufferView(1, m_cbPerFrame.get(), m_cbPerFrame->GetCBVOffset(frameIndex));

	m_radiance->Blit(pCommandList, m_srvTables[TABLE_RADIANCE][m_inputProbeIdx], 3, 0, 0, 0,
		m_samplerTable, 0, m_pipelines[GEN_RADIANCE_GRAPHICS]);
}

void LightProbe::generateRadianceCompute(CommandList* pCommandList, uint8_t frameIndex)
{
	ResourceBarrier barrier;
	const auto numBarriers = m_radiance->SetBarrier(&barrier, ResourceState::UNORDERED_ACCESS);
	pCommandList->Barrier(numBarriers, &barrier);

	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[GEN_RADIANCE_COMPUTE]);
	pCommandList->SetComputeRootConstantBufferView(1, m_cbPerFrame.get(), m_cbPerFrame->GetCBVOffset(frameIndex));

	m_radiance->AsTexture()->Blit(pCommandList, 8, 8, 1, m_uavTables[TABLE_RADIANCE][0], 2, 0,
		m_srvTables[TABLE_RADIANCE][m_inputProbeIdx], 3, m_samplerTable, 0, m_pipelines[GEN_RADIANCE_COMPUTE]);
}
