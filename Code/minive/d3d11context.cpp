#include <WinCore/io/io.h>
#include "context.h"
#include "d3d11context.h"


namespace minive {

void* D3D11Context::operator new( size_t stAllocateBlock) {
  return _aligned_malloc(sizeof(D3D11Context),16);
}

void  D3D11Context::operator delete (void* p)  {
  return _aligned_free(p);
}


ID3D11Buffer* vsbuf;

D3D11Context::D3D11Context() : vs(nullptr),ps(nullptr),ps_tex(nullptr),ia(nullptr),matrixBuffer(nullptr) {

}

D3D11Context::~D3D11Context() {
  Deinitialize();
}

int D3D11Context::Initialize(int width, int height, bool vsync, HWND hwnd, bool fullscreen, float depth, float screennear) {
	HRESULT result;
	IDXGIFactory* factory;
	IDXGIAdapter* adapter;
	IDXGIOutput* adapterOutput;
	unsigned int numModes, i, numerator, denominator, stringLength;
	DXGI_MODE_DESC* dispmode_list;
	DXGI_ADAPTER_DESC adaptor_desc;
	int error;
	DXGI_SWAP_CHAIN_DESC swapchain_desc;
	ID3D11Texture2D* backBufferPtr;
	D3D11_TEXTURE2D_DESC depthBufferDesc;
	D3D11_DEPTH_STENCIL_DESC depthStencilDesc;
	D3D11_DEPTH_STENCIL_VIEW_DESC depthStencilViewDesc;
	D3D11_RASTERIZER_DESC rasterDesc;
	D3D11_VIEWPORT viewport;
	float fieldOfView, screenAspect;
	D3D11_DEPTH_STENCIL_DESC depthDisabledStencilDesc;


	// Store the vsync setting.
	vsync_enabled_ = vsync;

	// Create a DirectX graphics interface factory.
	result = CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&factory);
  if (result != S_OK) {
		Deinitialize();
		return S_FALSE;
	}

	// Use the factory to create an adapter for the primary graphics interface (video card).
	result = factory->EnumAdapters(0, &adapter);
	if (result != S_OK) {
		Deinitialize();
		return S_FALSE;
	}

	// Enumerate the primary adapter output (monitor).
	result = adapter->EnumOutputs(0, &adapterOutput);
	if (result != S_OK) {
		Deinitialize();
		return S_FALSE;
	}

	// Get the number of modes that fit the DXGI_FORMAT_R8G8B8A8_UNORM display format for the adapter output (monitor).
	result = adapterOutput->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ENUM_MODES_INTERLACED, &numModes, NULL);
	if(result != S_OK) {
		Deinitialize();
		return S_FALSE;
	}

	// Create a list to hold all the possible display modes for this monitor/video card combination.
	dispmode_list = new DXGI_MODE_DESC[numModes];
	if(!dispmode_list) {
		Deinitialize();
		return S_FALSE;
	}

	// Now fill the display mode list structures.
	result = adapterOutput->GetDisplayModeList(DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ENUM_MODES_INTERLACED, &numModes, dispmode_list);
	if(result != S_OK) {
		Deinitialize();
		return S_FALSE;
	}

	// Now go through all the display modes and find the one that matches the screen width and height.
	// When a match is found store the numerator and denominator of the refresh rate for that monitor.
	for(i=0; i<numModes; i++)
	{
		if(dispmode_list[i].Width == (unsigned int)width)
		{
			if(dispmode_list[i].Height == (unsigned int)height)
			{
				numerator = dispmode_list[i].RefreshRate.Numerator;
				denominator = dispmode_list[i].RefreshRate.Denominator;
			}
		}
	}

	// Get the adapter (video card) description.
	result = adapter->GetDesc(&adaptor_desc);
	if(result != S_OK) {
		Deinitialize();
		return S_FALSE;
	}

  vc_mem_ = adaptor_desc.DedicatedVideoMemory;
  if (vc_mem_ == 0) 
    vc_mem_ = adaptor_desc.DedicatedSystemMemory;
  if (vc_mem_ == 0) 
    vc_mem_ = adaptor_desc.SharedSystemMemory;
  
	// Store the dedicated video card memory in megabytes.
	vc_mem_ = (vc_mem_ >> 20);

	// Convert the name of the video card to a character array and store it.
	error = wcstombs_s(&stringLength, vc_desc_, 128, adaptor_desc.Description, 128);
	if(error != 0) {
		Deinitialize();
		return S_FALSE;
	}

	// Release the display mode list.
	delete [] dispmode_list;
	dispmode_list = 0;

	// Release the adapter output.
  SafeRelease(&adapterOutput);

	// Release the adapter.
  SafeRelease(&adapter);

	// Release the factory.
  SafeRelease(&factory);

	// Initialize the swap chain description.
	ZeroMemory(&swapchain_desc, sizeof(swapchain_desc));
	swapchain_desc.BufferCount = 1;
	swapchain_desc.BufferDesc.Width = width;
	swapchain_desc.BufferDesc.Height = height;
	swapchain_desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	if(vsync_enabled_)	{
		swapchain_desc.BufferDesc.RefreshRate.Numerator = numerator;
		swapchain_desc.BufferDesc.RefreshRate.Denominator = denominator;
	}	else{
		swapchain_desc.BufferDesc.RefreshRate.Numerator = 0;
		swapchain_desc.BufferDesc.RefreshRate.Denominator = 1;
	}
	swapchain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; 
	swapchain_desc.OutputWindow = hwnd;                           
	swapchain_desc.SampleDesc.Count = 1;                          
	swapchain_desc.SampleDesc.Quality = 0;
	if(fullscreen) {	                                            
		swapchain_desc.Windowed = false;
	}	else	{
		swapchain_desc.Windowed = true;
	}
	swapchain_desc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	swapchain_desc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
	swapchain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	swapchain_desc.Flags = 0;


  D3D_FEATURE_LEVEL feature_levels[] = {
    D3D_FEATURE_LEVEL_11_1,
    D3D_FEATURE_LEVEL_11_0,
    D3D_FEATURE_LEVEL_10_1,
    D3D_FEATURE_LEVEL_10_0,
    D3D_FEATURE_LEVEL_9_3,
    D3D_FEATURE_LEVEL_9_2,
    D3D_FEATURE_LEVEL_9_1,
  };
       
  D3D_FEATURE_LEVEL feature_level;
	// Create the swap chain, Direct3D device, and Direct3D device context.
	result = D3D11CreateDeviceAndSwapChain(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,0,
										   feature_levels,ARRAYSIZE(feature_levels),D3D11_SDK_VERSION,&swapchain_desc,
										   &swapchain,&device, &feature_level, &devicecontext);
	if(result != S_OK) {
		Deinitialize();
		return S_FALSE;
	}

	// Get the pointer to the back buffer.
	result = swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&backBufferPtr);
	if(result != S_OK) {
		Deinitialize();
		return S_FALSE;
	}

	// Create the render target view with the back buffer pointer.
	result = device->CreateRenderTargetView(backBufferPtr, NULL, &rendertargetview);
	if(result != S_OK) {
		Deinitialize();
		return S_FALSE;
	}

	// Release pointer to the back buffer as we no longer need it.
	backBufferPtr->Release();
	backBufferPtr = 0;
  SafeRelease(&backBufferPtr);

	// Initialize the description of the depth buffer.
	ZeroMemory(&depthBufferDesc, sizeof(depthBufferDesc));

	// Set up the description of the depth buffer.
	depthBufferDesc.Width = width;
	depthBufferDesc.Height = height;
	depthBufferDesc.MipLevels = 1;
	depthBufferDesc.ArraySize = 1;
	depthBufferDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	depthBufferDesc.SampleDesc.Count = 1;
	depthBufferDesc.SampleDesc.Quality = 0;
	depthBufferDesc.Usage = D3D11_USAGE_DEFAULT;
	depthBufferDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	depthBufferDesc.CPUAccessFlags = 0;
	depthBufferDesc.MiscFlags = 0;

	// Create the texture for the depth buffer using the filled out description.
	result = device->CreateTexture2D(&depthBufferDesc, NULL, &depthstencilbuffer);
	if(result != S_OK) {
		Deinitialize();
		return S_FALSE;
	}

	// Initialize the description of the stencil state.
	ZeroMemory(&depthStencilDesc, sizeof(depthStencilDesc));

	// Set up the description of the stencil state.
	depthStencilDesc.DepthEnable = true;
	depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	depthStencilDesc.DepthFunc = D3D11_COMPARISON_LESS;

	depthStencilDesc.StencilEnable = true;
	depthStencilDesc.StencilReadMask = 0xFF;
	depthStencilDesc.StencilWriteMask = 0xFF;

	// Stencil operations if pixel is front-facing.
	depthStencilDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
	depthStencilDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_INCR;
	depthStencilDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
	depthStencilDesc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

	// Stencil operations if pixel is back-facing.
	depthStencilDesc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
	depthStencilDesc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_DECR;
	depthStencilDesc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
	depthStencilDesc.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

	// Create the depth stencil state.
	result = device->CreateDepthStencilState(&depthStencilDesc, &depthstencilstate);
	if(result != S_OK) {
		Deinitialize();
		return S_FALSE;
	}

	// Set the depth stencil state.
	devicecontext->OMSetDepthStencilState(depthstencilstate, 1);

	// Initialize the depth stencil view.
	ZeroMemory(&depthStencilViewDesc, sizeof(depthStencilViewDesc));

	// Set up the depth stencil view description.
	depthStencilViewDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	depthStencilViewDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
	depthStencilViewDesc.Texture2D.MipSlice = 0;

	// Create the depth stencil view.
	result = device->CreateDepthStencilView(depthstencilbuffer, &depthStencilViewDesc, &depthstencilview);
	if(result != S_OK) {
		Deinitialize();
		return S_FALSE;
	}
	// Bind the render target view and depth stencil buffer to the output render pipeline.
	devicecontext->OMSetRenderTargets(1, &rendertargetview, depthstencilview);

	// Setup the raster description which will determine how and what polygons will be drawn.
	rasterDesc.AntialiasedLineEnable = false;
	rasterDesc.CullMode = D3D11_CULL_BACK;
	rasterDesc.DepthBias = 0;
	rasterDesc.DepthBiasClamp = 0.0f;
	rasterDesc.DepthClipEnable = true;
	rasterDesc.FillMode = D3D11_FILL_SOLID;
	rasterDesc.FrontCounterClockwise = false;
	rasterDesc.MultisampleEnable = false;
	rasterDesc.ScissorEnable = false;
	rasterDesc.SlopeScaledDepthBias = 0.0f;

	// Create the rasterizer state from the description we just filled out.
	result = device->CreateRasterizerState(&rasterDesc, &rasterstate);
	if(result != S_OK) {
		Deinitialize();
		return S_FALSE;
	}

	// Now set the rasterizer state.
	devicecontext->RSSetState(rasterstate);
	
	// Setup the viewport for rendering.
	viewport.Width = (float)width;
	viewport.Height = (float)height;
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	viewport.TopLeftX = 0.0f;
	viewport.TopLeftY = 0.0f;

	// Create the viewport.
	devicecontext->RSSetViewports(1, &viewport);

	// Setup the projection matrix.
	fieldOfView = (float)XM_PI / 4.0f;
	screenAspect = (float)width / (float)height;

	// Create the projection matrix for 3D rendering.
	//projectionmatrix = XMMatrixPerspectiveFovLH(fieldOfView, screenAspect, screennear, depth);
	// Initialize the world matrix to the identity matrix.
	//worldmatrix = XMMatrixIdentity();

	// Clear the second depth stencil state before setting the parameters.
	ZeroMemory(&depthDisabledStencilDesc, sizeof(depthDisabledStencilDesc));

	// Now create a second depth stencil state which turns off the Z buffer for 2D rendering.  The only difference is 
	// that DepthEnable is set to false, all other parameters are the same as the other depth stencil state.
	depthDisabledStencilDesc.DepthEnable = false;
	depthDisabledStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	depthDisabledStencilDesc.DepthFunc = D3D11_COMPARISON_LESS;
	depthDisabledStencilDesc.StencilEnable = true;
	depthDisabledStencilDesc.StencilReadMask = 0xFF;
	depthDisabledStencilDesc.StencilWriteMask = 0xFF;
	depthDisabledStencilDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
	depthDisabledStencilDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_INCR;
	depthDisabledStencilDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
	depthDisabledStencilDesc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
	depthDisabledStencilDesc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
	depthDisabledStencilDesc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_DECR;
	depthDisabledStencilDesc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
	depthDisabledStencilDesc.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

	// Create the state using the device.
	result = device->CreateDepthStencilState(&depthDisabledStencilDesc, &depthdisabledstencilstate);
	if(result != S_OK) {
		Deinitialize();
		return S_FALSE;
	}

  result = CreateShaders();
	if(result != S_OK) {
		Deinitialize();
		return S_FALSE;
	}

  matrixBufferData.world = XMMatrixTranspose(XMMatrixIdentity());
  matrixBufferData.view = XMMatrixTranspose(XMMatrixIdentity());
  matrixBufferData.projection = XMMatrixTranspose(XMMatrixOrthographicOffCenterLH(0.0f,(FLOAT)width,(FLOAT)height,0.0f,-10000.0f,10000.0f));


  D3D11_BUFFER_DESC bd;
  ZeroMemory( &bd, sizeof(bd) );
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.ByteWidth = sizeof(matrixBufferData);
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bd.CPUAccessFlags = 0;
  D3D11_SUBRESOURCE_DATA InitData;
  ZeroMemory(&InitData,sizeof(InitData));
  InitData.pSysMem = &matrixBufferData;
  result = device->CreateBuffer( &bd, &InitData, &matrixBuffer );
  devicecontext->VSSetConstantBuffers(0,1,&matrixBuffer);

  devicecontext->VSSetShader(vs,0,0);
  devicecontext->PSSetShader(ps,0,0);
  devicecontext->IASetInputLayout(ia);

  {
    Vertex data[4];
    ZeroMemory(&data,sizeof(data));
    data[0].x = 0;    data[0].y = 0;    data[0].z = 0;    data[0].color = XMVectorSet(1.0f,1.0f,1.0f,1.0f);
    data[1].x = 200;   data[1].y = 0;    data[1].z = 0;    data[1].color = XMVectorSet(1.0f,1.0f,1.0f,1.0f);
    data[2].x = 0;    data[2].y = 200;   data[2].z = 0;    data[2].color = XMVectorSet(1.0f,1.0f,1.0f,1.0f);
    data[3].x = 200;   data[3].y = 200;   data[3].z = 0;    data[3].color = XMVectorSet(1.0f,1.0f,1.0f,1.0f);

    ZeroMemory( &bd, sizeof(bd) );
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.ByteWidth = sizeof(Vertex)*4;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = 0;
    D3D11_SUBRESOURCE_DATA InitData;
    ZeroMemory(&InitData,sizeof(InitData));
    InitData.pSysMem = &data;
    result = device->CreateBuffer( &bd,  &InitData, &vsbuf );
    devicecontext->VSSetConstantBuffers(0,1,&vsbuf);
  }
  devicecontext->OMSetDepthStencilState(depthdisabledstencilstate, 1);
	return S_OK;
}


int D3D11Context::Deinitialize() {

	if(swapchain)
	{
		swapchain->SetFullscreenState(false, NULL);
	}
  SafeRelease(&matrixBuffer);
  SafeRelease(&ps_tex);
  SafeRelease(&ps);
  SafeRelease(&ia);
  SafeRelease(&vs);

	SafeRelease(&depthdisabledstencilstate);
	SafeRelease(&rasterstate);
	SafeRelease(&depthstencilview);
	SafeRelease(&depthstencilstate);
	SafeRelease(&depthstencilbuffer);

	SafeRelease(&rendertargetview);
	SafeRelease(&devicecontext);
	SafeRelease(&device);
	SafeRelease(&swapchain);


	return S_OK;
}


int D3D11Context::Clear() {
	float color[4] = {0,0,0,1};
	devicecontext->ClearRenderTargetView(rendertargetview, color);
	devicecontext->ClearDepthStencilView(depthstencilview, D3D11_CLEAR_DEPTH, 1.0f, 0);
  return S_OK;
}



int D3D11Context::Present() {

  devicecontext->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
  UINT stride = sizeof(Vertex);
  UINT offset = 0;
  devicecontext->IASetVertexBuffers(0,1,&vsbuf,&stride,&offset);
  devicecontext->Draw(4,0);

  // Present the back buffer to the screen since rendering is complete.
	if(vsync_enabled_)	{
		// Lock to screen refresh rate.
		swapchain->Present(1, 0);
	}	else	{
		// Present as fast as possible.
		swapchain->Present(0, 0);
	}
  return S_OK;
}



int D3D11Context::CreateShaders() {
  HRESULT result;
	//D3D11_INPUT_ELEMENT_DESC polygonLayout[2];
	//unsigned int numElements;
	//D3D11_BUFFER_DESC matrixBufferDesc;
	//D3D11_SAMPLER_DESC samplerDesc;
  
  uint8_t* data=nullptr;
  size_t length=0;

  core::io::ReadWholeFileBinary("D:\\Personal\\Projects\\PsxEmu\\Output\\Win32\\Debug\\vs.cso",&data,length);
	result = device->CreateVertexShader(data,length, NULL, &vs);
	if(result != S_OK)
	{
		return S_FALSE;
	}

  
  D3D11_INPUT_ELEMENT_DESC polygonLayout[3] = {
    {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT    ,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
    {"COLOR"   ,0,DXGI_FORMAT_R32G32B32A32_FLOAT ,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
    {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT       ,0,28,D3D11_INPUT_PER_VERTEX_DATA,0}
  };
  

	// Get a count of the elements in the layout.
	auto numElements = sizeof(polygonLayout) / sizeof(polygonLayout[0]);
  
  // Create the vertex input layout.
	result = device->CreateInputLayout(polygonLayout, ARRAYSIZE(polygonLayout), data,length,  &ia);
	if(result != S_OK)
	{
		return S_FALSE;
	}
  core::io::DestroyFileBuffer(&data);




	core::io::ReadWholeFileBinary("D:\\Personal\\Projects\\PsxEmu\\Output\\Win32\\Debug\\ps.cso",&data,length);
	result = device->CreatePixelShader(data,length, NULL, &ps);
	core::io::DestroyFileBuffer(&data);
  if(result != S_OK)
	{
		return S_FALSE;
	}

  core::io::ReadWholeFileBinary("D:\\Personal\\Projects\\PsxEmu\\Output\\Win32\\Debug\\ps_tex.cso",&data,length);
  result = device->CreatePixelShader(data,length, NULL, &ps_tex);
	core::io::DestroyFileBuffer(&data);
  if(result != S_OK)
	{
		return S_FALSE;
	}

  return S_OK;
}




}