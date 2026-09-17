/*****************************************************************************************************************
* Copyright (c) 2012 Khalid Ali Al-Kooheji                                                                       *
*                                                                                                                *
* Permission is hereby granted, free of charge, to any person obtaining a copy of this software and              *
* associated documentation files (the "Software"), to deal in the Software without restriction, including        *
* without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell        *
* copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the       *
* following conditions:                                                                                          *
*                                                                                                                *
* The above copyright notice and this permission notice shall be included in all copies or substantial           *
* portions of the Software.                                                                                      *
*                                                                                                                *
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT          *
* LIMITED TO THE WARRANTIES OF MERCHANTABILITY, * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.          *
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, * DAMAGES OR OTHER LIABILITY,      *
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE            *
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                                                         *
*****************************************************************************************************************/
#pragma once

namespace reccore {

class Emitter	{
 public:
	Emitter();
	~Emitter();

	CodeBlock*	create_block(SIZE_T __in pSize);
	void	destroy_block(CodeBlock* __in pBlock);
	void	execute_block(CodeBlock* __in pBlock);

  inline SIZE_T	GetCursor() {
	  return block_->cursor;
  }

  inline CodeBlock*	block() {
	  return block_;
  }

  inline  void set_block(CodeBlock* __in pBlock) {
	  block_	= pBlock;
  }

	inline void emit8(uint8_t byte)	{
		*(block_->ptr8bit+block_->cursor++) = byte;
    #if defined(_DEBUG) && defined(_LOGEMIT)
    wchar_t str[25];
    swprintf(str,25,L"%02x",byte);
    OutputDebugString(str);
    #endif
	}

	inline void emit16(uint16_t bytes) {
		emit8(static_cast<uint8_t>(bytes&0xff));
		emit8(static_cast<uint8_t>((bytes&0xff00)>>8));
	}

	inline void emit32(uint32_t bytes) {
		emit16(static_cast<uint16_t>(bytes&0xffff));
		emit16(static_cast<uint16_t>((bytes&0xffff0000)>>16));
	}

	inline void emit64(uint64_t bytes) {
		emit32(static_cast<uint32_t>(bytes&0xffffffff));
		emit32(static_cast<uint32_t>((bytes&0xffffffff00000000)>>32));
	}
 protected:
	CodeBlock*  block_;	
};

}