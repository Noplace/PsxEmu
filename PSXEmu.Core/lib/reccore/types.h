namespace reccore {

template <typename F>
struct return_type
{
  typedef F type;
};


struct CodeBlock {
	SIZE_T	size;
	SIZE_T	cursor;
	LPVOID	address;
	uint8_t*	ptr8bit;



  /*void operator ()() {
	  auto func	= static_cast<void (*)()>(address);
	  return func();
  }

  template <typename P1>
  P1 operator ()(P1 p1) {
	  auto func	= static_cast<P1 (*)(P1 p1)>(address);
	  return func(p1);
  }
  
  template <typename P1, typename P2>
  void operator ()(P1 p1, P2 p2) {
	  auto func	= static_cast<void (*)(P1,P2)>(address);
	  return func(p1,p2);
  }*/

  template<class ... Params>
  void operator ()(Params... params) {
	  auto func = static_cast<void(*)(Params...)>(address);
	  return func(params...);
  }
};

}

