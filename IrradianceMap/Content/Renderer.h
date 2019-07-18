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

	void UpdateFrame(uint32_t frameIndex, DirectX::CXMVECTOR eyePt, DirectX::CXMMATRIX viewProj, bool isPaused);
	void Render(const XUSG::CommandList& commandList, uint32_t frameIndex);
	void ToneMap(const XUSG::CommandList& commandList, const XUSG::RenderTargetTable& rtvTable,
		uint32_t numBarriers, XUSG::ResourceBarrier* pBarriers);

	static const uint32_t FrameCount = 3;

protected:
	enum PipelineLayoutSlot : uint8_t
	{
		OUTPUT_VIEW,
		SHADER_RESOURCES,
		SAMPLER,
		PS_CONSTANTS,
		VS_CONSTANTS = OUTPUT_VIEW
	};

	enum PipelineIndex : uint8_t
	{
		BASE_PASS,
		TEMPORAL_AA,
		TONE_MAP,

		NUM_PIPELINE
	};

	enum SRVTable : uint8_t
	{
		SRV_TABLE_BASE,
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
		DirectX::XMFLOAT4X4	Normal;
		DirectX::XMFLOAT2	ProjBias;
	};

	bool createVB(const XUSG::CommandList& commandList, uint32_t numVert,
		uint32_t stride, const uint8_t* pData, std::vector<XUSG::Resource>& uploaders);
	bool createIB(const XUSG::CommandList& commandList, uint32_t numIndices,
		const uint32_t* pData, std::vector<XUSG::Resource>& uploaders);
	bool createInputLayout();
	bool createPipelineLayouts();
	bool createPipelines(XUSG::Format rtFormat);
	bool createDescriptorTables();

	void render(const XUSG::CommandList& commandList);
	void temporalAA(const XUSG::CommandList& commandList);

	XUSG::Device m_device;

	uint32_t	m_numIndices;
	uint8_t		m_frameParity;

	DirectX::XMUINT2	m_viewport;
	DirectX::XMFLOAT4	m_posScale;
	DirectX::XMFLOAT4X4 m_world;
	DirectX::XMFLOAT3	m_eyePt;
	BasePassConstants	m_cbBasePass;

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
