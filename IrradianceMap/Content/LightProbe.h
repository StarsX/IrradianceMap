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
		SH,

		NUM_PIPE_TYPE
	};

	LightProbe();
	virtual ~LightProbe();

	bool Init(XUSG::CommandList* pCommandList, uint32_t width, uint32_t height,
		const XUSG::DescriptorTableCache::sptr& descriptorTableCache,
		std::vector<XUSG::Resource::uptr>& uploaders, const std::wstring pFileNames[],
		uint32_t numFiles, bool typedUAV);

	void UpdateFrame(double time, uint8_t frameIndex);
	void Process(XUSG::CommandList* pCommandList, uint8_t frameIndex, PipelineType pipelineType);

	const XUSG::ShaderResource* GetIrradianceGT(XUSG::CommandList* pCommandList,
		const wchar_t* fileName = nullptr, std::vector<XUSG::Resource::uptr>* pUploaders = nullptr);
	XUSG::Texture2D* GetIrradiance() const;
	XUSG::ShaderResource* GetRadiance() const;
	XUSG::StructuredBuffer::sptr GetSH() const;

	static const uint8_t FrameCount = 3;
	static const uint8_t CubeMapFaceCount = 6;

protected:
	enum PipelineIndex : uint8_t
	{
		GEN_RADIANCE_GRAPHICS,
		GEN_RADIANCE_COMPUTE,
		RESAMPLE_GRAPHICS,
		RESAMPLE_COMPUTE,
		UP_SAMPLE_BLEND,
		UP_SAMPLE_INPLACE,
		FINAL_G,
		FINAL_C,
		SH_CUBE_MAP,
		SH_SUM,
		SH_NORMALIZE,

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

	uint32_t generateMipsGraphics(XUSG::CommandList* pCommandList, XUSG::ResourceBarrier* pBarriers);
	uint32_t generateMipsCompute(XUSG::CommandList* pCommandList, XUSG::ResourceBarrier* pBarriers);

	void upsampleGraphics(XUSG::CommandList* pCommandList, XUSG::ResourceBarrier* pBarriers, uint32_t numBarriers);
	void upsampleCompute(XUSG::CommandList* pCommandList, XUSG::ResourceBarrier* pBarriers, uint32_t numBarriers);
	void generateRadianceGraphics(XUSG::CommandList* pCommandList, uint8_t frameIndex);
	void generateRadianceCompute(XUSG::CommandList* pCommandList, uint8_t frameIndex);
	void shCubeMap(XUSG::CommandList* pCommandList, uint8_t order);
	void shSum(XUSG::CommandList* pCommandList, uint8_t order);
	void shNormalize(XUSG::CommandList* pCommandList, uint8_t order);

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

	XUSG::Texture::sptr m_groundTruth;
	std::vector<XUSG::Texture::sptr> m_sources;
	XUSG::RenderTarget::uptr	m_irradiance;
	XUSG::RenderTarget::uptr	m_radiance;

	XUSG::StructuredBuffer::sptr m_coeffSH[2];
	XUSG::StructuredBuffer::uptr m_weightSH[2];

	XUSG::ConstantBuffer::uptr	m_cbImmutable;
	XUSG::ConstantBuffer::uptr	m_cbPerFrame;

	uint32_t				m_inputProbeIdx;
	uint32_t				m_numSHTexels;
	uint8_t					m_shBufferParity;
};
