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

#include "IrradianceMap.h"

using namespace std;
using namespace XUSG;

const float g_FOVAngleY = XM_PIDIV4;
const float g_zNear = 1.0f;
const float g_zFar = 1000.0f;

Renderer::RenderMode g_renderMode = Renderer::MIP_APPROX;
const auto g_backFormat = Format::B8G8R8A8_UNORM;

IrradianceMap::IrradianceMap(uint32_t width, uint32_t height, wstring name) :
	DXFramework(width, height, name),
	m_typedUAV(false),
	m_frameIndex(0),
	m_pipelineType(LightProbe::HYBRID),
	m_glossy(1.0f),
	m_showFPS(true),
	m_isPaused(true),
	m_tracking(false),
	m_meshFileName("Assets/bunny.obj"),
	m_meshPosScale(0.0f, 0.0f, 0.0f, 1.0f)
{
#if defined (_DEBUG)
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
	AllocConsole();
	FILE* stream;
	freopen_s(&stream, "CONIN$", "r+t", stdin);
	freopen_s(&stream, "CONOUT$", "w+t", stdout);
	freopen_s(&stream, "CONOUT$", "w+t", stderr);
#endif

	m_envFileNames =
	{
		L"Assets/uffizi_cross.dds",
		L"Assets/grace_cross.dds",
		L"Assets/rnl_cross.dds",
		L"Assets/galileo_cross.dds",
		L"Assets/stpeters_cross.dds"
	};
}

IrradianceMap::~IrradianceMap()
{
#if defined (_DEBUG)
	FreeConsole();
#endif
}

void IrradianceMap::OnInit()
{
	LoadPipeline();
	LoadAssets();
}

// Load the rendering pipeline dependencies.
void IrradianceMap::LoadPipeline()
{
	auto dxgiFactoryFlags = 0u;

#if defined(_DEBUG)
	// Enable the debug layer (requires the Graphics Tools "optional feature").
	// NOTE: Enabling the debug layer after device creation will invalidate the active device.
	{
		com_ptr<ID3D12Debug1> debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
		{
			debugController->EnableDebugLayer();
			//debugController->SetEnableGPUBasedValidation(TRUE);

			// Enable additional debug layers.
			dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
		}
	}
#endif

	ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&m_factory)));

	DXGI_ADAPTER_DESC1 dxgiAdapterDesc;
	com_ptr<IDXGIAdapter1> dxgiAdapter;
	auto hr = DXGI_ERROR_UNSUPPORTED;
	for (auto i = 0u; hr == DXGI_ERROR_UNSUPPORTED; ++i)
	{
		dxgiAdapter = nullptr;
		ThrowIfFailed(m_factory->EnumAdapters1(i, &dxgiAdapter));

		m_device = Device::MakeUnique();
		hr = m_device->Create(dxgiAdapter.get(), D3D_FEATURE_LEVEL_11_0);
	}

	dxgiAdapter->GetDesc1(&dxgiAdapterDesc);
	if (dxgiAdapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
		m_title += dxgiAdapterDesc.VendorId == 0x1414 && dxgiAdapterDesc.DeviceId == 0x8c ? L" (WARP)" : L" (Software)";
	ThrowIfFailed(hr);

	D3D12_FEATURE_DATA_D3D12_OPTIONS featureData = {};
	const auto pDevice = static_cast<ID3D12Device*>(m_device->GetHandle());
	hr = pDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &featureData, sizeof(featureData));
	if (SUCCEEDED(hr))
	{
		// TypedUAVLoadAdditionalFormats contains a Boolean that tells you whether the feature is supported or not
		if (featureData.TypedUAVLoadAdditionalFormats)
		{
			// Can assume "all-or-nothing" subset is supported (e.g. R32G32B32A32_FLOAT)
			// Cannot assume other formats are supported, so we check:
			D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport = { DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE };
			hr = pDevice->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport, sizeof(formatSupport));
			if (SUCCEEDED(hr) && (formatSupport.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD))
				m_typedUAV = true;
		}
	}

	// Create the command queue.
	m_commandQueue = CommandQueue::MakeUnique();
	XUSG_N_RETURN(m_commandQueue->Create(m_device.get(), CommandListType::DIRECT, CommandQueueFlag::NONE,
		0, 0, L"CommandQueue"), ThrowIfFailed(E_FAIL));

	// Create the swap chain.
	CreateSwapchain();

	// Reset the index to the current back buffer.
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// Create a command allocator for each frame.
	for (uint8_t n = 0; n < FrameCount; ++n)
	{
		m_commandAllocators[n] = CommandAllocator::MakeUnique();
		XUSG_N_RETURN(m_commandAllocators[n]->Create(m_device.get(), CommandListType::DIRECT,
			(L"CommandAllocator" + to_wstring(n)).c_str()), ThrowIfFailed(E_FAIL));
	}

	// Create descriptor-table lib.
	m_descriptorTableLib = DescriptorTableLib::MakeShared(m_device.get(), L"DescriptorTableLib");

}

// Load the sample assets.
void IrradianceMap::LoadAssets()
{
	// Create the command list.
	m_commandList = CommandList::MakeUnique();
	const auto pCommandList = m_commandList.get();
	XUSG_N_RETURN(pCommandList->Create(m_device.get(), 0, CommandListType::DIRECT,
		m_commandAllocators[m_frameIndex].get(), nullptr), ThrowIfFailed(E_FAIL));

	vector<Resource::uptr> uploaders(0);

	m_lightProbe = make_unique<LightProbe>();
	XUSG_N_RETURN(m_lightProbe->Init(pCommandList, m_descriptorTableLib, uploaders, m_envFileNames.data(),
		static_cast<uint32_t>(m_envFileNames.size()), m_typedUAV), ThrowIfFailed(E_FAIL));

	m_renderer = make_unique<Renderer>();
	XUSG_N_RETURN(m_renderer->Init(pCommandList, m_descriptorTableLib, uploaders,
		m_meshFileName.c_str(), g_backFormat, m_meshPosScale), ThrowIfFailed(E_FAIL));

	if (g_renderMode == Renderer::GROUND_TRUTH)
	{
		const auto pIrradianctGT = m_lightProbe->GetIrradianceGT(m_commandList.get(), (m_envFileNames[0] + L"_gt.dds").c_str(), &uploaders);
		if (!m_renderer->SetLightProbesGT(pIrradianctGT->GetSRV(), m_lightProbe->GetRadiance()->GetSRV()))
			ThrowIfFailed(E_FAIL);
	}
	
	// Close the command list and execute it to begin the initial GPU setup.
	XUSG_N_RETURN(pCommandList->Close(), ThrowIfFailed(E_FAIL));
	m_commandQueue->ExecuteCommandList(pCommandList);

	// Create synchronization objects and wait until assets have been uploaded to the GPU.
	{
		if (!m_fence)
		{
			m_fence = Fence::MakeUnique();
			XUSG_N_RETURN(m_fence->Create(m_device.get(), m_fenceValues[m_frameIndex]++, FenceFlag::NONE, L"Fence"), ThrowIfFailed(E_FAIL));
		}

		// Create an event handle to use for frame synchronization.
		m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (!m_fenceEvent) ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));

		// Wait for the command list to execute; we are reusing the same command 
		// list in our main loop but for now, we just want to wait for setup to 
		// complete before continuing.
		WaitForGpu();
	}

	// Create window size dependent resources.
	//m_descriptorTableLib->ResetDescriptorPool(CBV_SRV_UAV_HEAP, 0);
	CreateResources();

	// Projection
	const auto aspectRatio = m_width / static_cast<float>(m_height);
	const auto proj = XMMatrixPerspectiveFovLH(g_FOVAngleY, aspectRatio, g_zNear, g_zFar);
	XMStoreFloat4x4(&m_proj, proj);

	// View initialization
	m_focusPt = XMFLOAT3(0.0f, 4.0f, 0.0f);
	m_eyePt = XMFLOAT3(4.0f, 6.0f, -20.0f);
	const auto focusPt = XMLoadFloat3(&m_focusPt);
	const auto eyePt = XMLoadFloat3(&m_eyePt);
	const auto view = XMMatrixLookAtLH(eyePt, focusPt, XMVectorSet(0.0f, 1.0f, 0.0f, 1.0f));
	XMStoreFloat4x4(&m_view, view);
}

void IrradianceMap::CreateSwapchain()
{
	// Describe and create the swap chain.
	m_swapChain = SwapChain::MakeUnique();
	XUSG_N_RETURN(m_swapChain->Create(m_factory.get(), Win32Application::GetHwnd(), m_commandQueue->GetHandle(),
		FrameCount, m_width, m_height, g_backFormat, SwapChainFlag::ALLOW_TEARING), ThrowIfFailed(E_FAIL));

	// This class does not support exclusive full-screen mode and prevents DXGI from responding to the ALT+ENTER shortcut.
	ThrowIfFailed(m_factory->MakeWindowAssociation(Win32Application::GetHwnd(), DXGI_MWA_NO_ALT_ENTER));
}

void IrradianceMap::CreateResources()
{
	// Obtain the back buffers for this window which will be the final render targets
	// and create render target views for each of them.
	for (uint8_t n = 0; n < FrameCount; ++n)
	{
		m_renderTargets[n] = RenderTarget::MakeUnique();
		XUSG_N_RETURN(m_renderTargets[n]->CreateFromSwapChain(m_device.get(), m_swapChain.get(), n), ThrowIfFailed(E_FAIL));
	}

	XUSG_N_RETURN(m_lightProbe->CreateDescriptorTables(m_device.get()), ThrowIfFailed(E_FAIL));
	XUSG_N_RETURN(m_renderer->SetLightProbes(m_lightProbe->GetIrradiance()->GetSRV(),
		m_lightProbe->GetRadiance()->GetSRV()), ThrowIfFailed(E_FAIL));
	XUSG_N_RETURN(m_renderer->SetViewport(m_device.get(), m_width, m_height), ThrowIfFailed(E_FAIL));
}

// Update frame-based values.
void IrradianceMap::OnUpdate()
{
	// Timer
	static auto time = 0.0, pauseTime = 0.0;

	m_timer.Tick();
	float timeStep;
	const auto totalTime = CalculateFrameStats(&timeStep);
	pauseTime = m_isPaused ? totalTime - time : pauseTime;
	timeStep = m_isPaused ? 0.0f : timeStep;
	time = totalTime - pauseTime;

	// View
	const auto eyePt = XMLoadFloat3(&m_eyePt);
	const auto view = XMLoadFloat4x4(&m_view);
	const auto proj = XMLoadFloat4x4(&m_proj);
	m_lightProbe->UpdateFrame(g_renderMode == Renderer::MIP_APPROX ? time : 0.0, m_frameIndex);
	m_renderer->UpdateFrame(m_frameIndex, eyePt, view * proj, m_glossy, m_isPaused);
}

// Render the scene.
void IrradianceMap::OnRender()
{
	// Record all the commands we need to render the scene into the command list.
	PopulateCommandList();

	// Execute the command list.
	m_commandQueue->ExecuteCommandList(m_commandList.get());

	// Present the frame.
	XUSG_N_RETURN(m_swapChain->Present(0, PresentFlag::ALLOW_TEARING), ThrowIfFailed(E_FAIL));

	MoveToNextFrame();
}

void IrradianceMap::OnDestroy()
{
	// Ensure that the GPU is no longer referencing resources that are about to be
	// cleaned up by the destructor.
	WaitForGpu();

	CloseHandle(m_fenceEvent);
}

void IrradianceMap::OnWindowSizeChanged(int width, int height)
{
	if (!Win32Application::GetHwnd())
	{
		throw std::exception("Call SetWindow with a valid Win32 window handle");
	}

	// Wait until all previous GPU work is complete.
	WaitForGpu();

	// Release resources that are tied to the swap chain and update fence values.
	for (uint8_t n = 0; n < FrameCount; ++n)
	{
		m_renderTargets[n].reset();
		m_fenceValues[n] = m_fenceValues[m_frameIndex];
	}
	m_descriptorTableLib->ResetDescriptorHeap(CBV_SRV_UAV_HEAP, 0);
	m_descriptorTableLib->ResetDescriptorHeap(RTV_HEAP, 0);

	// Determine the render target size in pixels.
	m_width = (max)(width, 1);
	m_height = (max)(height, 1);

	// If the swap chain already exists, resize it, otherwise create one.
	if (m_swapChain)
	{
		// If the swap chain already exists, resize it.
		const auto hr = m_swapChain->ResizeBuffers(FrameCount, m_width,
			m_height, g_backFormat, SwapChainFlag::ALLOW_TEARING);

		if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
		{
#ifdef _DEBUG
			char buff[64] = {};
			sprintf_s(buff, "Device Lost on ResizeBuffers: Reason code 0x%08X\n", (hr == DXGI_ERROR_DEVICE_REMOVED) ? m_device->GetDeviceRemovedReason() : hr);
			OutputDebugStringA(buff);
#endif
			// If the device was removed for any reason, a new device and swap chain will need to be created.
			//HandleDeviceLost();

			// Everything is set up now. Do not continue execution of this method. HandleDeviceLost will reenter this method 
			// and correctly set up the new device.
			return;
		}
		else
		{
			ThrowIfFailed(hr);
		}
	}
	else CreateSwapchain();

	// Reset the index to the current back buffer.
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// Create window size dependent resources.
	CreateResources();

	// Projection
	{
		const auto aspectRatio = m_width / static_cast<float>(m_height);
		const auto proj = XMMatrixPerspectiveFovLH(g_FOVAngleY, aspectRatio, g_zNear, g_zFar);
		XMStoreFloat4x4(&m_proj, proj);
	}
}

// User hot-key interactions.
void IrradianceMap::OnKeyUp(uint8_t key)
{
	switch (key)
	{
	case VK_SPACE:
		m_isPaused = !m_isPaused;
		break;
	case VK_F1:
		m_showFPS = !m_showFPS;
		break;
	case 'G':
		m_glossy = 1.0f - m_glossy;
		break;
	case 'P':
		const auto inc = m_pipelineType == LightProbe::COMPUTE - 1 && !m_typedUAV ? 2 : 1;
		m_pipelineType = static_cast<LightProbe::PipelineType>((m_pipelineType + inc) % LightProbe::NUM_PIPE_TYPE);
		break;
	}
}

// User camera interactions.
void IrradianceMap::OnLButtonDown(float posX, float posY)
{
	m_tracking = true;
	m_mousePt = XMFLOAT2(posX, posY);
}

void IrradianceMap::OnLButtonUp(float posX, float posY)
{
	m_tracking = false;
}

void IrradianceMap::OnMouseMove(float posX, float posY)
{
	if (m_tracking)
	{
		const auto dPos = XMFLOAT2(m_mousePt.x - posX, m_mousePt.y - posY);

		XMFLOAT2 radians;
		radians.x = XM_2PI * dPos.y / m_height;
		radians.y = XM_2PI * dPos.x / m_width;

		const auto focusPt = XMLoadFloat3(&m_focusPt);
		auto eyePt = XMLoadFloat3(&m_eyePt);

		const auto len = XMVectorGetX(XMVector3Length(focusPt - eyePt));
		auto transform = XMMatrixTranslation(0.0f, 0.0f, -len);
		transform *= XMMatrixRotationRollPitchYaw(radians.x, radians.y, 0.0f);
		transform *= XMMatrixTranslation(0.0f, 0.0f, len);

		const auto view = XMLoadFloat4x4(&m_view) * transform;
		const auto viewInv = XMMatrixInverse(nullptr, view);
		eyePt = viewInv.r[3];

		XMStoreFloat3(&m_eyePt, eyePt);
		XMStoreFloat4x4(&m_view, view);

		m_mousePt = XMFLOAT2(posX, posY);
	}
}

void IrradianceMap::OnMouseWheel(float deltaZ, float posX, float posY)
{
	const auto focusPt = XMLoadFloat3(&m_focusPt);
	auto eyePt = XMLoadFloat3(&m_eyePt);

	const auto len = XMVectorGetX(XMVector3Length(focusPt - eyePt));
	const auto transform = XMMatrixTranslation(0.0f, 0.0f, -len * deltaZ / 16.0f);

	const auto view = XMLoadFloat4x4(&m_view) * transform;
	const auto viewInv = XMMatrixInverse(nullptr, view);
	eyePt = viewInv.r[3];

	XMStoreFloat3(&m_eyePt, eyePt);
	XMStoreFloat4x4(&m_view, view);
}

void IrradianceMap::OnMouseLeave()
{
	m_tracking = false;
}

void IrradianceMap::ParseCommandLineArgs(wchar_t* argv[], int argc)
{
	DXFramework::ParseCommandLineArgs(argv, argc);

	for (auto i = 1; i < argc; ++i)
	{
		if (_wcsnicmp(argv[i], L"-mesh", wcslen(argv[i])) == 0 ||
			_wcsnicmp(argv[i], L"/mesh", wcslen(argv[i])) == 0)
		{
			if (i + 1 < argc)
			{
				m_meshFileName.resize(wcslen(argv[i + 1]));
				for (size_t j = 0; j < m_meshFileName.size(); ++j)
					m_meshFileName[j] = static_cast<char>(argv[i + 1][j]);
			}
			m_meshPosScale.x = i + 2 < argc ? static_cast<float>(_wtof(argv[i + 2])) : m_meshPosScale.x;
			m_meshPosScale.y = i + 3 < argc ? static_cast<float>(_wtof(argv[i + 3])) : m_meshPosScale.y;
			m_meshPosScale.z = i + 4 < argc ? static_cast<float>(_wtof(argv[i + 4])) : m_meshPosScale.z;
			m_meshPosScale.w = i + 5 < argc ? static_cast<float>(_wtof(argv[i + 5])) : m_meshPosScale.w;
		}
		else if (_wcsnicmp(argv[i], L"-env", wcslen(argv[i])) == 0 ||
			_wcsnicmp(argv[i], L"/env", wcslen(argv[i])) == 0)
		{
			m_envFileNames.clear();
			for (auto j = i + 1; j < argc; ++j)
				m_envFileNames.emplace_back(argv[j]);
		}
		else if (_wcsnicmp(argv[i], L"-gt", wcslen(argv[i])) == 0 ||
			_wcsnicmp(argv[i], L"/gt", wcslen(argv[i])) == 0)
		{
			m_envFileNames.clear();
			if (i + 1 < argc) m_envFileNames.emplace_back(argv[i + 1]);
			g_renderMode = Renderer::GROUND_TRUTH;
		}
	}
}

void IrradianceMap::PopulateCommandList()
{
	// Command list allocators can only be reset when the associated 
	// command lists have finished execution on the GPU; apps should use 
	// fences to determine GPU execution progress.
	const auto pCommandAllocator = m_commandAllocators[m_frameIndex].get();
	XUSG_N_RETURN(pCommandAllocator->Reset(), ThrowIfFailed(E_FAIL));

	// However, when ExecuteCommandList() is called on a particular command 
	// list, that command list can then be reset at any time and must be before 
	// re-recording.
	const auto pCommandList = m_commandList.get();
	XUSG_N_RETURN(pCommandList->Reset(pCommandAllocator, nullptr), ThrowIfFailed(E_FAIL));

	// Record commands.
	// Set descriptor heaps
	const DescriptorHeap descriptorHeaps[] =
	{
		m_descriptorTableLib->GetDescriptorHeap(CBV_SRV_UAV_HEAP),
		m_descriptorTableLib->GetDescriptorHeap(SAMPLER_HEAP)
	};
	pCommandList->SetDescriptorHeaps(static_cast<uint32_t>(size(descriptorHeaps)), descriptorHeaps);

	m_lightProbe->Process(pCommandList, m_frameIndex, m_pipelineType);	// V-cycle

	const auto renderMode = m_pipelineType == LightProbe::SH && g_renderMode != Renderer::GROUND_TRUTH ? Renderer::SH_APPROX : g_renderMode;
	if (renderMode == Renderer::SH_APPROX) m_renderer->SetLightProbesSH(m_lightProbe->GetSH());

	ResourceBarrier barriers[11];
	const auto dstState = ResourceState::NON_PIXEL_SHADER_RESOURCE | ResourceState::PIXEL_SHADER_RESOURCE;
	auto numBarriers = 0u;
	for (uint8_t i = 0; i < LightProbe::CubeMapFaceCount; ++i)
		numBarriers = m_lightProbe->GetIrradiance()->SetBarrier(barriers, 0, dstState, numBarriers, i);
	numBarriers = m_renderTargets[m_frameIndex]->SetBarrier(barriers, ResourceState::RENDER_TARGET,
		numBarriers, XUSG_BARRIER_ALL_SUBRESOURCES, BarrierFlag::BEGIN_ONLY);
	m_renderer->Render(pCommandList, m_frameIndex, barriers, numBarriers, renderMode);

	numBarriers = m_renderTargets[m_frameIndex]->SetBarrier(barriers, ResourceState::RENDER_TARGET,
		0, XUSG_BARRIER_ALL_SUBRESOURCES, BarrierFlag::END_ONLY);
	m_renderer->Postprocess(pCommandList, m_renderTargets[m_frameIndex]->GetRTV(), numBarriers, barriers);
	
	// Indicate that the back buffer will now be used to present.
	numBarriers = m_renderTargets[m_frameIndex]->SetBarrier(barriers, ResourceState::PRESENT);
	pCommandList->Barrier(numBarriers, barriers);

	XUSG_N_RETURN(pCommandList->Close(), ThrowIfFailed(E_FAIL));
}

// Wait for pending GPU work to complete.
void IrradianceMap::WaitForGpu()
{
	// Schedule a Signal command in the queue.
	XUSG_N_RETURN(m_commandQueue->Signal(m_fence.get(), m_fenceValues[m_frameIndex]), ThrowIfFailed(E_FAIL));

	// Wait until the fence has been processed.
	XUSG_N_RETURN(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent), ThrowIfFailed(E_FAIL));
	WaitForSingleObject(m_fenceEvent, INFINITE);

	// Increment the fence value for the current frame.
	m_fenceValues[m_frameIndex]++;
}

// Prepare to render the next frame.
void IrradianceMap::MoveToNextFrame()
{
	// Schedule a Signal command in the queue.
	const auto currentFenceValue = m_fenceValues[m_frameIndex];
	XUSG_N_RETURN(m_commandQueue->Signal(m_fence.get(), currentFenceValue), ThrowIfFailed(E_FAIL));

	// Update the frame index.
	m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	// If the next frame is not ready to be rendered yet, wait until it is ready.
	if (m_fence->GetCompletedValue() < m_fenceValues[m_frameIndex])
	{
		XUSG_N_RETURN(m_fence->SetEventOnCompletion(m_fenceValues[m_frameIndex], m_fenceEvent), ThrowIfFailed(E_FAIL));
		WaitForSingleObject(m_fenceEvent, INFINITE);
	}

	// Set the fence value for the next frame.
	m_fenceValues[m_frameIndex] = currentFenceValue + 1;
}

double IrradianceMap::CalculateFrameStats(float* pTimeStep)
{
	static int frameCnt = 0;
	static double elapsedTime = 0.0;
	static double previousTime = 0.0;
	const auto totalTime = m_timer.GetTotalSeconds();
	++frameCnt;

	const auto timeStep = static_cast<float>(totalTime - elapsedTime);

	// Compute averages over one second period.
	if ((totalTime - elapsedTime) >= 1.0f)
	{
		float fps = static_cast<float>(frameCnt) / timeStep;	// Normalize to an exact second.

		frameCnt = 0;
		elapsedTime = totalTime;

		wstringstream windowText;
		windowText << L"    fps: ";
		if (m_showFPS) windowText << setprecision(2) << fixed << fps;
		else windowText << L"[F1]";

		windowText << L"    [P] ";
		switch (m_pipelineType)
		{
		case LightProbe::GRAPHICS:
			windowText << L"Pure graphics pipelines";
			break;
		case LightProbe::COMPUTE:
			windowText << L"Pure compute pipelines";
			break;
		case LightProbe::SH:
			windowText << L"Spherical harmonics";
			break;
		default:
			windowText << L"Hybrid pipelines (mip-gen by compute and up-sampling by graphics)";
		}

		windowText << L"    [G] Glossy " << m_glossy;
		SetCustomWindowText(windowText.str().c_str());
	}

	if (pTimeStep)* pTimeStep = static_cast<float>(totalTime - previousTime);
	previousTime = totalTime;

	return totalTime;
}
