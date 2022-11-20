//--------------------------------------------------------------------------------------
// Copyright (c) XU, Tianchen. All rights reserved.
//--------------------------------------------------------------------------------------

#pragma once

#include "Core/XUSG.h"
#include "Advanced/XUSGSphericalHarmonics.h"

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

	bool Init(XUSG::CommandList* pCommandList, const XUSG::DescriptorTableLib::sptr& descriptorTableLib,
		std::vector<XUSG::Resource::uptr>& uploaders, const std::wstring pFileNames[],
		uint32_t numFiles, bool typedUAV);
	bool CreateDescriptorTables(XUSG::Device* pDevice);

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
		BLIT_GRAPHICS,
		BLIT_COMPUTE,
		UP_SAMPLE_BLEND,
		UP_SAMPLE_INPLACE,
		FINAL_G,
		FINAL_C,

		NUM_PIPELINE
	};

	enum CSIndex : uint8_t
	{
		CS_GEN_RADIANCE,
		CS_BLIT_CUBE,
		CS_UP_SAMPLE,
		CS_FINAL,
		CS_SH
	};

	enum UavSrvTableIndex : uint8_t
	{
		TABLE_RADIANCE,
		TABLE_BLIT,

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

	XUSG::ShaderLib::sptr				m_shaderLib;
	XUSG::Graphics::PipelineLib::uptr	m_graphicsPipelineLib;
	XUSG::Compute::PipelineLib::sptr	m_computePipelineLib;
	XUSG::PipelineLayoutLib::sptr		m_pipelineLayoutLib;
	XUSG::DescriptorTableLib::sptr		m_descriptorTableLib;

	XUSG::PipelineLayout	m_pipelineLayouts[NUM_PIPELINE];
	XUSG::Pipeline			m_pipelines[NUM_PIPELINE];

	XUSG::SphericalHarmonics::uptr m_sphericalHarmonics;

	std::vector<XUSG::DescriptorTable> m_srvTables[NUM_UAV_SRV];
	std::vector<XUSG::DescriptorTable> m_uavTables[NUM_UAV_SRV];
	XUSG::DescriptorTable	m_samplerTable;

	XUSG::Texture::sptr m_groundTruth;
	std::vector<XUSG::Texture::sptr> m_sources;
	XUSG::RenderTarget::uptr	m_irradiance;
	XUSG::RenderTarget::uptr	m_radiance;

	XUSG::ConstantBuffer::uptr	m_cbImmutable;
	XUSG::ConstantBuffer::uptr	m_cbPerFrame;

	uint32_t				m_inputProbeIdx;
};
