	.set noreorder

#include "pspstub.s"

	STUB_START "kcall",0x40090000,0x00030005
	STUB_FUNC  0x75F43FF0,kcall_2
	STUB_FUNC  0xBADD8D3B,kcall_3
	STUB_FUNC  0x2BB46CB6,kinit
	STUB_END
