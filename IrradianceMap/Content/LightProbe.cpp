#include "LightProbe.h"
#include "Advanced/XUSGDDSLoader.h"

using namespace std;
using namespace DirectX;
using namespace XUSG;

struct CosConstants
{
	float		MapSize;
	uint32_t	NumLevels;
	uint32_t	Level;
};

LightProbe::LightProbe(const Device& device) :
	m_device(device),
	m_numMips(11)
{
	m_computePipelineCache.SetDevice(device);
	m_pipelineLayoutCache.SetDevice(device);
}

LightProbe::~LightProbe()
{
}

bool LightProbe::Init(const CommandList& commandList, uint32_t width, uint32_t height,
	const shared_ptr<DescriptorTableCache>& descriptorTableCache, shared_ptr<ResourceBase>& source,
	vector<Resource>& uploaders, const wchar_t* fileName)
{
	m_descriptorTableCache = descriptorTableCache;

	// Load input image
	{
		DDS::Loader textureLoader;
		DDS::AlphaMode alphaMode;

		uploaders.push_back(nullptr);
		N_RETURN(textureLoader.CreateTextureFromFile(m_device, commandList, fileName,
			8192, false, source, uploaders.back(), &alphaMode), false);
	}

	// Create resources and pipelines
	const auto& desc = source->GetResource()->GetDesc();
	const auto texWidth = static_cast<uint32_t>(desc.Width);
	const auto& texHeight = desc.Height;
	m_numMips = (max)(Log2((max)(texWidth, texHeight)), 1ui8) + 1;
	m_mapSize = (texWidth + texHeight) * 0.5f;

	m_filtered[TABLE_DOWN_SAMPLE].Create(m_device, texWidth, texHeight, desc.Format,
		6, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_numMips - 1, 1,
		D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true);
	m_filtered[TABLE_UP_SAMPLE].Create(m_device, texWidth, texHeight, desc.Format,
		6, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, m_numMips, 1,
		D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true);

	N_RETURN(createPipelineLayouts(), false);
	N_RETURN(createPipelines(), false);
	N_RETURN(createDescriptorTables(), false);

	// Copy source
	{
		ResourceBarrier barriers[7];
		auto numBarriers = source->SetBarrier(barriers, D3D12_RESOURCE_STATE_COPY_SOURCE);
		for (auto i = 0ui8; i < desc.DepthOrArraySize; ++i)
			numBarriers = m_filtered[TABLE_DOWN_SAMPLE].SetBarrier(barriers, 0, D3D12_RESOURCE_STATE_COPY_DEST, numBarriers, i);
		
		commandList.Barrier(numBarriers, barriers);

		for (auto i = 0ui8; i < desc.DepthOrArraySize; ++i)
		{
			const auto srcSubres = D3D12CalcSubresource(0, i, 0, desc.MipLevels, desc.DepthOrArraySize);
			const auto dstSubres = D3D12CalcSubresource(0, i, 0, m_numMips - 1, desc.DepthOrArraySize);
			const TextureCopyLocation src(source->GetResource().get(), srcSubres);
			const TextureCopyLocation dst(m_filtered[TABLE_DOWN_SAMPLE].GetResource().get(), dstSubres);
			commandList.CopyTextureRegion(dst, 0, 0, 0, src);
		}
	}

	return true;
}

void LightProbe::Process(const CommandList& commandList, ResourceState dstState)
{
	const uint8_t numPasses = m_numMips - 1;

	// Set Descriptor pools
	const DescriptorPool descriptorPools[] =
	{
		m_descriptorTableCache->GetDescriptorPool(CBV_SRV_UAV_POOL),
		m_descriptorTableCache->GetDescriptorPool(SAMPLER_POOL)
	};
	commandList.SetDescriptorPools(static_cast<uint32_t>(size(descriptorPools)), descriptorPools);

	// Generate Mips
	ResourceBarrier barriers[12];
	auto numBarriers = 0u;
	if (numPasses > 0) numBarriers = m_filtered[TABLE_DOWN_SAMPLE].GenerateMips(commandList, barriers,
		8, 8, 1, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		m_pipelineLayouts[RESAMPLE], m_pipelines[RESAMPLE], m_uavSrvTables[TABLE_DOWN_SAMPLE].data(),
		1, m_samplerTable, 0, numBarriers, nullptr, 0, 1, numPasses - 1);
	numBarriers = m_filtered[TABLE_UP_SAMPLE].SetBarrier(barriers, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, numBarriers);
	commandList.Barrier(numBarriers, barriers);
	numBarriers = 0;

	commandList.SetComputeDescriptorTable(1, m_uavSrvTables[TABLE_DOWN_SAMPLE][numPasses]);
	commandList.Dispatch(1, 1, 6);

	// Up sampling
	commandList.SetComputePipelineLayout(m_pipelineLayouts[UP_SAMPLE]);
	commandList.SetPipelineState(m_pipelines[UP_SAMPLE]);
	commandList.SetComputeDescriptorTable(0, m_samplerTable);

	CosConstants cb = { m_mapSize, m_numMips };
	for (auto i = 0ui8; i < numPasses; ++i)
	{
		const auto c = numPasses - i;
		cb.Level = c - 1;
		commandList.SetCompute32BitConstants(2, SizeOfInUint32(uint32_t), &cb.Level);
		numBarriers = m_filtered[TABLE_UP_SAMPLE].Blit(commandList, barriers, 8, 8, 1,
			cb.Level, c, dstState, m_uavSrvTables[TABLE_UP_SAMPLE][i], 1, numBarriers);
	}
}

Texture2D& LightProbe::GetIrradiance()
{
	return m_filtered[TABLE_UP_SAMPLE];
}

Texture2D& LightProbe::GetRadiance()
{
	return m_filtered[TABLE_DOWN_SAMPLE];
}

bool LightProbe::createPipelineLayouts()
{
	// Resampling
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::SRV, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::UAV, 1, 0, 0,
			D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
		X_RETURN(m_pipelineLayouts[RESAMPLE], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, D3D12_ROOT_SIGNATURE_FLAG_NONE, L"ResamplingLayout"), false);
	}

	// Up sampling
	{
		Util::PipelineLayout utilPipelineLayout;
		utilPipelineLayout.SetRange(0, DescriptorType::SAMPLER, 1, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::SRV, 2, 0);
		utilPipelineLayout.SetRange(1, DescriptorType::UAV, 1, 0, 0,
			D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE);
		utilPipelineLayout.SetConstants(2, SizeOfInUint32(CosConstants), 0);
		X_RETURN(m_pipelineLayouts[UP_SAMPLE], utilPipelineLayout.GetPipelineLayout(
			m_pipelineLayoutCache, D3D12_ROOT_SIGNATURE_FLAG_NONE, L"UpSamplingLayout"), false);
	}

	return true;
}

bool LightProbe::createPipelines()
{
	// Resampling
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, RESAMPLE, L"CSResample.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[RESAMPLE]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, RESAMPLE));
		X_RETURN(m_pipelines[RESAMPLE], state.GetPipeline(m_computePipelineCache, L"Resampling"), false);
	}

	// Up sampling
	{
		N_RETURN(m_shaderPool.CreateShader(Shader::Stage::CS, UP_SAMPLE, L"CSUpSample.cso"), false);

		Compute::State state;
		state.SetPipelineLayout(m_pipelineLayouts[UP_SAMPLE]);
		state.SetShader(m_shaderPool.GetShader(Shader::Stage::CS, UP_SAMPLE));
		X_RETURN(m_pipelines[UP_SAMPLE], state.GetPipeline(m_computePipelineCache, L"UpSampling"), false);
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
				m_filtered[TABLE_DOWN_SAMPLE].GetSRVLevel(i),
				m_filtered[TABLE_DOWN_SAMPLE].GetUAV(i + 1)
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
				m_filtered[TABLE_DOWN_SAMPLE].GetSRVLevel(current),
				m_filtered[TABLE_UP_SAMPLE].GetSRVLevel(coarser),
				m_filtered[TABLE_UP_SAMPLE].GetUAV(current)
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
			m_filtered[TABLE_DOWN_SAMPLE].GetSRVLevel(numPasses - 1),
			m_filtered[TABLE_UP_SAMPLE].GetUAV(numPasses)
		};
		Util::DescriptorTable utilUavSrvTable;
		utilUavSrvTable.SetDescriptors(0, static_cast<uint32_t>(size(descriptors)), descriptors);
		X_RETURN(m_uavSrvTables[TABLE_DOWN_SAMPLE][numPasses], utilUavSrvTable.GetCbvSrvUavTable(*m_descriptorTableCache), false);
	}

	// Create the sampler table
	Util::DescriptorTable samplerTable;
	const auto sampler = LINEAR_WRAP;
	samplerTable.SetSamplers(0, 1, &sampler, *m_descriptorTableCache);
	X_RETURN(m_samplerTable, samplerTable.GetSamplerTable(*m_descriptorTableCache), false);

	return true;
}
