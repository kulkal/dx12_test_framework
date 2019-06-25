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

#include "stdafx.h"
#include "dx12_framework.h"
#include "occcity.h"
#include <iostream>
#include <algorithm>
#include <array>
#include "enkiTS/TaskScheduler.h""
#include "DXCAPI/dxcapi.use.h"


#include <sstream>
#include <fstream>

#define arraysize(a) (sizeof(a)/sizeof(a[0]))
#define align_to(_alignment, _val) (((_val + _alignment - 1) / _alignment) * _alignment)


static dxc::DxcDllSupport gDxcDllHelper;

enki::TaskScheduler g_TS;


template<class BlotType>
std::string convertBlobToString(BlotType* pBlob)
{
	std::vector<char> infoLog(pBlob->GetBufferSize() + 1);
	memcpy(infoLog.data(), pBlob->GetBufferPointer(), pBlob->GetBufferSize());
	infoLog[pBlob->GetBufferSize()] = 0;
	return std::string(infoLog.data());
}

ComPtr<ID3DBlob> compileLibrary(const WCHAR* filename, const WCHAR* targetString)
{
	// Initialize the helper
	gDxcDllHelper.Initialize();
	ComPtr<IDxcCompiler> pCompiler;
	ComPtr<IDxcLibrary> pLibrary;
	gDxcDllHelper.CreateInstance(CLSID_DxcCompiler, __uuidof(IDxcCompiler), &pCompiler);
	gDxcDllHelper.CreateInstance(CLSID_DxcLibrary, __uuidof(IDxcLibrary), &pLibrary);

	// Open and read the file
	std::ifstream shaderFile(filename);
	if (shaderFile.good() == false)
	{
		//msgBox("Can't open file " + wstring_2_string(std::wstring(filename)));
		return nullptr;
	}
	std::stringstream strStream;
	strStream << shaderFile.rdbuf();
	std::string shader = strStream.str();

	// Create blob from the string
	ComPtr<IDxcBlobEncoding> pTextBlob;
	pLibrary->CreateBlobWithEncodingFromPinned((LPBYTE)shader.c_str(), (uint32_t)shader.size(), 0, &pTextBlob);

	// Compile
	ComPtr<IDxcOperationResult> pResult;
	pCompiler->Compile(pTextBlob.Get(), filename, L"", targetString, nullptr, 0, nullptr, 0, nullptr, &pResult);

	// Verify the result
	HRESULT resultCode;
	pResult->GetStatus(&resultCode);
	if (FAILED(resultCode))
	{
		ComPtr<IDxcBlobEncoding> pError;
		pResult->GetErrorBuffer(&pError);
		std::string log = convertBlobToString(pError.Get());
		//msgBox("Compiler error:\n" + log);
		return nullptr;
	}

	ID3DBlob* pBlob;
	pResult->GetResult((IDxcBlob**)&pBlob);
	return ComPtr<ID3DBlob>(pBlob);
}


ComPtr<ID3D12RootSignature> createRootSignature(ComPtr<ID3D12Device5> pDevice, const D3D12_ROOT_SIGNATURE_DESC& desc)
{
	ComPtr<ID3DBlob> pSigBlob;
	ComPtr<ID3DBlob> pErrorBlob;
	HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &pSigBlob, &pErrorBlob);
	if (FAILED(hr))
	{
		std::string msg = convertBlobToString(pErrorBlob.Get());
		//msgBox(msg);
		return nullptr;
	}
	ComPtr<ID3D12RootSignature> pRootSig;
	pDevice->CreateRootSignature(0, pSigBlob->GetBufferPointer(), pSigBlob->GetBufferSize(), IID_PPV_ARGS(&pRootSig));
	return pRootSig;
}

struct RootSignatureDesc
{
	D3D12_ROOT_SIGNATURE_DESC desc = {};
	std::vector<D3D12_DESCRIPTOR_RANGE> range;
	std::vector<D3D12_ROOT_PARAMETER> rootParams;
};

RootSignatureDesc createRayGenRootDesc()
{
	// Create the root-signature
	RootSignatureDesc desc;
	desc.range.resize(3);
	// gOutput
	desc.range[0].BaseShaderRegister = 0;
	desc.range[0].NumDescriptors = 1;
	desc.range[0].RegisterSpace = 0;
	desc.range[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	desc.range[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	// gRtScene
	desc.range[1].BaseShaderRegister = 0;
	desc.range[1].NumDescriptors = 1;
	desc.range[1].RegisterSpace = 0;
	desc.range[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	desc.range[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

	desc.range[2].BaseShaderRegister = 0;
	desc.range[2].NumDescriptors = 1;
	desc.range[2].RegisterSpace = 0;
	desc.range[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
	desc.range[2].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;


	desc.rootParams.resize(1);
	desc.rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
	desc.rootParams[0].DescriptorTable.NumDescriptorRanges = 3;
	desc.rootParams[0].DescriptorTable.pDescriptorRanges = desc.range.data();

	// Create the desc
	desc.desc.NumParameters = 1;
	desc.desc.pParameters = desc.rootParams.data();
	desc.desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

	return desc;
}

struct DxilLibrary
{
	DxilLibrary(ComPtr<ID3DBlob> pBlob, const WCHAR* entryPoint[], uint32_t entryPointCount) : pShaderBlob(pBlob)
	{
		stateSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
		stateSubobject.pDesc = &dxilLibDesc;

		dxilLibDesc = {};
		exportDesc.resize(entryPointCount);
		exportName.resize(entryPointCount);
		if (pBlob)
		{
			dxilLibDesc.DXILLibrary.pShaderBytecode = pBlob->GetBufferPointer();
			dxilLibDesc.DXILLibrary.BytecodeLength = pBlob->GetBufferSize();
			dxilLibDesc.NumExports = entryPointCount;
			dxilLibDesc.pExports = exportDesc.data();

			for (uint32_t i = 0; i < entryPointCount; i++)
			{
				exportName[i] = entryPoint[i];
				exportDesc[i].Name = exportName[i].c_str();
				exportDesc[i].Flags = D3D12_EXPORT_FLAG_NONE;
				exportDesc[i].ExportToRename = nullptr;
			}
		}
	};

	DxilLibrary() : DxilLibrary(nullptr, nullptr, 0) {}

	D3D12_DXIL_LIBRARY_DESC dxilLibDesc = {};
	D3D12_STATE_SUBOBJECT stateSubobject{};
	ComPtr<ID3DBlob> pShaderBlob;
	std::vector<D3D12_EXPORT_DESC> exportDesc;
	std::vector<std::wstring> exportName;
};


static const WCHAR* kRayGenShader = L"rayGen";
static const WCHAR* kMissShader = L"miss";
static const WCHAR* kClosestHitShader = L"chs";
static const WCHAR* kHitGroup = L"HitGroup";

DxilLibrary createDxilLibrary()
{
	// Compile the shader
	ComPtr<ID3DBlob> pDxilLib = compileLibrary(L"07-Shaders.hlsl", L"lib_6_3");
	const WCHAR* entryPoints[] = { kRayGenShader, kMissShader, kClosestHitShader };
	return DxilLibrary(pDxilLib, entryPoints, arraysize(entryPoints));
}


struct HitProgram
{
	HitProgram(LPCWSTR ahsExport, LPCWSTR chsExport, const std::wstring& name) : exportName(name)
	{
		desc = {};
		desc.AnyHitShaderImport = ahsExport;
		desc.ClosestHitShaderImport = chsExport;
		desc.HitGroupExport = exportName.c_str();

		subObject.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
		subObject.pDesc = &desc;
	}

	std::wstring exportName;
	D3D12_HIT_GROUP_DESC desc;
	D3D12_STATE_SUBOBJECT subObject;
};

struct ExportAssociation
{
	ExportAssociation(const WCHAR* exportNames[], uint32_t exportCount, const D3D12_STATE_SUBOBJECT* pSubobjectToAssociate)
	{
		association.NumExports = exportCount;
		association.pExports = exportNames;
		association.pSubobjectToAssociate = pSubobjectToAssociate;

		subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
		subobject.pDesc = &association;
	}

	D3D12_STATE_SUBOBJECT subobject = {};
	D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association = {};
};

struct LocalRootSignature
{
	LocalRootSignature(ComPtr<ID3D12Device5> pDevice, const D3D12_ROOT_SIGNATURE_DESC& desc)
	{
		pRootSig = createRootSignature(pDevice, desc);
		pInterface = pRootSig.Get();
		subobject.pDesc = &pInterface;
		subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
	}
	ComPtr<ID3D12RootSignature> pRootSig;
	ID3D12RootSignature* pInterface = nullptr;
	D3D12_STATE_SUBOBJECT subobject = {};
};

struct GlobalRootSignature
{
	GlobalRootSignature(ComPtr<ID3D12Device5> pDevice, const D3D12_ROOT_SIGNATURE_DESC& desc)
	{
		pRootSig = createRootSignature(pDevice, desc);
		pInterface = pRootSig.Get();
		subobject.pDesc = &pInterface;
		subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
	}
	ComPtr<ID3D12RootSignature> pRootSig;
	ID3D12RootSignature* pInterface = nullptr;
	D3D12_STATE_SUBOBJECT subobject = {};
};

struct ShaderConfig
{
	ShaderConfig(uint32_t maxAttributeSizeInBytes, uint32_t maxPayloadSizeInBytes)
	{
		shaderConfig.MaxAttributeSizeInBytes = maxAttributeSizeInBytes;
		shaderConfig.MaxPayloadSizeInBytes = maxPayloadSizeInBytes;

		subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
		subobject.pDesc = &shaderConfig;
	}

	D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
	D3D12_STATE_SUBOBJECT subobject = {};
};

struct PipelineConfig
{
	PipelineConfig(uint32_t maxTraceRecursionDepth)
	{
		config.MaxTraceRecursionDepth = maxTraceRecursionDepth;

		subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
		subobject.pDesc = &config;
	}

	D3D12_RAYTRACING_PIPELINE_CONFIG config = {};
	D3D12_STATE_SUBOBJECT subobject = {};
};


dx12_framework::dx12_framework(UINT width, UINT height, std::wstring name) :
	DXSample(width, height, name),
	m_viewport(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)),
	m_scissorRect(0, 0, static_cast<LONG>(width), static_cast<LONG>(height))
{
}

dx12_framework::~dx12_framework()
{
	/*IDXGIDebug *pDebugInterface;
	DXGIGetDebugInterface(__uuidof(IDXGIDebug), (void**)&pDebugInterface);


	pDebugInterface->ReportLiveObjects(DXGI_DEBUG_D3D11, DXGI_DEBUG_RLO_ALL);*/


}

void dx12_framework::OnInit()
{
	g_TS.Initialize(8);

	m_camera.Init({100, 15, 50 });
	m_camera.SetMoveSpeed(200);

	LoadPipeline();
	LoadAssets();
}

// Load the rendering pipeline dependencies.
void dx12_framework::LoadPipeline()
{
	

	UINT dxgiFactoryFlags = 0;

#if 1//defined(_DEBUG)
	// Enable the debug layer (requires the Graphics Tools "optional feature").
	// NOTE: Enabling the debug layer after device creation will invalidate the active device.
	{
		ComPtr<ID3D12Debug> debugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
		{
			debugController->EnableDebugLayer();

			// Enable additional debug layers.
			dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;

		}

		/*ComPtr<ID3D12Debug1> spDebugController1;
		debugController->QueryInterface(IID_PPV_ARGS(&spDebugController1));
		spDebugController1->SetEnableGPUBasedValidation(true);*/


		
	}
#endif

	ComPtr<IDXGIFactory4> factory;
	ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

	if (m_useWarpDevice)
	{
		ComPtr<IDXGIAdapter> warpAdapter;
		ThrowIfFailed(factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)));

		ThrowIfFailed(D3D12CreateDevice(
			warpAdapter.Get(),
			D3D_FEATURE_LEVEL_11_0,
			IID_PPV_ARGS(&m_device)
			));
	}
	else
	{
		ComPtr<IDXGIAdapter1> hardwareAdapter;
		GetHardwareAdapter(factory.Get(), &hardwareAdapter);

		ThrowIfFailed(D3D12CreateDevice(
			hardwareAdapter.Get(),
			D3D_FEATURE_LEVEL_12_1,
			IID_PPV_ARGS(&m_device)
			));

		D3D12_FEATURE_DATA_D3D12_OPTIONS5 features5;
		HRESULT hr = m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &features5, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5));
		if (FAILED(hr) || features5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
		{
			//msgBox("Raytracing is not supported on this device. Make sure your GPU supports DXR (such as Nvidia's Volta or Turing RTX) and you're on the latest drivers. The DXR fallback layer is not supported.");
			ThrowIfFailed(hr);
		}


		ComPtr<IDXGIAdapter3> pDXGIAdapter3;
		hardwareAdapter->QueryInterface(IID_PPV_ARGS(&pDXGIAdapter3));
		
		ThrowIfFailed(pDXGIAdapter3->SetVideoMemoryReservation(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, 2213100441));


		DXGI_QUERY_VIDEO_MEMORY_INFO LocalVideoMemoryInfo;
		ThrowIfFailed(pDXGIAdapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &LocalVideoMemoryInfo));

		// break on error

		ComPtr<ID3D12InfoQueue> d3dInfoQueue;
		if (SUCCEEDED(m_device->QueryInterface(__uuidof(ID3D12InfoQueue), (void**)&d3dInfoQueue)))
		{
			//d3dInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
			//d3dInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
		}
	}



	

	dx12_rhi = std::make_unique<DumRHI_DX12>(m_device.Get());



	ViewParameter = make_unique<GlobalConstantBuffer>(sizeof(SceneConstantBuffer));

	// Describe and create the swap chain.
	DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
	swapChainDesc.BufferCount = dx12_rhi->NumFrame;
	swapChainDesc.Width = m_width;
	swapChainDesc.Height = m_height;
	swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	swapChainDesc.SampleDesc.Count = 1;

	ComPtr<IDXGISwapChain1> swapChain;
	ThrowIfFailed(factory->CreateSwapChainForHwnd(
		dx12_rhi->CommandQueue.Get(),		// Swap chain needs the queue so that it can force a flush on it.
		Win32Application::GetHwnd(),
		&swapChainDesc,
		nullptr,
		nullptr,
		&swapChain
		));

	// This sample does not support fullscreen transitions.
	ThrowIfFailed(factory->MakeWindowAssociation(Win32Application::GetHwnd(), DXGI_MWA_NO_ALT_ENTER));

	ThrowIfFailed(swapChain.As(&m_swapChain));
	//dx12_rhi->m_frameIndex = m_swapChain->GetCurrentBackBufferIndex();

	dx12_rhi->m_swapChain = m_swapChain;

}

// Load the sample assets.
void dx12_framework::LoadAssets()
{
	InitComputeRS();
	InitDrawMeshRS();
	InitCopyPass();
	InitLightingPass();
	

	for (UINT i = 0; i < dx12_rhi->NumFrame; i++)
	{

		ComPtr<ID3D12Resource> rendertarget;
		ThrowIfFailed(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&rendertarget)));

		// create each rtv for one actual resource(swapchain)
		shared_ptr<Texture> rt = dx12_rhi->CreateTexture2DFromResource(rendertarget);
		rt->MakeRTV();
		framebuffers.push_back(rt);
	}

	// lighting result
	ColorBuffer = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R8G8B8A8_UNORM,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);
	ColorBuffer->MakeRTV();
	ColorBuffer->MakeSRV();

	NAME_D3D12_OBJECT(ColorBuffer->resource);

	// world normal
	NormalBuffer = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R8G8B8A8_UNORM,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);
	NormalBuffer->MakeRTV();
	NormalBuffer->MakeSRV();

	NAME_D3D12_OBJECT(NormalBuffer->resource);

	// albedo
	AlbedoBuffer = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R8G8B8A8_UNORM,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, m_width, m_height, 1);
	AlbedoBuffer->MakeRTV();
	AlbedoBuffer->MakeSRV();

	NAME_D3D12_OBJECT(AlbedoBuffer->resource);
	
	// shadow result
	ShadowBuffer = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R8G8B8A8_UNORM,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, m_width, m_height, 1);
	ShadowBuffer->MakeSRV();
	
	ShadowBuffer->MakeUAV();



	NAME_D3D12_OBJECT(ShadowBuffer->resource);

	
	dx12_rhi->depthTexture = dx12_rhi->CreateTexture2D(DXGI_FORMAT_D32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE, m_width, m_height, 1);
	dx12_rhi->depthTexture->MakeDSV();



	// load indoor scene assets
	LoadMesh();



	// Describe and create a sampler.
	D3D12_SAMPLER_DESC samplerDesc = {};
	samplerDesc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplerDesc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	samplerDesc.MinLOD = 0;
	samplerDesc.MaxLOD = D3D12_FLOAT32_MAX;
	samplerDesc.MipLODBias = 0.0f;
	samplerDesc.MaxAnisotropy = 1;
	samplerDesc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;

	samplerWrap = dx12_rhi->CreateSampler(samplerDesc);

	InitRaytracing();

	
	//ThrowIfFailed(dx12_rhi->CommandList->Close());

}

void dx12_framework::LoadMesh()
{
	// Load scene assets.
	UINT fileSize = 0;
	UINT8* pAssetData;
	ThrowIfFailed(ReadDataFromFile(GetAssetFullPath(SampleAssetsIndoor::DataFileName).c_str(), &pAssetData, &fileSize));

	SquintRoom = make_unique<Mesh>();

	// vertex buffer
	/*D3D12_SUBRESOURCE_DATA vertexData = {};
	vertexData.pData = pAssetData + SampleAssetsIndoor::VertexDataOffset;
	vertexData.RowPitch = SampleAssetsIndoor::VertexDataSize;
	vertexData.SlicePitch = vertexData.RowPitch;*/

	SquintRoom->Vb = dx12_rhi->CreateVertexBuffer(SampleAssetsIndoor::VertexDataSize, SampleAssetsIndoor::StandardVertexStride, pAssetData + SampleAssetsIndoor::VertexDataOffset);
	SquintRoom->VertexStride = SampleAssetsIndoor::StandardVertexStride;
	SquintRoom->IndexFormat = SampleAssetsIndoor::StandardIndexFormat;


	// index buffer
	/*D3D12_SUBRESOURCE_DATA indexData = {};
	indexData.pData = pAssetData + SampleAssetsIndoor::IndexDataOffset;
	indexData.RowPitch = SampleAssetsIndoor::IndexDataSize;
	indexData.SlicePitch = indexData.RowPitch;*/
	SquintRoom->Ib = dx12_rhi->CreateIndexBuffer(SampleAssetsIndoor::StandardIndexFormat, SampleAssetsIndoor::IndexDataSize, pAssetData + SampleAssetsIndoor::IndexDataOffset);




	// read textures
	const UINT srvCount = _countof(SampleAssetsIndoor::Textures);

	for (UINT i = 0; i < srvCount; i++)
	{
		D3D12_SUBRESOURCE_DATA textureData = {};
		textureData.pData = pAssetData + SampleAssetsIndoor::Textures[i].Data[0].Offset;
		textureData.RowPitch = SampleAssetsIndoor::Textures[i].Data[0].Pitch;
		textureData.SlicePitch = SampleAssetsIndoor::Textures[i].Data[0].Size;

		//SquintRoom->Textures.push_back(dx12_rhi->CreateTexture2D(SampleAssetsIndoor::Textures[i].Format, SampleAssetsIndoor::Textures[i].Width, SampleAssetsIndoor::Textures[i].Height, SampleAssetsIndoor::Textures[i].MipLevels, &textureData));

		shared_ptr<Texture> tex = dx12_rhi->CreateTexture2D(SampleAssetsIndoor::Textures[i].Format, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, SampleAssetsIndoor::Textures[i].Width, SampleAssetsIndoor::Textures[i].Height, SampleAssetsIndoor::Textures[i].MipLevels);
		tex->MakeSRV();
		tex->UploadSRCData(&textureData);
		SquintRoom->Textures.push_back(tex);
	}

	const UINT drawCount = _countof(SampleAssetsIndoor::Draws);

	for (int i = 0; i < _countof(SampleAssetsIndoor::Draws); i++)
	{
		SampleAssetsIndoor::DrawParameters drawArgs = SampleAssetsIndoor::Draws[i];
		Mesh::DrawCall drawcall;
		drawcall.DiffuseTextureIndex = drawArgs.DiffuseTextureIndex;
		drawcall.IndexCount = drawArgs.IndexCount;
		drawcall.IndexStart = drawArgs.IndexStart;
		drawcall.NormalTextureIndex = drawArgs.NormalTextureIndex;
		drawcall.SpecularTextureIndex = drawArgs.SpecularTextureIndex;
		drawcall.VertexBase = drawArgs.VertexBase;

		if (i > 0)
		{
			auto& prevDraw = SquintRoom->Draws[i - 1];
			prevDraw.VertexCount = drawcall.VertexBase - prevDraw.VertexBase;
		}
		SquintRoom->Draws.push_back(drawcall);
	}
	SquintRoom->Draws[SquintRoom->Draws.size() - 1].VertexCount = SquintRoom->Vb->numVertices - SquintRoom->Draws[SquintRoom->Draws.size() - 1].VertexBase;
}

void dx12_framework::InitComputeRS()
{
	ComPtr<ID3DBlob> computeShader;
#if defined(_DEBUG)
	// Enable better shader debugging with the graphics debugging tools.
	UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
	UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	ID3DBlob*compilationMsgs = nullptr;

	try
	{
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"cs.hlsl").c_str(), nullptr, nullptr, "main", "cs_5_0", compileFlags, 0, &computeShader, &compilationMsgs));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer()));
		compilationMsgs->Release();
	}

	Shader* cs = new Shader((UINT8*)computeShader->GetBufferPointer(), computeShader->GetBufferSize());
	cs->BindUAV("output", 0);
	cs->BindTexture("input", 0, 1);
	//cs->BindTexture("input", 0, 1);

	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	//computePsoDesc.pRootSignature = m_computeRootSignature.Get();

	RS_Compute = unique_ptr<PipelineStateObject>(new PipelineStateObject);
	RS_Compute->cs = unique_ptr<Shader>(cs);
	RS_Compute->computePSODesc = computePsoDesc;
	RS_Compute->Init(true);

	ComputeOuputTexture = dx12_rhi->CreateTexture2D(DXGI_FORMAT_R8G8B8A8_UNORM,
		D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, m_width, m_height, 1);
	ComputeOuputTexture->MakeUAV();
	ComputeOuputTexture->MakeSRV();
}

void dx12_framework::InitDrawMeshRS()
{
	ComPtr<ID3DBlob> vertexShader;
	ComPtr<ID3DBlob> pixelShader;


#if defined(_DEBUG)
	// Enable better shader debugging with the graphics debugging tools.
	UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
	UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	ID3DBlob*compilationMsgs = nullptr;

	try
	{
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"shaders.hlsl").c_str(), nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, &compilationMsgs));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer()));
		compilationMsgs->Release();
	}

	try
	{
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"shaders.hlsl").c_str(), nullptr, nullptr, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, &compilationMsgs));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer()));
		compilationMsgs->Release();
	}

	Shader* ps = new Shader((UINT8*)pixelShader->GetBufferPointer(), pixelShader->GetBufferSize());
	ps->BindTexture("diffuseMap", 0, 1);
	ps->BindTexture("normalMap", 1, 1);

	ps->BindSampler("samplerWrap", 0);

	Shader* vs = new Shader((UINT8*)vertexShader->GetBufferPointer(), vertexShader->GetBufferSize());
	vs->BindGlobalConstantBuffer("ViewParameter", 0);
	vs->BindConstantBuffer("ObjParameter", 1, sizeof(ObjConstantBuffer), _countof(SampleAssetsIndoor::Draws));

	CD3DX12_RASTERIZER_DESC rasterizerStateDesc(D3D12_DEFAULT);
	rasterizerStateDesc.CullMode = D3D12_CULL_MODE_BACK;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDescMesh = {};
	psoDescMesh.InputLayout = { SampleAssetsIndoor::StandardVertexDescription, SampleAssetsIndoor::StandardVertexDescriptionNumElements };
	//psoDesc.pRootSignature = m_rootSignature.Get();
	/*psoDesc.VS = CD3DX12_SHADER_BYTECODE(pVertexShaderData, vertexShaderDataLength);
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(pPixelShaderData, pixelShaderDataLength);*/
	psoDescMesh.RasterizerState = rasterizerStateDesc;
	psoDescMesh.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDescMesh.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDescMesh.SampleMask = UINT_MAX;
	psoDescMesh.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDescMesh.NumRenderTargets = 2;
	psoDescMesh.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDescMesh.RTVFormats[1] = DXGI_FORMAT_R8G8B8A8_UNORM;
	psoDescMesh.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDescMesh.SampleDesc.Count = 1;

	RS_Mesh = unique_ptr<PipelineStateObject>(new PipelineStateObject);
	RS_Mesh->ps = shared_ptr<Shader>(ps);
	RS_Mesh->vs = shared_ptr<Shader>(vs);
	RS_Mesh->graphicsPSODesc = psoDescMesh;
	RS_Mesh->Init(false);
}

void dx12_framework::InitCopyPass()
{
	struct PostVertex
	{
		XMFLOAT4 position;
		XMFLOAT2 uv;
	};

	PostVertex quadVertices[] =
	{
		{ { -1.0f, -1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f } },    // Bottom left.
		{ { -1.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 1.0f } },    // Top left.
		{ { 1.0f, -1.0f, 0.0f, 1.0f }, { 1.0f, 0.0f } },    // Bottom right.
		{ { 1.0f, 1.0f, 0.0f, 1.0f }, { 1.0f, 1.0f } }        // Top right.
	};

	const UINT vertexBufferSize = sizeof(quadVertices);
	const UINT vertexBufferStride = sizeof(PostVertex);

	D3D12_SUBRESOURCE_DATA vertexData = {};
	vertexData.pData = &quadVertices;
	vertexData.RowPitch = vertexBufferSize;
	vertexData.SlicePitch = vertexData.RowPitch;

	FullScreenVB = dx12_rhi->CreateVertexBuffer(vertexBufferSize, vertexBufferStride, &quadVertices);

	///
	ComPtr<ID3DBlob> vertexShader;
	ComPtr<ID3DBlob> pixelShader;


#if defined(_DEBUG)
	// Enable better shader debugging with the graphics debugging tools.
	UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
	UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	ID3DBlob*compilationMsgs = nullptr;

	try
	{
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"CopyPS.hlsl").c_str(), nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, &compilationMsgs));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer()));
		compilationMsgs->Release();
	}

	try
	{
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"CopyPS.hlsl").c_str(), nullptr, nullptr, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, &compilationMsgs));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer()));
		compilationMsgs->Release();
	}

	Shader* ps = new Shader((UINT8*)pixelShader->GetBufferPointer(), pixelShader->GetBufferSize());
	ps->BindTexture("SrcTex", 0, 1);
	ps->BindSampler("samplerWrap", 0);

	Shader* vs = new Shader((UINT8*)vertexShader->GetBufferPointer(), vertexShader->GetBufferSize());
	
	CD3DX12_RASTERIZER_DESC rasterizerStateDesc(D3D12_DEFAULT);
	rasterizerStateDesc.CullMode = D3D12_CULL_MODE_NONE;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDescMesh = {};

	const D3D12_INPUT_ELEMENT_DESC StandardVertexDescription[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
	UINT StandardVertexDescriptionNumElements = _countof(StandardVertexDescription);

	psoDescMesh.InputLayout = { StandardVertexDescription, StandardVertexDescriptionNumElements };
	//psoDesc.pRootSignature = m_rootSignature.Get();
	/*psoDesc.VS = CD3DX12_SHADER_BYTECODE(pVertexShaderData, vertexShaderDataLength);
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(pPixelShaderData, pixelShaderDataLength);*/
	psoDescMesh.RasterizerState = rasterizerStateDesc;
	psoDescMesh.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDescMesh.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDescMesh.DepthStencilState.DepthEnable = FALSE;
	psoDescMesh.DepthStencilState.StencilEnable = FALSE;
	psoDescMesh.SampleMask = UINT_MAX;
	psoDescMesh.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDescMesh.NumRenderTargets = 1;
	psoDescMesh.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	//psoDescMesh.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDescMesh.SampleDesc.Count = 1;

	RS_Copy = unique_ptr<PipelineStateObject>(new PipelineStateObject);
	RS_Copy->ps = shared_ptr<Shader>(ps);
	RS_Copy->vs = shared_ptr<Shader>(vs);
	RS_Copy->graphicsPSODesc = psoDescMesh;
	RS_Copy->Init(false);
}



void dx12_framework::InitLightingPass()
{
	ComPtr<ID3DBlob> vertexShader;
	ComPtr<ID3DBlob> pixelShader;


#if defined(_DEBUG)
	// Enable better shader debugging with the graphics debugging tools.
	UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
	UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	ID3DBlob*compilationMsgs = nullptr;

	try
	{
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"LightingPS.hlsl").c_str(), nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, &compilationMsgs));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer()));
		compilationMsgs->Release();
	}

	try
	{
		ThrowIfFailed(D3DCompileFromFile(GetAssetFullPath(L"LightingPS.hlsl").c_str(), nullptr, nullptr, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, &compilationMsgs));
	}
	catch (const std::exception& e)
	{
		OutputDebugStringA(reinterpret_cast<const char*>(compilationMsgs->GetBufferPointer()));
		compilationMsgs->Release();
	}

	Shader* ps = new Shader((UINT8*)pixelShader->GetBufferPointer(), pixelShader->GetBufferSize());
	ps->BindTexture("AlbedoTex", 0, 1);
	ps->BindTexture("NormalTex", 1, 1);
	ps->BindSampler("samplerWrap", 0);
	ps->BindConstantBuffer("LightingParam", 0, sizeof(LightingParam));

	Shader* vs = new Shader((UINT8*)vertexShader->GetBufferPointer(), vertexShader->GetBufferSize());

	CD3DX12_RASTERIZER_DESC rasterizerStateDesc(D3D12_DEFAULT);
	rasterizerStateDesc.CullMode = D3D12_CULL_MODE_NONE;

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};

	const D3D12_INPUT_ELEMENT_DESC StandardVertexDescription[] =
	{
		{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};
	UINT StandardVertexDescriptionNumElements = _countof(StandardVertexDescription);

	psoDesc.InputLayout = { StandardVertexDescription, StandardVertexDescriptionNumElements };
	//psoDesc.pRootSignature = m_rootSignature.Get();
	/*psoDesc.VS = CD3DX12_SHADER_BYTECODE(pVertexShaderData, vertexShaderDataLength);
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(pPixelShaderData, pixelShaderDataLength);*/
	psoDesc.RasterizerState = rasterizerStateDesc;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = FALSE;
	psoDesc.DepthStencilState.StencilEnable = FALSE;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
	//psoDescMesh.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleDesc.Count = 1;

	RS_Lighting = unique_ptr<PipelineStateObject>(new PipelineStateObject);
	RS_Lighting->ps = shared_ptr<Shader>(ps);
	RS_Lighting->vs = shared_ptr<Shader>(vs);
	RS_Lighting->graphicsPSODesc = psoDesc;
	RS_Lighting->Init(false);
}

void dx12_framework::CopyPass()
{
	PIXBeginEvent(dx12_rhi->CommandList.Get(), 0, L"CopyPass");

	

	Texture* backbuffer = framebuffers[dx12_rhi->CurrentFrameIndex].get();

	D3D12_RESOURCE_BARRIER BarrierDesc = {};
	BarrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	BarrierDesc.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	BarrierDesc.Transition.pResource = backbuffer->resource.Get();
	BarrierDesc.Transition.Subresource = 0;
	BarrierDesc.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	BarrierDesc.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	dx12_rhi->CommandList->ResourceBarrier(1, &BarrierDesc);


	
	RS_Copy->ApplyGraphicsRSPSO(dx12_rhi->CommandList.Get());


	RS_Copy->ps->SetSampler("samplerWrap", samplerWrap.get(), dx12_rhi->CommandList.Get());
	RS_Copy->ps->SetTexture("SrcTex", ColorBuffer.get(), dx12_rhi->CommandList.Get(), nullptr);

	RS_Copy->ApplyGlobal(dx12_rhi->CommandList.Get());

	dx12_rhi->CommandList->OMSetRenderTargets(1, &backbuffer->CpuHandleRTV, FALSE, nullptr);


	dx12_rhi->CommandList->RSSetViewports(1, &m_viewport);
	dx12_rhi->CommandList->RSSetScissorRects(1, &m_scissorRect);

	dx12_rhi->CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	//dx12_rhi->CommandList->IASetIndexBuffer(&SquintRoom->Ib->view);
	dx12_rhi->CommandList->IASetVertexBuffers(0, 1, &FullScreenVB->view);

	
	

	dx12_rhi->CommandList->DrawInstanced(4, 1, 0, 0);


	D3D12_RESOURCE_BARRIER BarrierDescPresent = {};
	BarrierDescPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	BarrierDescPresent.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	BarrierDescPresent.Transition.pResource = backbuffer->resource.Get();
	BarrierDescPresent.Transition.Subresource = 0;
	BarrierDescPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	BarrierDescPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	dx12_rhi->CommandList->ResourceBarrier(1, &BarrierDescPresent);

	PIXEndEvent(dx12_rhi->CommandList.Get());


	dx12_rhi->CommandList->Close();
	ID3D12CommandList* ppCommandListsEnd[] = { dx12_rhi->CommandList.Get()
	};
	dx12_rhi->CommandQueue->ExecuteCommandLists(_countof(ppCommandListsEnd), ppCommandListsEnd);
}

void dx12_framework::LightingPass()
{
	PIXBeginEvent(dx12_rhi->CommandList.Get(), 0, L"LightingPass");



	D3D12_RESOURCE_BARRIER BarrierDesc = {};
	BarrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	BarrierDesc.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	BarrierDesc.Transition.pResource = ColorBuffer->resource.Get();
	BarrierDesc.Transition.Subresource = 0;
	BarrierDesc.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	BarrierDesc.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	dx12_rhi->CommandList->ResourceBarrier(1, &BarrierDesc);



	RS_Lighting->ApplyGraphicsRSPSO(dx12_rhi->CommandList.Get());


	RS_Lighting->ps->SetSampler("samplerWrap", samplerWrap.get(), dx12_rhi->CommandList.Get());
	RS_Lighting->ps->SetTexture("AlbedoTex", AlbedoBuffer.get(), dx12_rhi->CommandList.Get(), nullptr);
	RS_Lighting->ps->SetTexture("NormalTex", NormalBuffer.get(), dx12_rhi->CommandList.Get(), nullptr);

	LightingParam Param;
	//Param.LightDir = glm::vec4(1.0f, 1.0f, -1.0f, 0.0f);
	Param.LightDir = glm::vec4(LightDir, 0);

	glm::normalize(Param.LightDir);
	RS_Lighting->ps->SetConstantValue("LightingParam", &Param, 0, dx12_rhi->CommandList.Get(), nullptr);


	RS_Lighting->ApplyGlobal(dx12_rhi->CommandList.Get());

	dx12_rhi->CommandList->OMSetRenderTargets(1, &ColorBuffer->CpuHandleRTV, FALSE, nullptr);


	dx12_rhi->CommandList->RSSetViewports(1, &m_viewport);
	dx12_rhi->CommandList->RSSetScissorRects(1, &m_scissorRect);

	dx12_rhi->CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	//dx12_rhi->CommandList->IASetIndexBuffer(&SquintRoom->Ib->view);
	dx12_rhi->CommandList->IASetVertexBuffers(0, 1, &FullScreenVB->view);




	dx12_rhi->CommandList->DrawInstanced(4, 1, 0, 0);


	D3D12_RESOURCE_BARRIER BarrierDescPresent = {};
	BarrierDescPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	BarrierDescPresent.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	BarrierDescPresent.Transition.pResource = ColorBuffer->resource.Get();
	BarrierDescPresent.Transition.Subresource = 0;
	BarrierDescPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	BarrierDescPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	dx12_rhi->CommandList->ResourceBarrier(1, &BarrierDescPresent);

	PIXEndEvent(dx12_rhi->CommandList.Get());


	/*dx12_rhi->CommandList->Close();
	ID3D12CommandList* ppCommandListsEnd[] = { dx12_rhi->CommandList.Get()
	};
	dx12_rhi->CommandQueue->ExecuteCommandLists(_countof(ppCommandListsEnd), ppCommandListsEnd);*/
}

// Update frame-based values.
void dx12_framework::OnUpdate()
{
	m_timer.Tick(NULL);

	if (m_frameCounter == 100)
	{
		// Update window text with FPS value.
		wchar_t fps[64];
		swprintf_s(fps, L"%ufps", m_timer.GetFramesPerSecond());
		SetCustomWindowText(fps);
		m_frameCounter = 0;
	}

	m_frameCounter++;

	m_camera.Update(static_cast<float>(m_timer.GetElapsedSeconds()));





	// Compute the model-view-projection matrix.
	XMStoreFloat4x4(&mvp, XMMatrixTranspose(m_camera.GetViewMatrix() * m_camera.GetProjectionMatrix(0.8f, m_aspectRatio)));


	ViewParameter->SetValue(&mvp);

	XMStoreFloat4x4(&RTViewParam.ViewMatrix, XMMatrixTranspose(m_camera.GetViewMatrix()));
	XMVECTOR Det;
	XMStoreFloat4x4(&RTViewParam.InvViewMatrix, XMMatrixInverse(&Det, XMMatrixTranspose(m_camera.GetViewMatrix())));

}

// Render the scene.
void dx12_framework::OnRender()
{
	dx12_rhi->BeginFrame();
	
	// Record all the commands we need to render the scene into the command list.
	Texture* backbuffer = framebuffers[dx12_rhi->CurrentFrameIndex].get();

	DrawMeshPass();

	LightingPass();

	RaytracePass();

	CopyPass();

	dx12_rhi->EndFrame();
}

void dx12_framework::OnDestroy()
{
	// Ensure that the GPU is no longer referencing resources that are about to be
	// cleaned up by the destructor.
	

	
}

void dx12_framework::OnKeyDown(UINT8 key)
{
	switch (key)
	{
	case 'M':
		bMultiThreadRendering = !bMultiThreadRendering;
		break;
	default:
		break;
	}

	m_camera.OnKeyDown(key);
}

void dx12_framework::OnKeyUp(UINT8 key)
{
	m_camera.OnKeyUp(key);
}

struct ParallelDrawTaskSet : enki::ITaskSet
{
	dx12_framework* app;
	UINT StartIndex;
	UINT ThisDraw;
	UINT ThreadIndex;
	ThreadDescriptorHeapPool* DHPool;

	ParallelDrawTaskSet(){}
	ParallelDrawTaskSet(ParallelDrawTaskSet &&) {}
	ParallelDrawTaskSet(const ParallelDrawTaskSet&) = delete;

	virtual void ExecuteRange(enki::TaskSetPartition range, uint32_t threadnum)
	{
		app->RecordDraw(StartIndex, ThisDraw, ThreadIndex, const_cast<ThreadDescriptorHeapPool*>(DHPool));
	}
};

void dx12_framework::DrawMeshPass()
{
	//Texture* backbuffer = ColorBuffer.get();



	// upload frame texture descriptor to shader visible heap.
	for (int i = 0; i < SquintRoom->Draws.size(); i++)
	{
		Mesh::DrawCall& drawcall = SquintRoom->Draws[i];
		Texture* diffuseTex = SquintRoom->Textures[drawcall.DiffuseTextureIndex].get();
		diffuseTex->VisibleThisFrame();
		Texture* normalTex = SquintRoom->Textures[drawcall.NormalTextureIndex].get();
		normalTex->VisibleThisFrame();
	}


	/*D3D12_RESOURCE_BARRIER BarrierDesc = {};
	BarrierDesc.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	BarrierDesc.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	BarrierDesc.Transition.pResource = backbuffer->resource.Get();
	BarrierDesc.Transition.Subresource = 0;
	BarrierDesc.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	BarrierDesc.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
	dx12_rhi->CommandList->ResourceBarrier(1, &BarrierDesc);*/


	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(AlbedoBuffer->resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));
	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(NormalBuffer->resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET));


	const D3D12_CPU_DESCRIPTOR_HANDLE Rendertargets[] = { AlbedoBuffer->CpuHandleRTV, NormalBuffer->CpuHandleRTV };
	dx12_rhi->CommandList->OMSetRenderTargets(2, Rendertargets, FALSE, &dx12_rhi->depthTexture->CpuHandleDSV);


	//dx12_rhi->CommandList->OMSetRenderTargets(1, &backbuffer->CpuHandleRTV, FALSE, &dx12_rhi->depthTexture->CpuHandleDSV);

	const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
	dx12_rhi->CommandList->ClearRenderTargetView(AlbedoBuffer->CpuHandleRTV, clearColor, 0, nullptr);
	dx12_rhi->CommandList->ClearRenderTargetView(NormalBuffer->CpuHandleRTV, clearColor, 0, nullptr);
	dx12_rhi->CommandList->ClearDepthStencilView(dx12_rhi->depthTexture->CpuHandleDSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

	
	dx12_rhi->CommandList->Close();
	ID3D12CommandList* ppCommandLists[] = { dx12_rhi->CommandList.Get() };
	dx12_rhi->CommandQueue->ExecuteCommandLists(1, ppCommandLists);
	dx12_rhi->CommandList->Reset(dx12_rhi->GetCurrentCA(), nullptr);

	ID3D12DescriptorHeap* ppHeaps[] = { dx12_rhi->SRVCBVDescriptorHeapShaderVisible->DH.Get(), dx12_rhi->SamplerDescriptorHeap->DH.Get() };
	dx12_rhi->CommandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

	dx12_rhi->UploadeFrameTexture2ShaderVisibleHeap();

	PIXBeginEvent(dx12_rhi->CommandList.Get(), 0, L"Draw Mesh");
	if (!bMultiThreadRendering)
	{
		const D3D12_CPU_DESCRIPTOR_HANDLE Rendertargets[] = { AlbedoBuffer->CpuHandleRTV, NormalBuffer->CpuHandleRTV };
		dx12_rhi->CommandList->OMSetRenderTargets(2, Rendertargets, FALSE, &dx12_rhi->depthTexture->CpuHandleDSV);

		RS_Mesh->ApplyGraphicsRSPSO(dx12_rhi->CommandList.Get());
		RS_Mesh->ps->SetSampler("samplerWrap", samplerWrap.get(), dx12_rhi->CommandList.Get());
		RS_Mesh->vs->SetGlobalConstantBuffer("ViewParameter", ViewParameter.get(), dx12_rhi->CommandList.Get(), nullptr);;

		dx12_rhi->CommandList->RSSetViewports(1, &m_viewport);
		dx12_rhi->CommandList->RSSetScissorRects(1, &m_scissorRect);
		dx12_rhi->CommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		dx12_rhi->CommandList->IASetIndexBuffer(&SquintRoom->Ib->view);
		dx12_rhi->CommandList->IASetVertexBuffers(0, 1, &SquintRoom->Vb->view);

		for (int i = 0; i < SquintRoom->Draws.size(); i++)
		{

			Mesh::DrawCall& drawcall = SquintRoom->Draws[i];
			ObjConstantBuffer objCB;
			XMStoreFloat4x4(&objCB.WorldMatrix, XMMatrixRotationY(i * 0));
			RS_Mesh->vs->SetConstantValue("ObjParameter", (void*)&objCB.WorldMatrix, i, dx12_rhi->CommandList.Get(), nullptr);

			Texture* diffuseTex = SquintRoom->Textures[drawcall.DiffuseTextureIndex].get();
			RS_Mesh->ps->SetTexture("diffuseMap", diffuseTex, dx12_rhi->CommandList.Get(), nullptr);

			Texture* normalTex = SquintRoom->Textures[drawcall.NormalTextureIndex].get();
			RS_Mesh->ps->SetTexture("normalMap", normalTex, dx12_rhi->CommandList.Get(), nullptr);

			dx12_rhi->CommandList->DrawIndexedInstanced(drawcall.IndexCount, 1, drawcall.IndexStart, drawcall.VertexBase, 0);
		}
	}
	else
	{
		UINT NumThread = dx12_rhi->NumDrawMeshCommandList;
		UINT RemainDraw = SquintRoom->Draws.size();
		UINT NumDrawThread = SquintRoom->Draws.size() / (NumThread);
		UINT StartIndex = 0;

		vector<ThreadDescriptorHeapPool> vecDHPool;
		vecDHPool.resize(NumThread);

		vector<ParallelDrawTaskSet> vecTask;
		vecTask.resize(NumThread);

		for (int i = 0; i < NumThread; i++)
		{
			UINT ThisDraw = NumDrawThread;
			
			if (i == NumThread - 1)
				ThisDraw = RemainDraw;

			ThreadDescriptorHeapPool& DHPool = vecDHPool[i];
			DHPool.AllocPool(RS_Mesh->GetGraphicsBindingDHSize()*ThisDraw);

			// draw
			ParallelDrawTaskSet& task = vecTask[i];
			task.app = this;
			task.StartIndex = StartIndex;
			task.ThisDraw = ThisDraw;
			task.ThreadIndex = i;
			task.DHPool = &DHPool;

			g_TS.AddTaskSetToPipe(&task);

			RemainDraw -= ThisDraw;
			StartIndex += ThisDraw;
		}

		g_TS.WaitforAll();


		UINT NumCL = dx12_rhi->NumDrawMeshCommandList;
		vector< ID3D12CommandList*> vecCL;

		for (int i = 0; i < NumCL; i++)
			vecCL.push_back(dx12_rhi->DrawMeshCommandList[i].Get());

		dx12_rhi->CommandQueue->ExecuteCommandLists(vecCL.size(), &vecCL[0]);
	}

	//D3D12_RESOURCE_BARRIER BarrierDescPresent = {};
	//BarrierDescPresent.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	//BarrierDescPresent.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	//BarrierDescPresent.Transition.pResource = backbuffer->resource.Get();
	//BarrierDescPresent.Transition.Subresource = 0;
	//BarrierDescPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	//BarrierDescPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	//dx12_rhi->CommandList->ResourceBarrier(1, &BarrierDescPresent);
	
	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(AlbedoBuffer->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
	dx12_rhi->CommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(NormalBuffer->resource.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));


	PIXEndEvent(dx12_rhi->CommandList.Get());
}

void dx12_framework::RecordDraw (UINT StartIndex, UINT NumDraw, UINT CLIndex, ThreadDescriptorHeapPool* DHPool)
{
	Texture* backbuffer = ColorBuffer.get();

	ID3D12GraphicsCommandList* CL = dx12_rhi->DrawMeshCommandList[CLIndex].Get();

	ID3D12CommandAllocator* CA = dx12_rhi->FrameResourceVec[dx12_rhi->CurrentFrameIndex].VecCommandAllocatorMeshDraw[CLIndex].Get();
	CL->Reset(CA, nullptr);

	CL->OMSetRenderTargets(1, &backbuffer->CpuHandleRTV, FALSE, &dx12_rhi->depthTexture->CpuHandleDSV);
	CL->RSSetViewports(1, &m_viewport);
	CL->RSSetScissorRects(1, &m_scissorRect);
	CL->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	CL->IASetIndexBuffer(&SquintRoom->Ib->view);
	CL->IASetVertexBuffers(0, 1, &SquintRoom->Vb->view);

	ID3D12DescriptorHeap* ppHeaps[] = { dx12_rhi->SRVCBVDescriptorHeapShaderVisible->DH.Get(), dx12_rhi->SamplerDescriptorHeap->DH.Get() };
	CL->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
	
	RS_Mesh->ApplyGraphicsRSPSO(CL);
	RS_Mesh->ps->SetSampler("samplerWrap", samplerWrap.get(), CL);
	RS_Mesh->vs->SetGlobalConstantBuffer("ViewParameter", ViewParameter.get(), CL, DHPool);;

	for (int i = StartIndex; i < StartIndex + NumDraw; i++)
	{
		Mesh::DrawCall& drawcall = SquintRoom->Draws[i];
		ObjConstantBuffer objCB;
		XMStoreFloat4x4(&objCB.WorldMatrix, XMMatrixRotationY(i * 0));
		RS_Mesh->vs->SetConstantValue("ObjParameter", (void*)&objCB.WorldMatrix, i, CL, DHPool);


		Texture* diffuseTex = SquintRoom->Textures[drawcall.DiffuseTextureIndex].get();
		RS_Mesh->ps->SetTexture("diffuseMap", diffuseTex, CL, DHPool);

		Texture* normalTex = SquintRoom->Textures[drawcall.NormalTextureIndex].get();
		RS_Mesh->ps->SetTexture("normalMap", normalTex, CL, DHPool);


		CL->DrawIndexedInstanced(drawcall.IndexCount, 1, drawcall.IndexStart, drawcall.VertexBase, 0);
	}
	
	CL->Close();
}



void dx12_framework::ComputePass()
{
	PIXBeginEvent(dx12_rhi->CommandList.Get(), 0, L"Compute");

	RS_Compute->cs->SetUAV("output", ComputeOuputTexture.get());
	//RS_Compute->cs->SetTexture("input", ColorBuffer.get());

	RS_Compute->ApplyCS(dx12_rhi->CommandList.Get());

	dx12_rhi->CommandList->Dispatch(m_width / 32, m_height / 32, 1);
	PIXEndEvent(dx12_rhi->CommandList.Get());
}


void dx12_framework::InitRaytracing()
{
	// init raytracing
	BLAS = SquintRoom->CreateBLASArray();
	vector<shared_ptr<RTAS>> vecBLAS = { BLAS };
	uint64_t tlasSize;
	TLAS = dx12_rhi->CreateTLAS(vecBLAS, tlasSize);

	dx12_rhi->CommandList->Close();
	ID3D12CommandList* ppCommandLists[] = { dx12_rhi->CommandList.Get() };
	dx12_rhi->CommandQueue->ExecuteCommandLists(1, ppCommandLists);
	//dx12_rhi->CommandList->Reset(dx12_rhi->CommandAllocator.Get(), nullptr);
	//dx12_rhi->CommandList->Reset(dx12_rhi->GetCurrentCA(), nullptr);


	dx12_rhi->WaitGPU();



	// create acceleration structure srv (not shader-visible yet)
	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.RaytracingAccelerationStructure.Location = TLAS->Result->GetGPUVirtualAddress();

	// copydescriptor needed when being used.
	dx12_rhi->SRVCBVDescriptorHeapStorage->AllocDescriptor(RTASCPUHandle, RTASGPUHandle);
	dx12_rhi->Device->CreateShaderResourceView(nullptr, &srvDesc, RTASCPUHandle);

	

	// create rtpso
	PSO_RT = unique_ptr<RTPipelineStateObject>(new RTPipelineStateObject);
	//PSO_RT->BindUAVRaygen("gOutput");
	//PSO_RT->BindSRVRaygen("gRtScene");
	//PSO_RT->BindCBVRaygen("ViewParameter", sizeof(RTViewParamCB));



	//PSO_RT->kRayGenShader = L"rayGen";
	//PSO_RT->kMissShader = L"miss";
	//PSO_RT->kClosestHitShader = L"chs";
	//PSO_RT->kHitGroup = L"HitGroup";

	PSO_RT->NumInstance = 1; // important for cbv allocation & shadertable size.

	// new interface
	PSO_RT->AddHitGroup("HitGroup", "chs", "");
	PSO_RT->AddShader("rayGen", RTPipelineStateObject::RAYGEN);
	PSO_RT->BindUAV("rayGen", "gOutput");
	PSO_RT->BindSRV("rayGen", "gRtScene");
	PSO_RT->BindCBV("rayGen", "ViewParameter", sizeof(RTViewParamCB), 1);

	PSO_RT->AddShader("miss", RTPipelineStateObject::MISS);
	PSO_RT->AddShader("chs", RTPipelineStateObject::HIT);



	PSO_RT->MaxRecursion = 1;
	PSO_RT->MaxAttributeSizeInBytes = sizeof(float) * 2;
	PSO_RT->MaxPayloadSizeInBytes = sizeof(float) * 4;

	// PSO_RT->BindSRV("chs", "diffuse");

	//PSO_RT->Init("07-Shaders.hlsl");
	PSO_RT->InitRS("RT_Shadow.hlsl");




	

	


}


void dx12_framework::RaytracePass()
{
	
	{
		PSO_RT->BeginShaderTable();

		PSO_RT->SetUAV("rayGen", "gOutput", ShadowBuffer->CpuHandleUAV);
		PSO_RT->SetSRV("rayGen", "gRtScene", RTASCPUHandle);
		PSO_RT->SetCBVValue("rayGen", "ViewParameter", &RTViewParam, sizeof(RTViewParamCB));
		PSO_RT->SetHitProgram("chs", 0); // this pass use only 1 hit program
		PSO_RT->EndShaderTable();

		PSO_RT->Apply(m_width, m_height);
	}
}
