//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#include "LightProbe.h"
#define _INDEPENDENT_DDS_LOADER_
#include "Advanced/XUSGDDSLoader.h"
#undef _INDEPENDENT_DDS_LOADER_

using namespace std;
using namespace DirectX;
using namespace XUSG;

struct CosConstants
{
	struct Immutable
	{
		float		MapSize;
		uint32_t	NumLevels;
	} Imm;
	uint32_t Level;
};

LightProbe::LightProbe(const Device& device) :
	m_device(device),
	m_groundTruth(nullptr),
	m_numMips(11)
{
	m_shaderPool = ShaderPool::MakeUnique();
	m_graphicsPipelineCache = Graphics::PipelineCache::MakeUnique(device);
	m_computePipelineCache = Compute::PipelineCache::MakeUnique(device);
	m_pipelineLayoutCache = PipelineLayoutCache::MakeUnique(device);
}

LightProbe::~LightProbe()
{
}

bool LightProbe::Init(CommandList* pCommandList, uint32_t width, uint32_t height,
	const DescriptorTableCache::sptr& descriptorTableCache, vector<Resource>& uploaders,
	const wstring pFileNames[], uint32_t numFiles, bool typedUAV)
{
	m_descriptorTableCache = descriptorTableCache;

	// Load input image
	auto texWidth = 1u, texHeight = 1u;
	m_sources.resize(numFiles);
	for (auto i = 0u; i < numFiles; ++i)
	{
		DDS::Loader textureLoader;
		DDS::AlphaMode alphaMode;

		uploaders.emplace_back();
		N_RETURN(textureLoader.CreateTextureFromFile(m_device, pCommandList, pFileNames[i].c_str(),
			8192, false, m_sources[i], uploaders.back(), &alphaMode), false);

		const auto& desc = m_sources[i]->GetResource()->GetDesc();
		texWidth = (max)(static_cast<uint32_t>(desc.Width), texWidth);
		texHeight = (max)(desc.Height, texHeight);
	}

	// Create resources and pipelines
	m_numMips = (max)(Log2((max)(texWidth, texHeight)), 1ui8) + 1;
	m_mapSize = (texWidth + texHeight) * 0.5f;

	const auto format = Format::R11G11B10_FLOAT;
	m_radiance = RenderTarget::MakeUnique();
	m_radiance->Create(m_device, texWidth, texHeight, format,
		6, ResourceFlag::ALLOW_UNORDERED_ACCESS,
		1, 1, nullptr, true, L"Radiance");

	m_irradiance = RenderTarget::MakeUnique();
	m_irradiance->Create(m_device, texWidth, texHeight, format, 6,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, m_numMips, 1,
		nullptr, true, L"Irradiance");

	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(format, typedUAV), false);
	N_RETURN(createDescriptorTables(), false);

	return true;
}

void LightProbe::UpdateFrame(double time)
{
	m_time = time;
}

void LightProbe::Process(const CommandList* pCommandList, ResourceState dstState, PipelineType pipelineType)
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
		generateRadianceGraphics(pCommandList);
		numBarriers = generateMipsGraphics(pCommandList, barriers);
		upsampleGraphics(pCommandList, barriers, numBarriers);
		break;
	case COMPUTE:
		if (m_pipelines[UP_SAMPLE_INPLACE])
		{
			generateRadianceCompute(pCommandList);
			numBarriers = generateMipsCompute(pCommandList, barriers);
			upsampleCompute(pCommandList, barriers, numBarriers);
			break;
		}
	default:
		generateRadianceCompute(pCommandList);
		numBarriers = generateMipsCompute(pCommandList, barriers, ResourceState::PIXEL_SHADER_RESOURCE) - 6;
		for (auto i = 0ui8; i < 6; ++i)
			numBarriers = m_irradiance->SetBarrier(barriers, m_numMips - 1,
				ResourceState::UNORDERED_ACCESS, numBarriers, i);
		upsampleGraphics(pCommandList, barriers, numBarriers - 6);
	}
}

ResourceBase* LightProbe::GetIrradianceGT(CommandList* pCommandList,
	const wchar_t* fileName, vector<Resource>* pUploaders)
{
	if (!m_groundTruth && fileName && pUploaders)
	{
		DDS::Loader textureLoader;
		DDS::AlphaMode alphaMode;

		pUploaders->emplace_back();
		N_RETURN(textureLoader.CreateTextureFromFile(m_device, pCommandList, fileName,
			8192, false, m_groundTruth, pUploaders->back(), &alphaMode), nullptr);
	}

	return m_groundTruth.get();
}

Texture2D& LightProbe::GetIrradiance()
{
	return *m_irradiance;
}

Texture2D& LightProbe::GetRadiance()
{
	return *m_radiance;
}

Descriptor LightProbe::GetSH(CommandList* pCommandList,
	const wchar_t* fileName, vector<Resource>* pUploaders)
{
	if (pUploaders)
	{
		// Create immutable constant buffer
		m_cbCoeffSH = ConstantBuffer::MakeUnique();
		N_RETURN(m_cbCoeffSH->Create(m_device, sizeof(XMFLOAT4[9]), 1,
			nullptr, MemoryType::DEFAULT, L"cbCoeffSH"), Descriptor());

		// Initialize the data
		const float fCoeffSH[3][9] =
		{
			3.17645f, -3.70829f, -0.0266615f, 0.0814423f, -0.186518f, 0.0948620f, -2.79015f, -0.00564191f, -2.41370f,
			3.08304f, -3.67873f, -0.0241826f, 0.0842786f, -0.188335f, 0.0879189f, -2.75383f, -0.00393165f, -2.40570f,
			3.50625f, -4.27443f, -0.0307949f, 0.1134770f, -0.246335f, 0.1019110f, -3.18609f, -0.00245881f, -2.83863f
		};
		XMFLOAT4 cbCoeffSH[9];
		for (auto i = 0ui8; i < 9; ++i)
		{
			cbCoeffSH[i].x = fCoeffSH[0][i];
			cbCoeffSH[i].y = fCoeffSH[1][i];
			cbCoeffSH[i].z = fCoeffSH[2][i];
			cbCoeffSH[i].w = 0.0f;
		}
		pUploaders->emplace_back();

		N_RETURN(m_cbCoeffSH->Upload(pCommandList, pUploaders->back(), cbCoeffSH, sizeof(cbCoeffSH)), Descriptor());
	}

	return m_cbCoeffSH->GetCBV();
}

bool LightProbe::createPipelineLayouts()
{
	// Generate Radiance graphics
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRange(1, DescriptorType::SRV, 2, 0);
		utilPipelineLayout->SetConstants(2, SizeOfInUint32(float) + SizeOfInUint32(uint32_t), 0);
		X_RETURN(m_pipelineLayouts[GEN_RADIANCE_GRAPHICS], utilPipelineLayout->GetPipelineLayout(
			*m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"RadianceGenerationLayout"), false);
	}

	// Generate Radiance compute
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetConstants(1, SizeOfInUint32(float), 0);
		utilPipelineLayout->SetRange(2, DescriptorType::UAV, 1, 0, 0,
			DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout->SetRange(3, DescriptorType::SRV, 2, 0);
		X_RETURN(m_pipelineLayouts[GEN_RADIANCE_COMPUTE], utilPipelineLayout->GetPipelineLayout(
			*m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"RadianceGenerationLayout"), false);
	}

	// Resampling graphics
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRange(1, DescriptorType::SRV, 1, 0);
		utilPipelineLayout->SetConstants(2, SizeOfInUint32(uint32_t), 0);
		utilPipelineLayout->SetShaderStage(0, Shader::PS);
		utilPipelineLayout->SetShaderStage(1, Shader::PS);
		utilPipelineLayout->SetShaderStage(2, Shader::PS);
		X_RETURN(m_pipelineLayouts[RESAMPLE_GRAPHICS], utilPipelineLayout->GetPipelineLayout(
			*m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"ResamplingGraphicsLayout"), false);
	}

	// Resampling compute
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRange(1, DescriptorType::UAV, 1, 0, 0,
			DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout->SetRange(2, DescriptorType::SRV, 1, 0);
		X_RETURN(m_pipelineLayouts[RESAMPLE_COMPUTE], utilPipelineLayout->GetPipelineLayout(
			*m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"ResamplingComputeLayout"), false);
	}

	// Up sampling graphics, with alpha blending
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRange(1, DescriptorType::SRV, 1, 0);
		utilPipelineLayout->SetConstants(2, SizeOfInUint32(CosConstants) + SizeOfInUint32(uint32_t), 0);
		utilPipelineLayout->SetShaderStage(0, Shader::PS);
		utilPipelineLayout->SetShaderStage(1, Shader::PS);
		utilPipelineLayout->SetShaderStage(2, Shader::PS);
		X_RETURN(m_pipelineLayouts[UP_SAMPLE_BLEND], utilPipelineLayout->GetPipelineLayout(
			*m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"UpSamplingGraphicsLayout"), false);
	}

	// Up sampling compute, in-place
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRange(1, DescriptorType::UAV, 1, 0, 0,
			DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout->SetRange(2, DescriptorType::SRV, 1, 0);
		utilPipelineLayout->SetConstants(3, SizeOfInUint32(CosConstants), 0);
		X_RETURN(m_pipelineLayouts[UP_SAMPLE_INPLACE], utilPipelineLayout->GetPipelineLayout(
			*m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"UpSamplingComputeLayout"), false);
	}

	// Up sampling graphics, for the final pass
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRange(1, DescriptorType::SRV, 2, 0);
		utilPipelineLayout->SetConstants(2, SizeOfInUint32(CosConstants) + SizeOfInUint32(uint32_t), 0);
		utilPipelineLayout->SetShaderStage(0, Shader::PS);
		utilPipelineLayout->SetShaderStage(1, Shader::PS);
		utilPipelineLayout->SetShaderStage(2, Shader::PS);
		X_RETURN(m_pipelineLayouts[FINAL_G], utilPipelineLayout->GetPipelineLayout(
			*m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"FinalPassGraphicsLayout"), false);
	}

	// Up sampling compute, for the final pass
	{
		const auto utilPipelineLayout = Util::PipelineLayout::MakeUnique();
		utilPipelineLayout->SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout->SetRange(1, DescriptorType::UAV, 1, 0, 0,
			DescriptorFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout->SetRange(2, DescriptorType::SRV, 2, 0);
		utilPipelineLayout->SetConstants(3, SizeOfInUint32(CosConstants), 0);
		X_RETURN(m_pipelineLayouts[FINAL_C], utilPipelineLayout->GetPipelineLayout(
			*m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"FinalPassComputeLayout"), false);
	}

	return true;
}

bool LightProbe::createPipelines(Format rtFormat, bool typedUAV)
{
	auto vsIndex = 0u;
	auto psIndex = 0u;
	auto csIndex = 0u;

	// Generate Radiance graphics
	N_RETURN(m_shaderPool->CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSGenRadiance.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[GEN_RADIANCE_GRAPHICS]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, *m_graphicsPipelineCache);
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->OMSetNumRenderTargets(1);
		state->OMSetRTVFormat(0, rtFormat);
		X_RETURN(m_pipelines[GEN_RADIANCE_GRAPHICS], state->GetPipeline(*m_graphicsPipelineCache, L"RadianceGeneration_graphics"), false);
	}

	// Generate Radiance compute
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSGenRadiance.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[GEN_RADIANCE_COMPUTE]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[GEN_RADIANCE_COMPUTE], state->GetPipeline(*m_computePipelineCache, L"RadianceGeneration_compute"), false);
	}

	// Resampling graphics
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSResample.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RESAMPLE_GRAPHICS]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, *m_graphicsPipelineCache);
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->OMSetNumRenderTargets(1);
		state->OMSetRTVFormat(0, rtFormat);
		X_RETURN(m_pipelines[RESAMPLE_GRAPHICS], state->GetPipeline(*m_graphicsPipelineCache, L"Resampling)graphics"), false);
	}

	// Resampling compute
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSResample.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[RESAMPLE_COMPUTE]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[RESAMPLE_COMPUTE], state->GetPipeline(*m_computePipelineCache, L"Resampling_compute"), false);
	}

	// Up sampling graphics, with alpha blending
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSCosUp_blend.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[UP_SAMPLE_BLEND]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex++));
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, *m_graphicsPipelineCache);
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->OMSetBlendState(Graphics::NON_PRE_MUL, *m_graphicsPipelineCache);
		state->OMSetNumRenderTargets(1);
		state->OMSetRTVFormat(0, rtFormat);
		X_RETURN(m_pipelines[UP_SAMPLE_BLEND], state->GetPipeline(*m_graphicsPipelineCache, L"UpSampling_alpha_blend"), false);
	}

	// Up sampling compute
	if (typedUAV)
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSCosUp_in_place.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[UP_SAMPLE_INPLACE]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[UP_SAMPLE_INPLACE], state->GetPipeline(*m_computePipelineCache, L"UpSampling_in_place"), false);
	}

	// Up sampling graphics, for the final pass
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::PS, psIndex, L"PSCosineUp.cso"), false);

		const auto state = Graphics::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[FINAL_G]);
		state->SetShader(Shader::Stage::VS, m_shaderPool->GetShader(Shader::Stage::VS, vsIndex));
		state->SetShader(Shader::Stage::PS, m_shaderPool->GetShader(Shader::Stage::PS, psIndex));
		state->DSSetState(Graphics::DEPTH_STENCIL_NONE, *m_graphicsPipelineCache);
		state->IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state->OMSetNumRenderTargets(1);
		state->OMSetRTVFormat(0, rtFormat);
		X_RETURN(m_pipelines[FINAL_G], state->GetPipeline(*m_graphicsPipelineCache, L"UpSampling_graphics"), false);
	}

	// Up sampling compute, for the final pass
	{
		N_RETURN(m_shaderPool->CreateShader(Shader::Stage::CS, csIndex, L"CSCosineUp.cso"), false);

		const auto state = Compute::State::MakeUnique();
		state->SetPipelineLayout(m_pipelineLayouts[FINAL_C]);
		state->SetShader(m_shaderPool->GetShader(Shader::Stage::CS, csIndex));
		X_RETURN(m_pipelines[FINAL_C], state->GetPipeline(*m_computePipelineCache, L"UpSampling_compute"), false);
	}

	return true;
}

bool LightProbe::createDescriptorTables()
{
	// Get UAV table for radiance generation
	m_uavTables[TABLE_RADIANCE].resize(1);
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_radiance->GetUAV());
		X_RETURN(m_uavTables[TABLE_RADIANCE][0], descriptorTable->GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Get SRV tables for radiance generation
	const auto numSources = static_cast<uint32_t>(m_sources.size());
	m_srvTables[TABLE_RADIANCE].resize(m_sources.size());
	for (auto i = 0u; i + 1 < numSources; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_sources[i]->GetSRV());
		X_RETURN(m_srvTables[TABLE_RADIANCE][i], descriptorTable->GetCbvSrvUavTable(*m_descriptorTableCache), false);
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
		X_RETURN(m_srvTables[TABLE_RADIANCE][i], descriptorTable->GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Get UAVs for resampling
	m_uavTables[TABLE_RESAMPLE].resize(m_numMips);
	for (auto i = 0ui8; i < m_numMips; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, &m_irradiance->GetUAV(i));
		X_RETURN(m_uavTables[TABLE_RESAMPLE][i], descriptorTable->GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Get SRVs for resampling
	m_srvTables[TABLE_RESAMPLE].resize(m_numMips);
	for (auto i = 0ui8; i < m_numMips; ++i)
	{
		const auto descriptorTable = Util::DescriptorTable::MakeUnique();
		descriptorTable->SetDescriptors(0, 1, i ? &m_irradiance->GetSRVLevel(i) : &m_radiance->GetSRV());
		X_RETURN(m_srvTables[TABLE_RESAMPLE][i], descriptorTable->GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Create the sampler table
	const auto descriptorTable = Util::DescriptorTable::MakeUnique();
	const auto sampler = LINEAR_WRAP;
	descriptorTable->SetSamplers(0, 1, &sampler, *m_descriptorTableCache);
	X_RETURN(m_samplerTable, descriptorTable->GetSamplerTable(*m_descriptorTableCache), false);

	return true;
}

uint32_t LightProbe::generateMipsGraphics(const CommandList* pCommandList, ResourceBarrier* pBarriers)
{
	const auto numBarriers = m_radiance->SetBarrier(pBarriers, ResourceState::PIXEL_SHADER_RESOURCE);
	return m_irradiance->GenerateMips(pCommandList, pBarriers,
		ResourceState::PIXEL_SHADER_RESOURCE, m_pipelineLayouts[RESAMPLE_GRAPHICS],
		m_pipelines[RESAMPLE_GRAPHICS], &m_srvTables[TABLE_RESAMPLE][0],
		1, m_samplerTable, 0, numBarriers);
}

uint32_t LightProbe::generateMipsCompute(const CommandList* pCommandList, ResourceBarrier* pBarriers,
	ResourceState addState)
{
	const auto numBarriers = m_radiance->SetBarrier(pBarriers, ResourceState::NON_PIXEL_SHADER_RESOURCE | addState);
	return m_irradiance->AsTexture2D()->GenerateMips(pCommandList, pBarriers, 8, 8, 1,
		ResourceState::NON_PIXEL_SHADER_RESOURCE, m_pipelineLayouts[RESAMPLE_COMPUTE],
		m_pipelines[RESAMPLE_COMPUTE], &m_uavTables[TABLE_RESAMPLE][1], 1, m_samplerTable,
		0, numBarriers, &m_srvTables[TABLE_RESAMPLE][0], 2);
}

void LightProbe::upsampleGraphics(const CommandList* pCommandList, ResourceBarrier* pBarriers, uint32_t numBarriers)
{
	// Up sampling
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[UP_SAMPLE_BLEND]);
	pCommandList->SetPipelineState(m_pipelines[UP_SAMPLE_BLEND]);
	pCommandList->SetGraphicsDescriptorTable(0, m_samplerTable);

	struct CosGConstants
	{
		CosConstants CosConsts;
		uint32_t Slice;
	} cb = { m_mapSize, m_numMips };
	pCommandList->SetGraphics32BitConstants(2, SizeOfInUint32(cb.CosConsts.Imm), &cb);

	const uint8_t numPasses = m_numMips - 1;
	for (auto i = 0ui8; i + 1 < numPasses; ++i)
	{
		const auto c = numPasses - i;
		cb.CosConsts.Level = c - 1;
		pCommandList->SetGraphics32BitConstants(2, 2, &cb.CosConsts.Level, SizeOfInUint32(cb.CosConsts.Imm));
		numBarriers = m_irradiance->Blit(pCommandList, pBarriers, cb.CosConsts.Level, c,
			ResourceState::PIXEL_SHADER_RESOURCE, m_srvTables[TABLE_RESAMPLE][c],
			1, numBarriers, 0, 0, SizeOfInUint32(cb.CosConsts));
	}

	// Final pass
	cb.CosConsts.Level = 0;
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[FINAL_G]);
	pCommandList->SetPipelineState(m_pipelines[FINAL_G]);
	pCommandList->SetGraphicsDescriptorTable(0, m_samplerTable);
	pCommandList->SetGraphics32BitConstants(2, SizeOfInUint32(cb), &cb);
	numBarriers = m_irradiance->Blit(pCommandList, pBarriers, 0, 1,
		ResourceState::PIXEL_SHADER_RESOURCE, m_srvTables[TABLE_RESAMPLE][0],
		1, numBarriers, 0, 0, SizeOfInUint32(cb.CosConsts));
}

void LightProbe::upsampleCompute(const CommandList* pCommandList, ResourceBarrier* pBarriers, uint32_t numBarriers)
{
	// Up sampling
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[UP_SAMPLE_INPLACE]);
	pCommandList->SetPipelineState(m_pipelines[UP_SAMPLE_INPLACE]);
	pCommandList->SetComputeDescriptorTable(0, m_samplerTable);

	CosConstants cb = { m_mapSize, m_numMips };
	pCommandList->SetCompute32BitConstants(3, SizeOfInUint32(cb.Imm), &cb);

	const uint8_t numPasses = m_numMips - 1;
	for (auto i = 0ui8; i + 1 < numPasses; ++i)
	{
		const auto c = numPasses - i;
		const auto level = c - 1;
		pCommandList->SetCompute32BitConstant(3, level, SizeOfInUint32(cb.Imm));
		numBarriers = m_irradiance->AsTexture2D()->Blit(pCommandList, pBarriers,
			8, 8, 1, level, c, ResourceState::NON_PIXEL_SHADER_RESOURCE,
			m_uavTables[TABLE_RESAMPLE][level], 1, numBarriers,
			m_srvTables[TABLE_RESAMPLE][c], 2);
	}

	// Final pass
	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[FINAL_C]);
	pCommandList->SetPipelineState(m_pipelines[FINAL_C]);
	pCommandList->SetComputeDescriptorTable(0, m_samplerTable);
	pCommandList->SetCompute32BitConstants(3, SizeOfInUint32(cb), &cb);
	numBarriers = m_irradiance->AsTexture2D()->Blit(pCommandList, pBarriers,
		8, 8, 1, 0, 1, ResourceState::NON_PIXEL_SHADER_RESOURCE,
		m_uavTables[TABLE_RESAMPLE][0], 1, numBarriers,
		m_srvTables[TABLE_RESAMPLE][1], 2);
}

void LightProbe::generateRadianceGraphics(const CommandList* pCommandList)
{
	static const auto period = 3.0;
	ResourceBarrier barrier;
	const auto numBarriers = m_radiance->SetBarrier(&barrier, ResourceState::RENDER_TARGET);
	pCommandList->Barrier(numBarriers, &barrier);

	const auto numSources = static_cast<uint32_t>(m_srvTables[TABLE_RADIANCE].size());
	auto blend = static_cast<float>(m_time / period);
	auto i = static_cast<uint32_t>(m_time / period);
	blend = numSources > 1 ? blend - i : 0.0f;
	i %= numSources;

	const float cb[2] = { blend };
	pCommandList->SetGraphicsPipelineLayout(m_pipelineLayouts[GEN_RADIANCE_GRAPHICS]);
	pCommandList->SetGraphics32BitConstants(2, SizeOfInUint32(cb), &cb);

	m_radiance->Blit(pCommandList, m_srvTables[TABLE_RADIANCE][i], 1, 0, 0, 0,
		m_samplerTable, 0, m_pipelines[GEN_RADIANCE_GRAPHICS], SizeOfInUint32(blend));
}

void LightProbe::generateRadianceCompute(const CommandList* pCommandList)
{
	static const auto period = 3.0;
	ResourceBarrier barrier;
	const auto numBarriers = m_radiance->SetBarrier(&barrier, ResourceState::UNORDERED_ACCESS);
	pCommandList->Barrier(numBarriers, &barrier);

	const auto numSources = static_cast<uint32_t>(m_srvTables[TABLE_RADIANCE].size());
	auto blend = static_cast<float>(m_time / period);
	auto i = static_cast<uint32_t>(m_time / period);
	blend = numSources > 1 ? blend - i : 0.0f;
	i %= numSources;

	pCommandList->SetComputePipelineLayout(m_pipelineLayouts[GEN_RADIANCE_COMPUTE]);
	pCommandList->SetCompute32BitConstant(1, reinterpret_cast<const uint32_t&>(blend));

	m_radiance->AsTexture2D()->Blit(pCommandList, 8, 8, 1, m_uavTables[TABLE_RADIANCE][0], 2, 0,
		m_srvTables[TABLE_RADIANCE][i], 3, m_samplerTable, 0, m_pipelines[GEN_RADIANCE_COMPUTE]);
}
