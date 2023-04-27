//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#pragma once

#include "DXFramework.h"
#include "StepTimer.h"
#include "LightProbe.h"
#include "Renderer.h"

using namespace DirectX;

// Note that while ComPtr is used to manage the lifetime of resources on the CPU,
// it has no understanding of the lifetime of resources on the GPU. Apps must account
// for the GPU lifetime of resources to avoid destroying objects that may still be
// referenced by the GPU.
// An example of this can be found in the class method: OnDestroy().

class IrradianceMap : public DXFramework
{
public:
	IrradianceMap(uint32_t width, uint32_t height, std::wstring name);
	virtual ~IrradianceMap();

	virtual void OnInit();
	virtual void OnUpdate();
	virtual void OnRender();
	virtual void OnDestroy();

	virtual void OnWindowSizeChanged(int width, int height);

	virtual void OnKeyUp(uint8_t /*key*/);
	virtual void OnLButtonDown(float posX, float posY);
	virtual void OnLButtonUp(float posX, float posY);
	virtual void OnMouseMove(float posX, float posY);
	virtual void OnMouseWheel(float deltaZ, float posX, float posY);
	virtual void OnMouseLeave();

	virtual void ParseCommandLineArgs(wchar_t* argv[], int argc);

private:
	static const uint8_t FrameCount = LightProbe::FrameCount;
	static_assert(FrameCount == Renderer::FrameCount, "IrradianceMap::FrameCount should be equal to Renderer::FrameCount");

	XUSG::com_ptr<IDXGIFactory5> m_factory;

	XUSG::DescriptorTableLib::sptr	m_descriptorTableLib;

	XUSG::SwapChain::uptr			m_swapChain;
	XUSG::CommandAllocator::uptr	m_commandAllocators[FrameCount];
	XUSG::CommandQueue::uptr		m_commandQueue;

	XUSG::Device::uptr			m_device;
	XUSG::RenderTarget::uptr	m_renderTargets[FrameCount];
	XUSG::CommandList::uptr		m_commandList;

	// App resources.
	std::unique_ptr<LightProbe>	m_lightProbe;
	std::unique_ptr<Renderer>	m_renderer;
	XMFLOAT4X4	m_proj;
	XMFLOAT4X4	m_view;
	XMFLOAT3	m_focusPt;
	XMFLOAT3	m_eyePt;

	bool		m_typedUAV;

	// Synchronization objects.
	uint8_t		m_frameIndex;
	HANDLE		m_fenceEvent;
	XUSG::Fence::uptr m_fence;
	uint64_t	m_fenceValues[FrameCount];

	// Screen-shot helper
	XUSG::Buffer::uptr		m_readBuffer;

	// Application state
	LightProbe::PipelineType m_pipelineType;
	float		m_glossy;
	bool		m_showFPS;
	bool		m_isPaused;
	StepTimer	m_timer;

	// User camera interactions
	bool m_tracking;
	XMFLOAT2 m_mousePt;

	// Screen-shot state
	uint8_t		m_screenShot;

	// User external settings
	std::string m_meshFileName;
	std::vector<std::wstring> m_envFileNames;
	XMFLOAT4 m_meshPosScale;

	void LoadPipeline();
	void LoadAssets();
	void CreateSwapchain();
	void CreateResources();
	void PopulateCommandList();
	void WaitForGpu();
	void MoveToNextFrame();
	void SaveImage(char const* fileName, XUSG::Buffer* imageBuffer, uint32_t w, uint32_t h, uint8_t comp = 3);
	double CalculateFrameStats(float* fTimeStep = nullptr);
};
