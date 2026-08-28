#include "context.h"
#include "d2d1context.h"

namespace minive {

D2D1Context::D2D1Context() {
  m_pRenderTarget = nullptr;
  m_pDirect2dFactory = nullptr;

}

D2D1Context::~D2D1Context() {

}

int D2D1Context::Initialize(int width, int height, bool vsync, HWND hwnd, bool fullscreen, float depth, float screennear) {


  HRESULT hr = S_OK;
   // Create a Direct2D factory.
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_pDirect2dFactory);

    if (!m_pRenderTarget)
    {
        RECT rc;
        GetClientRect(hwnd, &rc);

        D2D1_SIZE_U size = D2D1::SizeU(
            rc.right - rc.left,
            rc.bottom - rc.top
            );

        // Create a Direct2D render target.
        hr = m_pDirect2dFactory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(hwnd, size),
            &m_pRenderTarget
            );


       
    }

    return hr;

}

int D2D1Context::Deinitialize() {
  SafeRelease(&m_pDirect2dFactory);
  SafeRelease(&m_pRenderTarget);
	return S_OK;
}


int D2D1Context::Clear() {
  m_pRenderTarget->BeginDraw();
  m_pRenderTarget->Clear(D2D1::ColorF(D2D1::ColorF::Black));

  static ID2D1SolidColorBrush* m_pCornflowerBlueBrush = nullptr;
  if (m_pCornflowerBlueBrush == nullptr) {
   m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::CornflowerBlue),&m_pCornflowerBlueBrush);
  }
m_pRenderTarget->FillRectangle(D2D1::RectF(0,0,200,200),m_pCornflowerBlueBrush);

  return S_OK;
}

int D2D1Context::Present() {
  return m_pRenderTarget->EndDraw();
}

}