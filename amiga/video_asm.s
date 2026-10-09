/*
 * video_asm.s - Optimized 68k assembly for video blitting
 *
 * RGB565 to RGBA32 conversion with lookup table
 * Simple version for compatibility
 */

	.text
	.even

/***************************************************************************
 * Blit565toRGBA_asm - Convert RGB565 to RGBA32 using lookup table
 *
 * void Blit565toRGBA_asm(UWORD *src, ULONG *dst, ULONG *lut, int pixels)
 *
 * Stack: 8 bytes saved regs + 4 return = args at sp@(12,16,20,24)
 ***************************************************************************/

	.globl	_Blit565toRGBA_asm

_Blit565toRGBA_asm:
	movem.l	d2/a2,-(sp)		| save 2 regs = 8 bytes

	move.l	12(sp),a0		| src
	move.l	16(sp),a1		| dst
	move.l	20(sp),a2		| lut
	move.l	24(sp),d0		| pixels

	tst.l	d0
	ble.s	.Ldone1x

	subq.l	#1,d0			| for dbf

.Lloop1x:
	moveq	#0,d1
	move.w	(a0)+,d1		| get RGB565 pixel
	lsl.l	#2,d1			| * 4 for ULONG index [stall source: writes d1]
	moveq	#0,d2			| PIPELINE BRIDGE: dead write; fills stall slot
	move.l	(a2,d1.l),(a1)+		| lookup and store [no stall]

	dbf	d0,.Lloop1x

	| handle counts > 65535
	sub.l	#0x10000,d0
	bpl.s	.Lloop1x

.Ldone1x:
	movem.l	(sp)+,d2/a2
	rts


/***************************************************************************
 * Blit565toRGBA_2x_asm - Convert RGB565 to RGBA32 with 2x scaling
 *
 * void Blit565toRGBA_2x_asm(UWORD *src, ULONG *dst1, ULONG *dst2, 
 *                           ULONG *lut, int pixels)
 *
 * Stack: 12 bytes saved regs + 4 return = args at sp@(16,20,24,28,32)
 ***************************************************************************/

	.globl	_Blit565toRGBA_2x_asm

_Blit565toRGBA_2x_asm:
	movem.l	d2/a2-a3,-(sp)		| save 3 regs = 12 bytes

	move.l	16(sp),a0		| src
	move.l	20(sp),a1		| dst1
	move.l	24(sp),a2		| dst2
	move.l	28(sp),a3		| lut
	move.l	32(sp),d0		| pixels

	tst.l	d0
	ble.s	.Ldone2x

	subq.l	#1,d0

.Lloop2x:
	moveq	#0,d1
	move.w	(a0)+,d1		| get RGB565 pixel
	lsl.l	#2,d1			| * 4 for ULONG index [stall source: writes d1]
	moveq	#0,d2			| PIPELINE BRIDGE: dead write (d2 overwritten below); fills stall slot
	move.l	(a3,d1.l),d2		| lookup pixel [no stall]
	move.l	d2,(a1)+		| dst1 first
	move.l	d2,(a1)+		| dst1 second (2x horiz)
	move.l	d2,(a2)+		| dst2 first
	move.l	d2,(a2)+		| dst2 second

	dbf	d0,.Lloop2x

	sub.l	#0x10000,d0
	bpl.s	.Lloop2x

.Ldone2x:
	movem.l	(sp)+,d2/a2-a3
	rts


/***************************************************************************
 * Blit565toRGBA_2x_scanline_asm - 2x scale with scanline darkening
 *
 * void Blit565toRGBA_2x_scanline_asm(UWORD *src, ULONG *dst1, ULONG *dst2,
 *                                     ULONG *lut, ULONG *dark_lut, int pixels)
 *
 * dst1 = main line (normal LUT), each pixel written twice (2x horizontal)
 * dst2 = scanline (dark LUT), each pixel written twice
 *
 * Uses pre-computed dark LUT - zero per-pixel math for darkening.
 *
 * Stack: 16 bytes saved regs + 4 return = args at sp@(20,24,28,32,36,40)
 ***************************************************************************/

	.globl	_Blit565toRGBA_2x_scanline_asm

_Blit565toRGBA_2x_scanline_asm:
	movem.l	d2/a2-a4,-(sp)		| save 4 regs = 16 bytes

	move.l	20(sp),a0		| src
	move.l	24(sp),a1		| dst1 (main line)
	move.l	28(sp),a2		| dst2 (scanline)
	move.l	32(sp),a3		| lut (normal)
	move.l	36(sp),a4		| dark_lut
	move.l	40(sp),d0		| pixels

	tst.l	d0
	ble.s	.Ldone_sl

	subq.l	#1,d0

.Lloop_sl:
	moveq	#0,d1
	move.w	(a0)+,d1		| get RGB565 pixel
	lsl.l	#2,d1			| * 4 for ULONG index [stall source: writes d1]
	moveq	#0,d2			| PIPELINE BRIDGE: dead write (d2 overwritten below); fills stall slot

	| Main line: normal LUT, write twice
	move.l	(a3,d1.l),d2		| lookup normal pixel [no stall]
	move.l	d2,(a1)+		| dst1 first copy
	move.l	d2,(a1)+		| dst1 second copy (2x horiz)

	| Scanline: dark LUT, write twice
	move.l	(a4,d1.l),d2		| lookup dark pixel
	move.l	d2,(a2)+		| dst2 first copy
	move.l	d2,(a2)+		| dst2 second copy

	dbf	d0,.Lloop_sl

	sub.l	#0x10000,d0
	bpl.s	.Lloop_sl

.Ldone_sl:
	movem.l	(sp)+,d2/a2-a4
	rts
