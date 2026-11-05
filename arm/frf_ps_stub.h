/* EMU68AGA standalone (FRF_VIRTUAL_AGA): there is no PiStorm and no Amiga
 * motherboard behind the Pi. Every physical-bus access in the AGA glue is
 * turned into a no-op that reads as an open bus. Include AFTER ps_protocol.h.
 * SPDX-License-Identifier: GPL-2.0-only */
#ifndef FRF_PS_STUB_H
#define FRF_PS_STUB_H
#ifdef FRF_VIRTUAL_AGA
#undef ps_read_8
#undef ps_read_16
#undef ps_read_32
#undef ps_write_8
#undef ps_write_16
#undef ps_write_32
#define ps_read_8(a)       ((void)(a), (unsigned)0xffu)
#define ps_read_16(a)      ((void)(a), (unsigned)0xffffu)
#define ps_read_32(a)      ((void)(a), (unsigned)0xffffffffu)
#define ps_write_8(a,v)    do { (void)(a); (void)(v); } while (0)
#define ps_write_16(a,v)   do { (void)(a); (void)(v); } while (0)
#define ps_write_32(a,v)   do { (void)(a); (void)(v); } while (0)
#endif
#endif
