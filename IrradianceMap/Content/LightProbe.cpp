#include "LightProbe.h"
#include "Advanced/XUSGDDSLoader.h"

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
	m_graphicsPipelineCache.SetDevice(device);
	m_computePipelineCache.SetDevice(device);
	m_pipelineLayoutCache.SetDevice(device);
}

LightProbe::~LightProbe()
{
}

bool LightProbe::Init(const CommandList& commandList, uint32_t width, uint32_t height,
	const shared_ptr<DescriptorTableCache>& descriptorTableCache, vector<Resource>& uploaders,
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
		N_RETURN(textureLoader.CreateTextureFromFile(m_device, commandList, pFileNames[i].c_str(),
			8192, false, m_sources[i], uploaders.back(), &alphaMode), false);

		const auto& desc = m_sources[i]->GetResource()->GetDesc();
		texWidth = (max)(static_cast<uint32_t>(desc.Width), texWidth);
		texHeight = (max)(desc.Height, texHeight);
	}

	// Create resources and pipelines
	m_numMips = (max)(Log2((max)(texWidth, texHeight)), 1ui8) + 1;
	m_mapSize = (texWidth + texHeight) * 0.5f;

	const auto format = Format::R11G11B10_FLOAT;
	m_radiance.Create(m_device, texWidth, texHeight, format,
		6, ResourceFlag::ALLOW_UNORDERED_ACCESS,
		1, 1, nullptr, true, L"Radiance");
	m_irradiance.Create(m_device, texWidth, texHeight, format, 6,
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

void LightProbe::Process(const CommandList& commandList, ResourceState dstState, PipelineType pipelineType)
{
	// Set Descriptor pools
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache->GetDescriptorPool(SAMPLER_POOL)
	};
	commandList.SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	ResourceBarrier barriers[13];
	uint32_t numBarriers;

	switch (pipelineType)
	{
	case GRAPHICS:
		generateRadianceGraphics(commandList);
		numBarriers = generateMipsGraphics(commandList, barriers);
		upsampleGraphics(commandList, barriers, numBarriers);
		break;
	case COMPUTE:
		if (m_pipelines[UP_SAMPLE_C])
		{
			generateRadianceCompute(commandList);
			numBarriers = generateMipsCompute(commandList, barriers);
			upsampleCompute(commandList, barriers, numBarriers);
			break;
		}
	default:
		generateRadianceCompute(commandList);
		numBarriers = generateMipsCompute(commandList, barriers, ResourceState::PIXEL_SHADER_RESOURCE) - 6;
		for (auto i = 0ui8; i < 6; ++i)
			numBarriers = m_irradiance.SetBarrier(barriers, m_numMips - 1,
				ResourceState::UNORDERED_ACCESS, numBarriers, i);
		upsampleGraphics(commandList, barriers, numBarriers - 6);
	}
}

ResourceBase* LightProbe::GetIrradianceGT(const CommandList& commandList,
	const wchar_t* fileName, vector<Resource>* pUploaders)
{
	if (!m_groundTruth && fileName && pUploaders)
	{
		DDS::Loader textureLoader;
		DDS::AlphaMode alphaMode;

		pUploaders->emplace_back();
		N_RETURN(textureLoader.CreateTextureFromFile(m_device, commandList, fileName,
			8192, false, m_groundTruth, pUploaders->back(), &alphaMode), nullptr);
	}

	return m_groundTruth.get();
}

Texture2D& LightProbe::GetIrradiance()
{
	return m_irradiance;
}

Texture2D& LightProbe::GetRadiance()
{
	return m_radiance;
}

Descriptor LightProbe::GetSH(const CommandList& commandList,
	const wchar_t* fileName, vector<Resource>* pUploaders)
{
	if (pUploaders)
	{
		// Create immutable constant buffer
		N_RETURN(m_cbCoeffSH.Create(m_device, sizeof(XMFLOAT4[9]), 1,
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

		N_RETURN(m_cbCoeffSH.Upload(commandList, pUploaders->back(), cbCoeffSH, sizeof(cbCoeffSH)), Descriptor());
	}

	return m_cbCoeffSH.GetCBV();
}

bool LightProbe::createPipelineLayouts()
{
	// Generate Radiance graphics
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::SRV, 2, 0);
		utilPipelineLayout.SetConstants(2, SizeOfInUint32(float) + SizeOfInUint32(uint32_t), 0);
		X_RETURN(m_pipelineLayouts[RADIANCE_G], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"RadianceGenerationLayout"), false);
	}

	// Generate Radiance compute
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetConstants(1, SizeOfInUint32(float), 0);
		utilPipelineLayout.SetRange(2, DescriptorType::UAV, 1, 0, 0,
			DescriptorRangeFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout.SetRange(3, DescriptorType::SRV, 2, 0);
		X_RETURN(m_pipelineLayouts[RADIANCE_C], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"RadianceGenerationLayout"), false);
	}

	// Resampling graphics
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::SRV, 1, 0);
		utilPipelineLayout.SetConstants(2, SizeOfInUint32(uint32_t), 0);
		utilPipelineLayout.SetShaderStage(0, Shader::PS);
		utilPipelineLayout.SetShaderStage(1, Shader::PS);
		utilPipelineLayout.SetShaderStage(2, Shader::PS);
		X_RETURN(m_pipelineLayouts[RESAMPLE_G], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"ResamplingGraphicsLayout"), false);
	}

	// Resampling compute
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::UAV, 1, 0, 0,
			DescriptorRangeFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout.SetRange(2, DescriptorType::SRV, 1, 0);
		X_RETURN(m_pipelineLayouts[RESAMPLE_C], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"ResamplingComputeLayout"), false);
	}

	// Up sampling graphics
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::SRV, 1, 0);
		utilPipelineLayout.SetConstants(2, SizeOfInUint32(CosConstants) + SizeOfInUint32(uint32_t), 0);
		utilPipelineLayout.SetShaderStage(0, Shader::PS);
		utilPipelineLayout.SetShaderStage(1, Shader::PS);
		utilPipelineLayout.SetShaderStage(2, Shader::PS);
		X_RETURN(m_pipelineLayouts[UP_SAMPLE_G], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"UpSamplingGraphicsLayout"), false);
	}

	// Up sampling compute
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::UAV, 1, 0, 0,
			DescriptorRangeFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout.SetRange(2, DescriptorType::SRV, 1, 0);
		utilPipelineLayout.SetConstants(3, SizeOfInUint32(CosConstants), 0);
		X_RETURN(m_pipelineLayouts[UP_SAMPLE_C], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"UpSamplingComputeLayout"), false);
	}

	// Final pass graphics
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::SRV, 2, 0);
		utilPipelineLayout.SetConstants(2, SizeOfInUint32(CosConstants) + SizeOfInUint32(uint32_t), 0);
		utilPipelineLayout.SetShaderStage(0, Shader::PS);
		utilPipelineLayout.SetShaderStage(1, Shader::PS);
		utilPipelineLayout.SetShaderStage(2, Shader::PS);
		X_RETURN(m_pipelineLayouts[FINAL_G], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"FinalPassGraphicsLayout"), false);
	}

	// Final pass compute
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::UAV, 1, 0, 0,
			DescriptorRangeFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout.SetRange(2, DescriptorType::SRV, 2, 0);
		utilPipelineLayout.SetConstants(3, SizeOfInUint32(CosConstants), 0);
		X_RETURN(m_pipelineLayouts[FINAL_C], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"FinalPassComputeLayout"), false);
	}

	return true;
}

bool LightProbe::createPipelines(Format rtFormat, bool typedUAV)
{
	auto vsIndex = 0u;
	auto psIndex = 0u;
	auto csIndex = 0u;

	// Generate Radiance graphics
	N_RETURN(m_shaderPool.CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::PS, psIndex, L"PSRadiance.cso"), false);

		Graphics::State state;
		state.SetPipelineLayout(m_pipelineLayouts[RADIANCE_G]);
		state.SetShader(Shader::Stage::VS, m_shaderPool.GetShader(Shader::Stage::VS, vsIndex));
		state.SetShader(Shader::Stage::PS, m_shaderPool.GetShader(Shader::Stage::PS, psIndex++));
		state.DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache);
		state.IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state.OMSetNumRenderTargets(1);
		state.OMSetRTVFormat(0, rtFormat);
		X_RETURN(m_pipelines[RADIANCE_G], state.GetPipeline(m_graphicsPipelineCache, L"RadianceGenerationGraphics"), false);
	}

	// Generate Radiance compute
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, csIndex, L"CSRadiance.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[RADIANCE_C]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[RADIANCE_C], state.GetPipeline(m_computePipelineCache, L"RadianceGenerationCompute"), false);
	}

	// Resampling graphics
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::PS, psIndex, L"PSResample.cso"), false);

		Graphics::State state;
		state.SetPipelineLayout(m_pipelineLayouts[RESAMPLE_G]);
		state.SetShader(Shader::Stage::VS, m_shaderPool.GetShader(Shader::Stage::VS, vsIndex));
		state.SetShader(Shader::Stage::PS, m_shaderPool.GetShader(Shader::Stage::PS, psIndex++));
		state.DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache);
		state.IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state.OMSetNumRenderTargets(1);
		state.OMSetRTVFormat(0, rtFormat);
		X_RETURN(m_pipelines[RESAMPLE_G], state.GetPipeline(m_graphicsPipelineCache, L"ResamplingGraphics"), false);
	}

	// Resampling compute
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, csIndex, L"CSResample.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[RESAMPLE_C]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[RESAMPLE_C], state.GetPipeline(m_computePipelineCache, L"ResamplingCompute"), false);
	}

	// Up sampling graphics
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::PS, psIndex, L"PSCosineUp.cso"), false);

		Graphics::State state;
		state.SetPipelineLayout(m_pipelineLayouts[UP_SAMPLE_G]);
		state.SetShader(Shader::Stage::VS, m_shaderPool.GetShader(Shader::Stage::VS, vsIndex));
		state.SetShader(Shader::Stage::PS, m_shaderPool.GetShader(Shader::Stage::PS, psIndex++));
		state.DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache);
		state.IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state.OMSetBlendState(Graphics::NON_PRE_MUL, m_graphicsPipelineCache);
		state.OMSetNumRenderTargets(1);
		state.OMSetRTVFormat(0, rtFormat);
		X_RETURN(m_pipelines[UP_SAMPLE_G], state.GetPipeline(m_graphicsPipelineCache, L"UpSamplingGraphics"), false);
	}

	// Up sampling compute
	if (typedUAV)
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, csIndex, L"CSCosineUp.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[UP_SAMPLE_C]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[UP_SAMPLE_C], state.GetPipeline(m_computePipelineCache, L"UpSampling"), false);
	}

	// Final pass graphics
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::PS, psIndex, L"PSFinal.cso"), false);

		Graphics::State state;
		state.SetPipelineLayout(m_pipelineLayouts[FINAL_G]);
		state.SetShader(Shader::Stage::VS, m_shaderPool.GetShader(Shader::Stage::VS, vsIndex));
		state.SetShader(Shader::Stage::PS, m_shaderPool.GetShader(Shader::Stage::PS, psIndex));
		state.DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache);
		state.IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state.OMSetNumRenderTargets(1);
		state.OMSetRTVFormat(0, rtFormat);
		X_RETURN(m_pipelines[FINAL_G], state.GetPipeline(m_graphicsPipelineCache, L"FinalPassGraphics"), false);
	}

	// Final pass compute
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, csIndex, L"CSFinal.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[FINAL_C]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, csIndex));
		X_RETURN(m_pipelines[FINAL_C], state.GetPipeline(m_computePipelineCache, L"FinalPassCompute"), false);
	}

	return true;
}

bool LightProbe::createDescriptorTables()
{
	// Get UAV table for radiance generation
	m_uavTables[TABLE_RADIANCE].resize(1);
	{
		Util::DescriptorTable utilUavTable;
		utilUavTable.SetDescriptors(0, 1, &m_radiance.GetUAV());
		X_RETURN(m_uavTables[TABLE_RADIANCE][0], utilUavTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Get SRV tables for radiance generation
	const auto numSources = static_cast<uint32_t>(m_sources.size());
	m_srvTables[TABLE_RADIANCE].resize(m_sources.size());
	for (auto i = 0u; i + 1 < numSources; ++i)
	{
		Util::DescriptorTable utilSrvTable;
		utilSrvTable.SetDescriptors(0, 1, &m_sources[i]->GetSRV());
		X_RETURN(m_srvTables[TABLE_RADIANCE][i], utilSrvTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}
	{
		const auto i = numSources - 1;
		const Descriptor descriptors[] =
		{
			m_sources[i]->GetSRV(),
			m_sources[0]->GetSRV()
		};
		Util::DescriptorTable utilSrvTable;
		utilSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_srvTables[TABLE_RADIANCE][i], utilSrvTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Get UAVs for resampling
	m_uavTables[TABLE_RESAMPLE].resize(m_numMips);
	for (auto i = 0ui8; i < m_numMips; ++i)
	{
		Util::DescriptorTable utilUavTable;
		utilUavTable.SetDescriptors(0, 1, &m_irradiance.GetUAV(i));
		X_RETURN(m_uavTables[TABLE_RESAMPLE][i], utilUavTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Get SRVs for resampling
	m_srvTables[TABLE_RESAMPLE].resize(m_numMips);
	for (auto i = 0ui8; i < m_numMips; ++i)
	{
		Util::DescriptorTable utilSrvTable;
		utilSrvTable.SetDescriptors(0, 1, i ? &m_irradiance.GetSRVLevel(i) : &m_radiance.GetSRV());
		X_RETURN(m_srvTables[TABLE_RESAMPLE][i], utilSrvTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Create the sampler table
	Util::DescriptorTable samplerTable;
	const auto sampler = LINEAR_WRAP;
	samplerTable.SetSamplers(0, 1, &sampler, *m_descriptorTableCache);
	X_RETURN(m_samplerTable, samplerTable.GetSamplerTable(*m_descriptorTableCache), false);

	return true;
}

uint32_t LightProbe::generateMipsGraphics(const CommandList& commandList, ResourceBarrier* pBarriers)
{
	const auto numBarriers = m_radiance.SetBarrier(pBarriers, ResourceState::PIXEL_SHADER_RESOURCE);
	return m_irradiance.GenerateMips(commandList, pBarriers,
		ResourceState::PIXEL_SHADER_RESOURCE, m_pipelineLayouts[RESAMPLE_G],
		m_pipelines[RESAMPLE_G], &m_srvTables[TABLE_RESAMPLE][0],
		1, m_samplerTable, 0, numBarriers);
}

uint32_t LightProbe::generateMipsCompute(const CommandList& commandList, ResourceBarrier* pBarriers,
	ResourceState addState)
{
	const auto numBarriers = m_radiance.SetBarrier(pBarriers, ResourceState::NON_PIXEL_SHADER_RESOURCE | addState);
	return m_irradiance.Texture2D::GenerateMips(commandList, pBarriers, 8, 8, 1,
		ResourceState::NON_PIXEL_SHADER_RESOURCE, m_pipelineLayouts[RESAMPLE_C],
		m_pipelines[RESAMPLE_C], &m_uavTables[TABLE_RESAMPLE][1], 1, m_samplerTable,
		0, numBarriers, &m_srvTables[TABLE_RESAMPLE][0], 2);
}

void LightProbe::upsampleGraphics(const CommandList& commandList, ResourceBarrier* pBarriers, uint32_t numBarriers)
{
	// Up sampling
	commandList.SetGraphicsPipelineLayout(m_pipelineLayouts[UP_SAMPLE_G]);
	commandList.SetPipelineState(m_pipelines[UP_SAMPLE_G]);
	commandList.SetGraphicsDescriptorTable(0, m_samplerTable);

	struct CosGConstants
	{
		CosConstants CosConsts;
		uint32_t Slice;
	} cb = { m_mapSize, m_numMips };
	commandList.SetGraphics32BitConstants(2, SizeOfInUint32(cb.CosConsts.Imm), &cb);

	const uint8_t numPasses = m_numMips - 1;
	for (auto i = 0ui8; i + 1 < numPasses; ++i)
	{
		const auto c = numPasses - i;
		cb.CosConsts.Level = c - 1;
		commandList.SetGraphics32BitConstants(2, 2, &cb.CosConsts.Level, SizeOfInUint32(cb.CosConsts.Imm));
		numBarriers = m_irradiance.Blit(commandList, pBarriers, cb.CosConsts.Level, c,
			ResourceState::PIXEL_SHADER_RESOURCE, m_srvTables[TABLE_RESAMPLE][c],
			1, numBarriers, 0, 0, SizeOfInUint32(cb.CosConsts));
	}

	// Final pass
	cb.CosConsts.Level = 0;
	commandList.SetGraphicsPipelineLayout(m_pipelineLayouts[FINAL_G]);
	commandList.SetPipelineState(m_pipelines[FINAL_G]);
	commandList.SetGraphicsDescriptorTable(0, m_samplerTable);
	commandList.SetGraphics32BitConstants(2, SizeOfInUint32(cb), &cb);
	numBarriers = m_irradiance.Blit(commandList, pBarriers, 0, 1,
		ResourceState::PIXEL_SHADER_RESOURCE, m_srvTables[TABLE_RESAMPLE][0],
		1, numBarriers, 0, 0, SizeOfInUint32(cb.CosConsts));
}

void LightProbe::upsampleCompute(const CommandList& commandList, ResourceBarrier* pBarriers, uint32_t numBarriers)
{
	// Up sampling
	commandList.SetComputePipelineLayout(m_pipelineLayouts[UP_SAMPLE_C]);
	commandList.SetPipelineState(m_pipelines[UP_SAMPLE_C]);
	commandList.SetComputeDescriptorTable(0, m_samplerTable);

	CosConstants cb = { m_mapSize, m_numMips };
	commandList.SetCompute32BitConstants(3, SizeOfInUint32(cb.Imm), &cb);

	const uint8_t numPasses = m_numMips - 1;
	for (auto i = 0ui8; i + 1 < numPasses; ++i)
	{
		const auto c = numPasses - i;
		const auto level = c - 1;
		commandList.SetCompute32BitConstant(3, level, SizeOfInUint32(cb.Imm));
		numBarriers = m_irradiance.Texture2D::Blit(commandList, pBarriers,
			8, 8, 1, level, c, ResourceState::NON_PIXEL_SHADER_RESOURCE,
			m_uavTables[TABLE_RESAMPLE][level], 1, numBarriers,
			m_srvTables[TABLE_RESAMPLE][c], 2);
	}

	// Final pass
	commandList.SetComputePipelineLayout(m_pipelineLayouts[FINAL_C]);
	commandList.SetPipelineState(m_pipelines[FINAL_C]);
	commandList.SetComputeDescriptorTable(0, m_samplerTable);
	commandList.SetCompute32BitConstants(3, SizeOfInUint32(cb), &cb);
	numBarriers = m_irradiance.Texture2D::Blit(commandList, pBarriers,
		8, 8, 1, 0, 1, ResourceState::NON_PIXEL_SHADER_RESOURCE,
		m_uavTables[TABLE_RESAMPLE][0], 1, numBarriers,
		m_srvTables[TABLE_RESAMPLE][1], 2);
}

void LightProbe::generateRadianceGraphics(const CommandList& commandList)
{
	static const auto period = 3.0;
	ResourceBarrier barrier;
	const auto numBarriers = m_radiance.SetBarrier(&barrier, ResourceState::RENDER_TARGET);
	commandList.Barrier(numBarriers, &barrier);

	const auto numSources = static_cast<uint32_t>(m_srvTables[TABLE_RADIANCE].size());
	auto blend = static_cast<float>(m_time / period);
	auto i = static_cast<uint32_t>(m_time / period);
	blend = numSources > 1 ? blend - i : 0.0f;
	i %= numSources;

	const float cb[2] = { blend };
	commandList.SetGraphicsPipelineLayout(m_pipelineLayouts[RADIANCE_G]);
	commandList.SetGraphics32BitConstants(2, SizeOfInUint32(cb), &cb);

	m_radiance.Blit(commandList, m_srvTables[TABLE_RADIANCE][i], 1, 0, 0, 0,
		m_samplerTable, 0, m_pipelines[RADIANCE_G], SizeOfInUint32(blend));
}

void LightProbe::generateRadianceCompute(const CommandList& commandList)
{
	static const auto period = 3.0;
	ResourceBarrier barrier;
	const auto numBarriers = m_radiance.SetBarrier(&barrier, ResourceState::UNORDERED_ACCESS);
	commandList.Barrier(numBarriers, &barrier);

	const auto numSources = static_cast<uint32_t>(m_srvTables[TABLE_RADIANCE].size());
	auto blend = static_cast<float>(m_time / period);
	auto i = static_cast<uint32_t>(m_time / period);
	blend = numSources > 1 ? blend - i : 0.0f;
	i %= numSources;

	commandList.SetComputePipelineLayout(m_pipelineLayouts[RADIANCE_C]);
	commandList.SetCompute32BitConstant(1, reinterpret_cast<const uint32_t&>(blend));

	m_radiance.Texture2D::Blit(commandList, 8, 8, 1, m_uavTables[TABLE_RADIANCE][0], 2, 0,
		m_srvTables[TABLE_RADIANCE][i], 3, m_samplerTable, 0, m_pipelines[RADIANCE_C]);
}
