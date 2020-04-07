//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#pragma once

#include "DXFramework.h"
#include "Core/XUSG.h"

class LightProbe
{
public:
	enum PipelineType
	{
		HYBRID,
		GRAPHICS,
		COMPUTE,

		NUM_PIPE_TYPE
	};

	LightProbe(const XUSG::Device &device);
	virtual ~LightProbe();

	bool Init(XUSG::CommandList* pCommandList, uint32_t width, uint32_t height,
		const XUSG::DescriptorTableCache::sptr& descriptorTableCache,
		std::vector<XUSG::Resource>& uploaders, const std::wstring pFileNames[],
		uint32_t numFiles, bool typedUAV);

	void UpdateFrame(double time);
	void Process(const XUSG::CommandList* pCommandList, XUSG::ResourceState dstState, PipelineType pipelineType);

	XUSG::ResourceBase* GetIrradianceGT(XUSG::CommandList* pCommandList,
		const wchar_t* fileName = nullptr, std::vector<XUSG::Resource>* pUploaders = nullptr);
	XUSG::Texture2D& GetIrradiance();
	XUSG::Texture2D& GetRadiance();
	XUSG::Descriptor GetSH(XUSG::CommandList* pCommandList, const wchar_t* fileName = nullptr,
		std::vector<XUSG::Resource>* pUploaders = nullptr);

protected:
	enum PipelineIndex : uint8_t
	{
		RADIANCE_G,
		RADIANCE_C,
		RESAMPLE_G,
		RESAMPLE_C,
		UP_SAMPLE_G,
		UP_SAMPLE_C,
		FINAL_G,
		FINAL_C,

		NUM_PIPELINE
	};

	enum UavSrvTableIndex : uint8_t
	{
		TABLE_RADIANCE,
		TABLE_RESAMPLE,

		NUM_UAV_SRV
	};

	bool createPipelineLayouts();
	bool createPipelines(XUSG::Format rtFormat, bool typedUAV);
	bool createDescriptorTables();

	uint32_t generateMipsGraphics(const XUSG::CommandList* pCommandList, XUSG::ResourceBarrier* pBarriers);
	uint32_t generateMipsCompute(const XUSG::CommandList* pCommandList, XUSG::ResourceBarrier* pBarriers,
		XUSG::ResourceState addState = XUSG::ResourceState::COMMON);

	void upsampleGraphics(const XUSG::CommandList* pCommandList, XUSG::ResourceBarrier* pBarriers, uint32_t numBarriers);
	void upsampleCompute(const XUSG::CommandList* pCommandList, XUSG::ResourceBarrier* pBarriers, uint32_t numBarriers);
	void generateRadianceGraphics(const XUSG::CommandList* pCommandList);
	void generateRadianceCompute(const XUSG::CommandList* pCommandList);
	
	XUSG::Device m_device;

	XUSG::ShaderPool::uptr				m_shaderPool;
	XUSG::Graphics::PipelineCache::uptr	m_graphicsPipelineCache;
	XUSG::Compute::PipelineCache::uptr	m_computePipelineCache;
	XUSG::PipelineLayoutCache::uptr		m_pipelineLayoutCache;
	XUSG::DescriptorTableCache::sptr	m_descriptorTableCache;

	XUSG::PipelineLayout	m_pipelineLayouts[NUM_PIPELINE];
	XUSG::Pipeline			m_pipelines[NUM_PIPELINE];

	std::vector<XUSG::DescriptorTable> m_srvTables[NUM_UAV_SRV];
	std::vector<XUSG::DescriptorTable> m_uavTables[NUM_UAV_SRV];
	XUSG::DescriptorTable	m_samplerTable;

	XUSG::ResourceBase::sptr m_groundTruth;
	std::vector<XUSG::ResourceBase::sptr> m_sources;
	XUSG::RenderTarget::uptr	m_radiance;
	XUSG::RenderTarget::uptr	m_irradiance;

	XUSG::ConstantBuffer::uptr	m_cbCoeffSH;

	uint8_t					m_numMips;
	float					m_mapSize;
	double					m_time;
};
