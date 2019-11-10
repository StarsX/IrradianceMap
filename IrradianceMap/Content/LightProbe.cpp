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
	const wstring pFileNames[], uint32_t numFiles)
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
		6, ResourceFlag::ALLOW_UNORDERED_ACCESS, m_numMips - 1, 1,
		MemoryType::DEFAULT, ResourceState::COMMON, true, L"Radiance");
	m_irradiance.Create(m_device, texWidth, texHeight, format, 6,
		ResourceFlag::ALLOW_UNORDERED_ACCESS, m_numMips, 1,
		ResourceState::COMMON, nullptr, true, L"Irradiance");

	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(format), false);
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

	generateRadiance(commandList);

	switch (pipelineType)
	{
	case GRAPHICS:
		generateMipsGraphics(commandList, dstState);
		upsampleGraphics(commandList, dstState);
		break;
	case COMPUTE:
		processLegacy(commandList, dstState);
		break;
	default:
		generateMipsCompute(commandList, dstState);
		upsampleGraphics(commandList, dstState);
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
	// Generate Radiance
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetConstants(1, SizeOfInUint32(float), 0);
		utilPipelineLayout.SetRange(2, DescriptorType::UAV, 1, 0, 0,
			DescriptorRangeFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout.SetRange(3, DescriptorType::SRV, 2, 0);
		X_RETURN(m_pipelineLayouts[RADIANCE], utilPipelineLayout.GetPipelineLayout(
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
			m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"ReamplingGraphicsLayout"), false);
	}

	// Resampling compute
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::SRV, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::UAV, 1, 0, 0,
			DescriptorRangeFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		X_RETURN(m_pipelineLayouts[RESAMPLE_C], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"ResamplingComputeLayout"), false);
	}

	// Up sampling graphics
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::SRV, 1, 0);
		utilPipelineLayout.SetConstants(2, SizeOfInUint32(CosConstants) + 1, 0);
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
		utilPipelineLayout.SetRange(1, DescriptorType::SRV, 2, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::UAV, 1, 0, 0,
			DescriptorRangeFlag::DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout.SetConstants(2, SizeOfInUint32(CosConstants), 0);
		X_RETURN(m_pipelineLayouts[UP_SAMPLE_C], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, PipelineLayoutFlag::NONE, L"UpSamplingComputeLayout"), false);
	}

	return true;
}

bool LightProbe::createPipelines(Format rtFormat, bool typedUAV)
{
	auto vsIndex = 0u;
	auto psIndex = 0u;
	auto csIndex = 0u;

	// Generate Radiance
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, csIndex, L"CSRadiance.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[RADIANCE]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, csIndex++));
		X_RETURN(m_pipelines[RADIANCE], state.GetPipeline(m_computePipelineCache, L"RadianceGeneration"), false);
	}

	// Resampling graphics
	N_RETURN(m_shaderPool.CreateShader(Shader::Stage::VS, vsIndex, L"VSScreenQuad.cso"), false);
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
		state.SetShader(Shader::Stage::PS, m_shaderPool.GetShader(Shader::Stage::PS, psIndex));
		state.DSSetState(Graphics::DEPTH_STENCIL_NONE, m_graphicsPipelineCache);
		state.IASetPrimitiveTopologyType(PrimitiveTopologyType::TRIANGLE);
		state.OMSetBlendState(Graphics::NON_PRE_MUL, m_graphicsPipelineCache);
		state.OMSetNumRenderTargets(1);
		state.OMSetRTVFormat(0, rtFormat);
		X_RETURN(m_pipelines[UP_SAMPLE_G], state.GetPipeline(m_graphicsPipelineCache, L"UpSamplingGraphics"), false);
	}

	// Up sampling compute
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, csIndex, L"CSCosineUp.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[UP_SAMPLE_C]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, csIndex));
		X_RETURN(m_pipelines[UP_SAMPLE_C], state.GetPipeline(m_computePipelineCache, L"UpSampling"), false);
	}

	return true;
}

bool LightProbe::createDescriptorTables()
{
	const uint8_t numPasses = m_numMips - 1;
	m_uavSrvTables[TABLE_DOWN_SAMPLE].resize(m_numMips);
	m_uavSrvTables[TABLE_UP_SAMPLE].resize(m_numMips);
	for (auto i = 0ui8; i + 1 < numPasses; ++i)
	{
		// Get UAV and SRVs
		{
			const Descriptor descriptors[] =
			{
				m_radiance.GetSRVLevel(i),
				m_radiance.GetUAV(i + 1)
			};
			Util::DescriptorTable utilUavSrvTable;
			utilUavSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
			X_RETURN(m_uavSrvTables[TABLE_DOWN_SAMPLE][i], utilUavSrvTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
		}
	}

	for (auto i = 0ui8; i < numPasses; ++i)
	{
		{
			const auto coarser = numPasses - i;
			const auto current = coarser - 1;
			const Descriptor descriptors[] =
			{
				m_radiance.GetSRVLevel(current),
				m_irradiance.GetSRVLevel(coarser),
				m_irradiance.GetUAV(current)
			};
			Util::DescriptorTable utilUavSrvTable;
			utilUavSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
			X_RETURN(m_uavSrvTables[TABLE_UP_SAMPLE][i], utilUavSrvTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
		}
	}

	// Get UAV and SRVs for the final-time down sampling
	{
		const Descriptor descriptors[] =
		{
			m_radiance.GetSRVLevel(numPasses - 1),
			m_irradiance.GetUAV(numPasses)
		};
		Util::DescriptorTable utilUavSrvTable;
		utilUavSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_uavSrvTables[TABLE_DOWN_SAMPLE][numPasses], utilUavSrvTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Get UAV table for radiance generation
	{
		Util::DescriptorTable utilUavTable;
		utilUavTable.SetDescriptors(0, 1, &m_radiance.GetUAV());
		X_RETURN(m_uavTable, utilUavTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Get SRV tables for radiance generation
	const auto numSources = static_cast<uint32_t>(m_sources.size());
	m_srvTables.resize(m_sources.size());
	for (auto i = 0u; i + 1 < numSources; ++i)
	{
		Util::DescriptorTable utilSrvTable;
		utilSrvTable.SetDescriptors(0, 1, &m_sources[i]->GetSRV());
		X_RETURN(m_srvTables[i], utilSrvTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
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
		X_RETURN(m_srvTables[i], utilSrvTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	////////////////////////////////////////////////////////////
	m_uavSrvTables[TABLE_RESAMPLE].resize(m_numMips);
	for (auto i = 0ui8; i < numPasses; ++i)
	{
		// Get UAV and SRVs
		const Descriptor descriptors[] =
		{
			i ? m_irradiance.GetSRVLevel(i) : m_radiance.GetSRV(),
			m_irradiance.GetUAV(i + 1)
		};
		Util::DescriptorTable utilUavSrvTable;
		utilUavSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_uavSrvTables[TABLE_RESAMPLE][i], utilUavSrvTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Get SRVs for graphics up-sampling
	{
		Util::DescriptorTable utilUavSrvTable;
		utilUavSrvTable.SetDescriptors(0, 1, &m_irradiance.GetSRVLevel(numPasses));
		X_RETURN(m_uavSrvTables[TABLE_RESAMPLE][numPasses], utilUavSrvTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Create the sampler table
	Util::DescriptorTable samplerTable;
	const auto sampler = LINEAR_WRAP;
	samplerTable.SetSamplers(0, 1, &sampler, *m_descriptorTableCache);
	X_RETURN(m_samplerTable, samplerTable.GetSamplerTable(*m_descriptorTableCache), false);

	return true;
}

void LightProbe::generateRadiance(const CommandList& commandList)
{
	static const auto period = 3.0;
	ResourceBarrier barrier;
	const auto numBarriers = m_radiance.SetBarrier(&barrier, ResourceState::UNORDERED_ACCESS);
	commandList.Barrier(numBarriers, &barrier);

	const auto numSources = static_cast<uint32_t>(m_srvTables.size());
	auto blend = static_cast<float>(m_time / period);
	auto i = static_cast<uint32_t>(m_time / period);
	blend = numSources > 1 ? blend - i : 0.0f;
	i %= numSources;

	commandList.SetComputePipelineLayout(m_pipelineLayouts[RADIANCE]);
	commandList.SetPipelineState(m_pipelines[RADIANCE]);
	commandList.SetComputeDescriptorTable(0, m_samplerTable);
	commandList.SetCompute32BitConstant(1, reinterpret_cast<const uint32_t&>(blend));

	m_radiance.Blit(commandList, 8, 8, 1, m_uavTable, 2, 0, m_srvTables[i], 3);
}

void LightProbe::generateMipsGraphics(const CommandList& commandList, ResourceState dstState)
{
	ResourceBarrier barriers[13];
	auto numBarriers = m_radiance.SetBarrier(barriers, dstState);
	numBarriers = m_irradiance.GenerateMips(commandList, barriers,
		ResourceState::PIXEL_SHADER_RESOURCE, m_pipelineLayouts[RESAMPLE_G],
		m_pipelines[RESAMPLE_G], m_uavSrvTables[TABLE_RESAMPLE].data(),
		1, m_samplerTable, 0, numBarriers, 1, m_numMips - 1);
	commandList.Barrier(numBarriers, barriers);
}

void LightProbe::generateMipsCompute(const CommandList& commandList, ResourceState dstState)
{
	ResourceBarrier barriers[13];
	auto numBarriers = m_radiance.SetBarrier(barriers, dstState);
	numBarriers = m_irradiance.Texture2D::GenerateMips(commandList, barriers, 8, 8,
		1, ResourceState::NON_PIXEL_SHADER_RESOURCE, m_pipelineLayouts[RESAMPLE_C],
		m_pipelines[RESAMPLE_C], m_uavSrvTables[TABLE_RESAMPLE].data(),
		1, m_samplerTable, 0, numBarriers, nullptr, 0, 1, m_numMips - 1);
	commandList.Barrier(numBarriers, barriers);
}

void LightProbe::upsampleGraphics(const CommandList& commandList, ResourceState dstState)
{
	// Up sampling
	ResourceBarrier barriers[12];
	auto numBarriers = 0u;
	commandList.SetGraphicsPipelineLayout(m_pipelineLayouts[UP_SAMPLE_G]);
	commandList.SetPipelineState(m_pipelines[UP_SAMPLE_G]);
	commandList.SetGraphicsDescriptorTable(0, m_samplerTable);

	CosConstants::Immutable cb = { m_mapSize, m_numMips };
	commandList.SetGraphics32BitConstants(2, SizeOfInUint32(cb), &cb);

	const uint8_t numPasses = m_numMips - 1;
	for (auto i = 0ui8; i < numPasses; ++i)
	{
		const auto c = numPasses - i;
		const auto level = c - 1;
		commandList.SetGraphics32BitConstant(2, level, SizeOfInUint32(cb));
		numBarriers = m_irradiance.Blit(commandList, barriers, level, c,
			dstState, m_uavSrvTables[TABLE_RESAMPLE][c], 1, numBarriers,
			0, 0, SizeOfInUint32(CosConstants));
	}
}

void LightProbe::processLegacy(const CommandList& commandList, ResourceState dstState)
{
	const uint8_t numPasses = m_numMips - 1;

	// Generate Mips
	ResourceBarrier barriers[12];
	auto numBarriers = 0u;
	if (numPasses > 0) numBarriers = m_radiance.GenerateMips(commandList, barriers,
		8, 8, 1, ResourceState::NON_PIXEL_SHADER_RESOURCE | ResourceState::PIXEL_SHADER_RESOURCE,
		m_pipelineLayouts[RESAMPLE_C], m_pipelines[RESAMPLE_C], m_uavSrvTables[TABLE_DOWN_SAMPLE].data(),
		1, m_samplerTable, 0, numBarriers, nullptr, 0, 1, numPasses - 1);
	numBarriers = m_irradiance.SetBarrier(barriers, ResourceState::UNORDERED_ACCESS, numBarriers);
	commandList.Barrier(numBarriers, barriers);

	numBarriers = 0;
	commandList.SetComputeDescriptorTable(1, m_uavSrvTables[TABLE_DOWN_SAMPLE][numPasses]);
	commandList.Dispatch(1, 1, 6);

	// Up sampling
	commandList.SetComputePipelineLayout(m_pipelineLayouts[UP_SAMPLE_C]);
	commandList.SetPipelineState(m_pipelines[UP_SAMPLE_C]);
	commandList.SetComputeDescriptorTable(0, m_samplerTable);

	CosConstants::Immutable cb = { m_mapSize, m_numMips };
	commandList.SetCompute32BitConstants(2, SizeOfInUint32(cb), &cb);

	for (auto i = 0ui8; i < numPasses; ++i)
	{
		const auto c = numPasses - i;
		const auto level = c - 1;
		commandList.SetCompute32BitConstant(2, level, SizeOfInUint32(cb));
		numBarriers = m_irradiance.Texture2D::Blit(commandList, barriers, 8, 8, 1,
			level, c, dstState, m_uavSrvTables[TABLE_UP_SAMPLE][i], 1, numBarriers);
	}
}
