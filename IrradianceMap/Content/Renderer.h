//--------------------------------------------------------------------------------------
// By Stars XU Tianchen
//--------------------------------------------------------------------------------------

#pragma once

#include "Core/XUSG.h"

class Renderer
{
public:
	Renderer(const XUSG::Device& device);
	virtual ~Renderer();

	bool Init(const XUSG::CommandList& commandList, uint32_t width, uint32_t height,
		const std::shared_ptr<XUSG::DescriptorTableCache>& descriptorTableCache,
		std::vector<XUSG::Resource>& uploaders, const char* fileName, XUSG::Format rtFormat,
		const DirectX::XMFLOAT4& posScale = DirectX::XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f));
	bool SetLightProbes(const XUSG::Descriptor& irradiance, const XUSG::Descriptor& radiance);
	bool SetLightProbesGT(const XUSG::Descriptor& irradiance, const XUSG::Descriptor& radiance);

	void UpdateFrame(uint32_t frameIndex, DirectX::CXMVECTOR eyePt, DirectX::CXMMATRIX viewProj, bool isPaused);
	void Render(const XUSG::CommandList& commandList, uint32_t frameIndex,
		bool isGroundTruth = false, bool needClear = false);
	void ToneMap(const XUSG::CommandList& commandList, const XUSG::Descriptor& rtv,
		uint32_t numBarriers, XUSG::ResourceBarrier* pBarriers);

protected:
	enum PipelineLayoutSlot : uint8_t
	{
		OUTPUT_VIEW,
		SHADER_RESOURCES,
		SAMPLER,
		VS_CONSTANTS,
		PS_CONSTANTS = OUTPUT_VIEW
	};

	enum PipelineIndex : uint8_t
	{
		BASE_PASS,
		ENVIRONMENT,
		TEMPORAL_AA,
		TONE_MAP,

		NUM_PIPELINE
	};

	enum SRVTable : uint8_t
	{
		SRV_TABLE_BASE,
		SRV_TABLE_GT,
		SRV_TABLE_TAA,
		SRV_TABLE_TAA1,
		SRV_TABLE_TM,
		SRV_TABLE_TM1,

		NUM_SRV_TABLE
	};

	enum UAVTable : uint8_t
	{
		UAV_TABLE_TAA,
		UAV_TABLE_TAA1,

		NUM_UAV_TABLE
	};

	enum RenderTarget : uint8_t
	{
		RT_COLOR,
		RT_VELOCITY,

		NUM_RENDER_TARGET
	};

	enum OutputView : uint8_t
	{
		UAV_PP_TAA,
		UAV_PP_TAA1,

		NUM_OUTPUT_VIEW
	};

	struct BasePassConstants
	{
		DirectX::XMFLOAT4X4	WorldViewProj;
		DirectX::XMFLOAT4X4	WorldViewProjPrev;
		DirectX::XMFLOAT4X4	World;
		DirectX::XMFLOAT2	ProjBias;
	};

	struct PerFrameConstants
	{
		DirectX::XMFLOAT4	EyePt;
		DirectX::XMFLOAT4X4	ScreenToWorld;
		DirectX::XMFLOAT2	Viewport;
	};

	bool createVB(const XUSG::CommandList& commandList, uint32_t numVert,
		uint32_t stride, const uint8_t* pData, std::vector<XUSG::Resource>& uploaders);
	bool createIB(const XUSG::CommandList& commandList, uint32_t numIndices,
		const uint32_t* pData, std::vector<XUSG::Resource>& uploaders);
	bool createInputLayout();
	bool createPipelineLayouts();
	bool createPipelines(XUSG::Format rtFormat);
	bool createDescriptorTables();

	void render(const XUSG::CommandList& commandList, bool isGroundTruth, bool needClear);
	void environment(const XUSG::CommandList& commandList);
	void temporalAA(const XUSG::CommandList& commandList);

	XUSG::Device m_device;

	uint32_t	m_numIndices;
	uint8_t		m_frameParity;

	DirectX::XMUINT2	m_viewport;
	DirectX::XMFLOAT4	m_posScale;
	BasePassConstants	m_cbBasePass;
	PerFrameConstants	m_cbPerFrame;

	XUSG::InputLayout		m_inputLayout;
	XUSG::PipelineLayout	m_pipelineLayouts[NUM_PIPELINE];
	XUSG::Pipeline			m_pipelines[NUM_PIPELINE];

	XUSG::DescriptorTable	m_srvTables[NUM_SRV_TABLE];
	XUSG::DescriptorTable	m_uavTables[NUM_UAV_TABLE];
	XUSG::DescriptorTable	m_samplerTable;
	XUSG::RenderTargetTable	m_rtvTable;

	XUSG::VertexBuffer		m_vertexBuffer;
	XUSG::IndexBuffer		m_indexBuffer;

	XUSG::RenderTarget		m_renderTargets[NUM_RENDER_TARGET];
	XUSG::Texture2D			m_outputViews[NUM_OUTPUT_VIEW];
	XUSG::DepthStencil		m_depth;

	XUSG::ShaderPool				m_shaderPool;
	XUSG::Graphics::PipelineCache	m_graphicsPipelineCache;
	XUSG::Compute::PipelineCache	m_computePipelineCache;
	XUSG::PipelineLayoutCache		m_pipelineLayoutCache;
	std::shared_ptr<XUSG::DescriptorTableCache> m_descriptorTableCache;
};
