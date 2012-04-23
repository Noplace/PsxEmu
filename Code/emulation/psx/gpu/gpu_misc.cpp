#include "..\global.h"



void Gpu::CacheInit()
{
 for (int i=0;i<32;i++)
 {
   TexturePage.Tex[i]=NULL;
   TexturePage.Clut[i]=0;
 }
}

void Gpu::CacheDeinit()
{
 for (int i=0;i<32;i++)
 {
  if (TexturePage.Tex[i])
  {
    G->GetTextureManager()->DeleteTexture(TexturePage.Tex[i]);
  }
 }
}


/*
void CreateTexture(int i,int type)
{
  //if (type==2)
  TexturePage.Tex[i] = G->CreateTexture(256,256,0,D3DFMT_A1R5G5B5);
  //else
  //TexturePage.Tex[i] = G->CreateTexture(256,256,0,D3DFMT_P8);
}*/

typedef struct 
{
  unsigned char block_id;   /* block number tag */
  unsigned short data[4];   /*texture pattern data */
} 
CacheEntry;

CacheEntry Entry[256];

#define CEntry(u,v)  (((v&0x3f)<<2) + ((u&0x3f)>>4))

int is_cache_hit_4bit(u_char u, u_char v)
{
  int block_id = ((v>>6)<<2) + (u>>6);
  int entry_id = ((v&0x3f)<<2) + ((u&0x3f)>>4);

  _cprintf("%d %d\n",entry_id,block_id);

  if (Entry[entry_id].block_id == block_id)
    return(1); /* hit in the cache */
  else
    return(0); /* no-hit in the cache */
}


uint32_t A1R5G5B5_to_A8R8G8B8(uint16_t source)
{
 uint8_t r = (source>>10)&0x1f;
 uint8_t g = (source>>5)&0x1f;
 uint8_t b = (source)&0x1f;
 return ARGB0(255,r<<3,g<<3,b<<3);
}

uint16_t A8R8G8B8_to_A1R5G5B5(uint32_t source)
{
 uint8_t r = (uint8_t)((source>>16)&0xff);
 uint8_t g = (uint8_t)((source>>8)&0xff);
 uint8_t b = (uint8_t)((source)&0xff);
 return (0x8000|(((r>>3)<<10)&0x7C00)|(((g>>3)<<5)&0x3E0)|((b>>3)&0x1f));
}





/*void GpuLockVRamForTransfer(int rw)
{
	D3DLOCKED_RECT rect;
//	_cprintf("lock for %d(%d %d %d %d)\n",rw,GpuUnit.TransX0,GpuUnit.TransY0,GpuUnit.TransX1,GpuUnit.TransY1);
	if (!VRamLock)
	{
	  //G->LockTexture(&VRamTex,GpuUnit.TransX0,GpuUnit.TransY0,GpuUnit.TransX1,GpuUnit.TransY1,&rect);
      G->LockTexture(&VRamTex,0,0,1024,512,&rect);
	  psxVRam->u8=(uint8_t*)rect.pBits;
	  psxVRam->u16=(uint16_t*)rect.pBits;
	  psxVRam->u32=(uint32_t*)rect.pBits;
	}
	else
	{
		if (rw!=VRamLock)
		{
	     //_cprintf("%d Lock within %d lock (%d %d %d %d)\n",rw,VRamLock,GpuUnit.TransX0,GpuUnit.TransY0,GpuUnit.TransX1,GpuUnit.TransY1);
	     //return;
		}
	}
	if (rw==1)
	{
	 VRamLock=1;
	}
	if (rw==2)
	{
	 VRamLock=2;
	}
}

void GpuUnlockVRamForTransfer(int rw)
{
	//_cprintf("unlock for %d(%d %d %d %d) %d\n",rw,GpuUnit.TransX0,GpuUnit.TransY0,GpuUnit.TransX1,GpuUnit.TransY1,VRamLock);
	if (VRamLock)
	{
	  G->UnlockTexture(&VRamTex);
	  psxVRam->u8=NULL;
	  psxVRam->u16=NULL;
	  psxVRam->u32=NULL;
	  VRamLock=0;
	}

	//UNSET_BIT(GpuUnit.Status,27);
	 if (rw==1)GpuUnit.ReadMode=GPU_RW_IO;
	 if (rw==2)GpuUnit.WriteMode=GPU_RW_IO;
	 GpuUnit.TransY0=GpuUnit.TransX0=GpuUnit.TransY1=GpuUnit.TransX1=0;
	 //GPUIsIdle;
}*/

BOOL Gpu::WriteVRamToFile( )
{
CTexture *ss;
D3DLOCKED_RECT src_rect;
 D3DLOCKED_RECT dst_rect;
 unsigned short *mem1;
 uint32_t *mem2;
 int x,y;
 return 1;
 #define _pix(rect,mem,x,y) *(mem+x+(rect.Pitch>>2)*y)

 ss=G->GetTextureManager()->CreateTexture(1024,512,D3DFMT_A8R8G8B8);
 
 
 G->LockBackBuffer(0,0,1024,512,&src_rect);


 mem2=(uint32_t *)src_rect.pBits;
 mem1=(unsigned short *)Parent->MC->VRam->u16;
 
  
    
 for ( y=0;y<Height;y++)
  for ( x=0;x<Width;x++)
  {
	  //*mem=i;//
	  mem1[x+y*1024]=A8R8G8B8_to_A1R5G5B5(_pix(src_rect,mem2,x,y));
	  //mem++;
	  
  }

 G->UnlockBackBuffer();
 

 
// G->LockTexture(ss,0,0,1024,512,&dst_rect);
ss->Lock(0,0,1024,512,&dst_rect);
 mem1=(unsigned short *)PS->MC->VRam->u16;
  mem2=(uint32_t *)dst_rect.pBits;
 
   
 for ( y=0;y<512;y++)
  for ( x=0;x<1024;x++)
  {
	  //*mem=i;//
	  _pix(dst_rect,mem2,x,y)=A1R5G5B5_to_A8R8G8B8(mem1[x+y*1024]);
	  //mem++;
	  
  }

 // G->UnlockTexture(ss);
  ss->Unlock();



	char *getExePath(char *exename);
	char *path = getExePath("PsxEmu(Debug)");
	char fn[255];
	sprintf(fn,"%vram.bmp",path);
	delete [] path;
 D3DXSaveTextureToFile(fn,D3DXIFF_BMP,(LPDIRECT3DTEXTURE9)ss->GetResourcePointer(),0 );
 
 G->GetTextureManager()->DeleteTexture(ss);
 return 1;
}





void Gpu::UpdateDisplay()
{
	 {
		 RECT rc;
		 GetClientRect(mhWnd,&rc);
		 if (rc.right!=(signed)Width||rc.bottom!=(signed)Height)
		 {
          SetWindowPos(mhWnd,HWND_TOP,0,0,Width+6,Height+25,SWP_NOMOVE);

         }
		 D3DVIEWPORT9 vp={0,0,1024*1024/Width,512*512/Height,0,1};//DisplayStartX,DisplayStartY,Width&2,Height*2,0,1};
		 //d3dDevice->SetViewport(&vp);

	GV->Width	=	Width;
	GV->Height	=	 Height;
	GV->OffsetX	=	DisplayStartX;
	GV->OffsetY	=	 0;//DisplayStartY;
	
	GV->Build();
	GV->Set();
	 }
}



unsigned short ApplyTransToColor(unsigned short src,uint32_t code)
{
	unsigned short dst=0;//0x8000|src;

	if (!src) 
		dst=0x0000;
	else
		dst=0x8000|src;


/*	if (!src)
		dst=0x8000;

		if (src&0x8000) 
			{
				if (src&0x7fff)
				{
					if (code&0x2)
				  dst = 0x0000|src;
				  else
					dst = 0x8000|src;
				}
				else
				{
					dst = 0;
				}

				_cprintf("%x\n",src);

			}
			else
			{
				if (src&0x7fff)
				{
					dst = 0x8000|src;
				}
				else
					dst = 0x0;
			}
*/

	return dst;
}



uint32_t ApplyTransToColor2(unsigned short src,uint32_t code)
{
	unsigned short dst=0;//0x8000|src;

	if (!src) 
		dst=0x0000;
	else
		dst=0x8000|src;

	return A1R5G5B5_to_A8R8G8B8(dst);
}


/*

*/


CTexture *Gpu::LoadTexture(uint32_t tp,uint32_t clut_id,uint32_t code)
{
	uint32_t TClut;
	D3DLOCKED_RECT dstRect;
	uint16_t pal[256];
    uint16_t *srcMem;
	uint16_t *srcCLUT;
	uint8_t *dstMem;
	//return 0;
	int x,y,i;
	int tx,ty,cx,cy;
	cx = ((clut_id & 0x3f) << 4);
	cy = ((clut_id & 0xffc0) >> 6);

	tx=(tp&0xf)*64;
	ty=((tp>>4)&0x1)*256;

	TClut = (tp>>7)&3;

	//if (((GpuPrimeBuf[0]>>24)&0xff)==0x7d)
//	_cprintf("tx,ty:%d %d %x\n",tx,ty,tp&0x1f);
	//_cprintf("cx,cy:%d %d %x\n",cx>>4,cy-256,tp&0x1f);

	if (TClut>1)
		clut_id = -1;

	if (TexturePage.Clut[tp&0x1f]==clut_id)
	    return TexturePage.Tex[tp&0x1f];

	//if (TexturePage.Clut[tp&0x1f]!=0)
	//	return TexturePage.Tex[tp&0x1f];
	
	if (!TexturePage.Tex[tp&0x1f])
	TexturePage.Tex[tp&0x1f] = G->GetTextureManager()->CreateTexture(256,256,D3DFMT_A1R5G5B5);
      //CreateTexture(tp&0x1f,TClut);

    TexturePage.Clut[tp&0x1f] = clut_id;
    
	if (TClut==1)
	{
	 srcCLUT=(uint16_t*)&PS->MC->VRam->u16[cy*1024+cx];
	 for (i=0;i<256;i++)
	 {
	  pal[i] = BGR2ARGB555(srcCLUT[i]);
	 }
	}

	if (TClut==0)
	{

	 srcCLUT=(uint16_t*)&PS->MC->VRam->u16[cy*1024+cx];
     for (i=0;i<16;i++)
	 {
	  pal[i] = BGR2ARGB555(srcCLUT[i]);

	 }

	}


	srcMem=(uint16_t*)&PS->MC->VRam->u16[tx+ty*1024];

	//G->LockTexture(TexturePage.Tex[tp&0x1f],0,0,256,256,&dstRect);
	TexturePage.Tex[tp&0x1f]->Lock(0,0,256,256,&dstRect);
	dstMem    = (uint8_t*)dstRect.pBits;

	 if (TClut == 2)
	 {
	    for (y=0;y<256;y++)
		{
		 unsigned short *src_line = (unsigned short *)srcMem;
		 unsigned short *dst_line = (unsigned short *)dstMem;
		  for (x=0;x<256;x++)
		  {
		  	*dst_line = ApplyTransToColor(BGR2ARGB555(*src_line),code);
			src_line++;
			dst_line++;
		  }
	   	 srcMem+=1024;
		 dstMem+=dstRect.Pitch;
		}
	 }

     if (TClut == 1)
	 {
	

	    for (y=0;y<256;y++)
		{
		 unsigned short *src_line = (unsigned short *)srcMem;
		 unsigned short *dst_line = (unsigned short *)dstMem;
		  for (x=0;x<128;x++)
		  {
			*dst_line = ApplyTransToColor(pal[(((*src_line) & 0x00ff))],code);
			dst_line++;
			*dst_line = ApplyTransToColor(pal[(((*src_line) & 0xff00)>>8)],code);
			dst_line++;
			src_line++;
		  }
	   	 srcMem+=1024;
		 dstMem+=dstRect.Pitch;
		}
	 }

	 if (TClut == 0)
	 {
	    for (y=0;y<256;y++)
		{
		 unsigned short *src_line = (unsigned short *)srcMem;
		 unsigned short *dst_line = (unsigned short *)dstMem;
		  for (x=0;x<64;x++)
		  {
           unsigned short c = *src_line;
			
			*dst_line = ApplyTransToColor(pal[(c&0xf)],code);
			dst_line++;
			*dst_line = ApplyTransToColor(pal[(c&0xf0)>>4],code);
			dst_line++;
			*dst_line = ApplyTransToColor(pal[(c&0xf00)>>8],code);
			dst_line++;
			*dst_line = ApplyTransToColor(pal[(c&0xf000)>>12],code);
			dst_line++;
			src_line++;
		  }
	   	 srcMem+=1024;
		 dstMem+=dstRect.Pitch;
		}
	 }

	 //G->UnlockTexture(TexturePage.Tex[tp&0x1f]);
	 TexturePage.Tex[tp&0x1f]->Unlock();
	 
     return TexturePage.Tex[tp&0x1f];
}

uint32_t Gpu::SetTransMode(uint32_t tp,uint32_t in_color)
{
 uint8_t code = (uint8_t)(in_color>>24);
 uint32_t rgb = (uint32_t)(in_color&0xffffff);
 uint32_t st_mode = (tp>>5)&3;
 uint32_t out_color=in_color&0xffffff;
 
// _cprintf("trans mode %x %x\n",code&0x2,st_mode);
 
 out_color |= 0xff000000;

 if ((code&0x2))
 {
	 //out_color|=0x80000000;
  if (st_mode==0)
  {
   d3dDevice->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);
   d3dDevice->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
   d3dDevice->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
   
  }

  if (st_mode==1)
  {
   d3dDevice->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);
   d3dDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
   d3dDevice->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
   //primitive->BufTS(D3DTSS_ALPHAOP,D3DTOP_SELECTARG1);
   //primitive->BufTS(D3DTSS_ALPHAARG1,D3DTA_DIFFUSE);
   //d3dDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
  }

  if (st_mode==2)
  {
   d3dDevice->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);
   d3dDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCCOLOR);
   d3dDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
  }

  if (st_mode==3)
  {
   d3dDevice->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);
   d3dDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
   d3dDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
  }
 }
 else
 {
	 out_color |= 0xff000000;
	 //primitive->BufTS(D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
    //
	//uint32_t x;
	//d3dDevice->GetRenderState(D3DRS_SRCBLEND,&x);
	//d3dDevice->GetRenderState(D3DRS_DESTBLEND,&x);
    //d3dDevice->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
    //d3dDevice->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
 }
	// 
 return out_color;
}

