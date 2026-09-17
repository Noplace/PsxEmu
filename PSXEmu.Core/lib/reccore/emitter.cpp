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
#include "reccore.h"

namespace reccore {

Emitter::Emitter() {
	block_	=	NULL;
}

Emitter::~Emitter() {

}

CodeBlock*	Emitter::create_block(SIZE_T __in pSize) {
	CodeBlock* block = new CodeBlock();
	block->size      = pSize;
	block->cursor	   = 0;
	block->address 	 = VirtualAlloc(0,block->size,MEM_COMMIT,PAGE_EXECUTE_READWRITE);
	block->ptr8bit   = static_cast<uint8_t*>(block->address);
	return block;
}

void	Emitter::destroy_block(CodeBlock* __in pBlock) {
#ifdef _DEBUG
  assert(pBlock != nullptr);
#endif
	VirtualFree(pBlock->address,pBlock->size,MEM_DECOMMIT|MEM_RELEASE);
	//VirtualFree(pBlock->address,pBlock->size,MEM_RELEASE);
	delete pBlock;
}

void	Emitter::execute_block(CodeBlock* __in pBlock) {
	void (*CallableAddress)();
	CallableAddress	= static_cast<void (*)()>(pBlock->address);
	CallableAddress();
}

}