namespace ia_emitter {
namespace intel {

class IS {
 public:
	IS(Emitter* pEmitter) : e(pEmitter) {ExpectBuffer[0]=0;ExpectBuffer[1]=0;}
	Emitter&	GetEmitter(){return *e;}
	void	SetOperandSize(OMode pMode,bool SignExtendFor8Bit){_w=pMode;_s=SignExtendFor8Bit;}
	IAMode	GetMode(){return m;}

	void	Prefix(uint8_t prefix)	{
		__check();
		e->emit8(prefix);
	}

	void	Immediate8(uint8_t imm)	{
		_ASSERTE(ExpectBuffer[0]==0);
		_ASSERTE((ExpectBuffer[1]&8)==8);
		e->emit8(imm);
		ExpectBuffer[1]=0;
	}

	void	Immediate16(uint16_t imm)	{
		_ASSERTE(ExpectBuffer[0]==0);
		_ASSERTE((ExpectBuffer[1]&16)==16);
		e->emit16(imm);
		ExpectBuffer[1]=0;
	}

	void	Immediate32(uint32_t imm)	{		
    _ASSERTE(ExpectBuffer[0]==0);
		_ASSERTE((ExpectBuffer[1]&32)==32);
		e->emit32(imm);
		ExpectBuffer[1]=0;
	}

	void	Immediate64(uint64_t imm)	{
		_ASSERTE(ExpectBuffer[0]==0);
		_ASSERTE((ExpectBuffer[1]&64)==64);
		e->emit64(imm);
		ExpectBuffer[1]=0;
	}

	void	Displacement8(uint8_t disp)	{
		_ASSERTE((ExpectBuffer[0]&8)==8);
		e->emit8(disp);
		ExpectBuffer[0]=0;
	}

	void	Displacement16(uint16_t disp)	{
		_ASSERTE((ExpectBuffer[0]&16)==16);
		e->emit16(disp);
		ExpectBuffer[0]=0;
	}

	void	Displacement32(uint32_t disp)	{
		_ASSERTE((ExpectBuffer[0]&32)==32);
		e->emit32(disp);
		ExpectBuffer[0]=0;
	}

	void	Displacement64(uint64_t disp)	{
		_ASSERTE((ExpectBuffer[0]&64)==64);
		e->emit64(disp);
		ExpectBuffer[0]=0;
	}
 protected:
  Emitter*	e;
	IAMode		m;
	int ExpectBuffer[2];
	uint8_t _w;
	uint8_t _s;

	inline void __check()	{
		_ASSERTE(ExpectBuffer[0]==0);
		_ASSERTE(ExpectBuffer[1]==0);
	}

	inline void __expect_imm8()	{
		ExpectBuffer[1] = 8;
	}

	inline void __expect_imm16()	{
		ExpectBuffer[1] = 16;
	}

	inline void __expect_imm32()	{
		ExpectBuffer[1] = 32;
	}

	inline void __expect_imm64()	{
		ExpectBuffer[1] = 64;
	}

	inline void __expect_imm()	{
		ExpectBuffer[1] = 64|32|16|8;
	}


	inline void __expect_disp8()	{
		ExpectBuffer[0] = 8;
	}

	inline void __expect_disp16()	{
		ExpectBuffer[0] = 16;
	}

	inline void __expect_disp32()	{
		ExpectBuffer[0] = 32;
	}

	inline void __expect_disp64()	{
		ExpectBuffer[0] = 64;
	}

	inline void __expect_disp()	{
		ExpectBuffer[0] = 64|32|16|8;
	}

  __inline uint8_t WriteBitAt(uint8_t bit,uint8_t index)  {
	  bit&=0x1;
	  return (bit<<index);
  }

  __inline uint8_t WriteBitsAt(uint8_t bits,uint8_t count,uint8_t index)  {
	  bits&=((1<<count)-1);
	  return (bits<<index);
  }

  __inline uint8_t ModRM(uint8_t mod,uint8_t reg,uint8_t rm)  {
	  return (((mod&0x3)<<6)|((reg&0x7)<<3)|((rm&0x7)));
  }

  __inline uint8_t SIB(uint8_t scale,uint8_t index,uint8_t base)  {
	  return (((scale&0x3)<<6)|((index&0x7)<<3)|((base&0x7)));
  }
};
	
}
}
