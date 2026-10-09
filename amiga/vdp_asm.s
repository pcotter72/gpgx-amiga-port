/*
 * vdp_asm.s - Optimized 68k assembly for VDP rendering hot paths
 *
 * Three functions:
 *
 *   draw_bg_columns_asm  - Renders N Mode 5 background tile columns to linebuffer.
 *                          Uses swap+lsr.w for attribute bit-field extraction
 *                          (avoids BFEXTU which is misassembled by some GAS versions).
 *
 *   merge_pixels_asm     - Combines two linebuffers with a 64KB priority table.
 *                          Uses moveq+byte-load for zero-extension (saves 2 instrs/pixel
 *                          vs GCC's sign-extension guards).
 *
 *   remap_line_asm       - Converts indexed-colour linebuffer to PIXEL_OUT_T (uint16)
 *                          using add.l+simple index: 2 instructions per pixel for the
 *                          256-entry palette lookup (avoids scaled-index GAS issues).
 *
 * Targets: 68020+ (swap, lsr.w immediate); compiled as part of USE_ASM_VDP=1 build.
 *
 * GNU Assembler (GAS) syntax for bebbo's amiga-gcc.
 * All functions use standard AmigaOS C ABI: args on stack, a0-a1/d0-d1 scratch.
 */


	.text
	.even


/*===========================================================================
 * draw_bg_columns_asm
 *
 * void draw_bg_columns_asm(
 *     uint32 *dst,       sp@(36) output linebuffer (uint32*, 4-byte aligned)
 *     uint32 *nt,        sp@(40) nametable row pointer (uint32*)
 *     int    start_idx,  sp@(44) starting column index
 *     int    col_mask,   sp@(48) pf_col_mask (power-of-2 minus 1, or 0xFFFF for window)
 *     int    count,      sp@(52) number of columns to render (must be > 0)
 *     uint8  *pat_cache, sp@(56) bg_pattern_cache  (byte array, byte-offset addressing)
 *     uint32 *atex_tbl,  sp@(60) atex_table[8]     (const uint32[8])
 *     int    v_line      sp@(64) row offset = (line&7)<<3  (0,8,16,...,56)
 * )
 *
 * Per column: reads one 32-bit nametable entry (ATTR) and renders two 8-pixel tiles
 * (MSB attr = first tile, LSB attr = second tile, big-endian layout) into *dst.
 * Writes 4 uint32s (= 16 pixels) per column.  dst is advanced by caller.
 *
 * Attribute format (big-endian 32-bit nametable word):
 *   MSB tile: bits 31:29 = palette/priority index, bits 28:16 = pattern ID
 *   LSB tile: bits 15:13 = palette/priority index, bits 12:0  = pattern ID
 *
 * Pattern cache layout: each 64-byte block is one tile row set (uint8 array).
 *   Byte offset = pattern_id * 64 + v_line
 *   src[0..3] = cache[offset+0..3],  src[4..7] = cache[offset+4..7]
 *
 * Atex table: 8 uint32 entries; each entry has the palette/priority nibble
 * replicated in every byte so that OR'ing with pixel data sets the right bits.
 *
 * Bit extraction without BFEXTU:
 *   MSB atex_index = (atbuf >> 29) & 7:
 *     rol.l #3 => bit31->bit2, bit30->bit1, bit29->bit0; and.l #7
 *   MSB pattern_id = (atbuf >> 16) & 0x1FFF = atbuf[28:16]:
 *     swap => d3[15:0] = atbuf[31:16]; and.l #0x1FFF => bits 28:16 of orig
 *   LSB atex_index = (atbuf >> 13) & 7 = atbuf[15:13]:
 *     lsr.l #8 + lsr.l #5 (13 total, split because imm max is 8); and.l #7
 *   LSB pattern_id = atbuf & 0x1FFF = atbuf[12:0]:
 *     and.l #0x1FFF
 *
 * Register map:
 *   a0 = dst         a1 = nt          a2 = pat_cache   a3 = atex_tbl
 *   d0 = loop count  d1 = temp        d2 = atex        d3 = src byte-offset
 *   d4 = atbuf       d5 = v_line_b    d6 = index       d7 = col_mask
 *
 * Stack after movem (8 regs * 4 = 32 bytes saved + 4 return addr = 36 bytes):
 *   args start at sp+36.
 *===========================================================================*/

	.globl	_draw_bg_columns_asm

_draw_bg_columns_asm:
	movem.l	d2-d7/a2-a3,-(sp)	| save 8 regs = 32 bytes

	move.l	36(sp),a0		| dst
	move.l	40(sp),a1		| nt
	move.l	44(sp),d6		| start_idx
	move.l	48(sp),d7		| col_mask
	move.l	52(sp),d0		| count
	move.l	56(sp),a2		| pat_cache
	move.l	60(sp),a3		| atex_tbl
	move.l	64(sp),d5		| v_line_b

	tst.l	d0
	ble.s	.Lbgcol_done		| guard: skip if count <= 0

	subq.l	#1,d0			| adjust for dbf (count-1 iterations after first)

.Lbgcol_loop:
	| ------ Load nametable entry: atbuf = nt[index & col_mask] ------
	| Pipeline note: lsl.l result used immediately as EA index -> 1-cycle stall on 68030.
	| Fix: slide addq.l #1,d6 (independent of d1) between the lsl and the load.
	move.l	d6,d1
	and.l	d7,d1			| d1 = index & col_mask
	lsl.l	#2,d1			| d1 = byte offset = (index & mask) * 4
	addq.l	#1,d6			| index++ (PIPELINE BRIDGE: independent; fills stall slot)
	move.l	(a1,d1.l),d4		| d4 = atbuf; no stall (d1 written 1 instruction ago)

	| ------ MSB TILE: bits 31:16 of atbuf ------
	| atex_index = (atbuf >> 29) & 7:
	|   rol.l #3 rotates bit31->bit2, bit30->bit1, bit29->bit0
	| pattern_id = (atbuf >> 16) & 0x1FFF:
	|   swap brings atbuf[31:16] into lower word; and.l masks to bits 28:16
	move.l	d4,d2
	rol.l	#3,d2			| d2[2:0] = atbuf[31:29]  (rol imm 1-8 always valid)
	and.l	#7,d2
	lsl.l	#2,d2			| * 4 = uint32 table byte offset       [stall source: d2]
	move.l	d4,d3			| PIPELINE BRIDGE: start d3=pattern_id_msb (fills stall slot)
	move.l	(a3,d2.l),d2		| d2 = atex_msb; no stall (d2 written 1 instruction ago)

	| d3 = d4 from the bridge above; continue pattern_id extraction:
	swap	d3			| d3[15:0] = atbuf[31:16]
	and.l	#0x1FFF,d3		| d3 = atbuf[28:16] = pattern_id_msb (0..8191)
	lsl.l	#6,d3			| * 64 = byte base of tile row in pat_cache
	or.l	d5,d3			| + v_line_b = final byte offset       [stall source: d3]
	move.l	d4,d1			| PIPELINE BRIDGE: d4->d1 (overwritten below; fills stall slot)
	move.l	(a2,d3.l),d1		| pixels[0..3]; no stall (d3 written 1 instruction ago)
	or.l	d2,d1
	move.l	d1,(a0)+		| *dst++
	move.l	4(a2,d3.l),d1		| pixels[4..7]; no stall (d3 unmodified since or.l)
	or.l	d2,d1
	move.l	d1,(a0)+		| *dst++

	| ------ LSB TILE: bits 15:0 of atbuf ------
	| atex_index = (atbuf >> 13) & 7:
	|   split into lsr.l #8 + lsr.l #5 (imm max is 8; 8+5=13)
	| pattern_id = atbuf & 0x1FFF     =>  and.l #0x1FFF
	move.l	d4,d2
	lsr.l	#8,d2
	lsr.l	#5,d2			| d2[2:0] = atbuf[15:13]  (shifted 13 total)
	and.l	#7,d2			| mask upper bits
	lsl.l	#2,d2			| * 4                                  [stall source: d2]
	move.l	d4,d3			| PIPELINE BRIDGE: start d3=pattern_id_lsb (fills stall slot)
	move.l	(a3,d2.l),d2		| d2 = atex_lsb; no stall (d2 written 1 instruction ago)

	| d3 = d4 from the bridge above; continue pattern_id extraction:
	and.l	#0x1FFF,d3		| d3 = atbuf[12:0] = pattern_id_lsb (0..8191)
	lsl.l	#6,d3			| * 64 = byte base
	or.l	d5,d3			| + v_line_b = byte offset             [stall source: d3]
	move.l	d4,d1			| PIPELINE BRIDGE: d4->d1 (overwritten below; fills stall slot)
	move.l	(a2,d3.l),d1		| no stall (d3 written 1 instruction ago)
	or.l	d2,d1
	move.l	d1,(a0)+
	move.l	4(a2,d3.l),d1		| no stall (d3 unmodified since or.l)
	or.l	d2,d1
	move.l	d1,(a0)+

	dbf	d0,.Lbgcol_loop		| decrement, branch if not -1

	| Handle count > 65535 (defensive; in practice count <= 42)
	sub.l	#0x10000,d0
	bpl.s	.Lbgcol_loop

.Lbgcol_done:
	movem.l	(sp)+,d2-d7/a2-a3
	rts


/*===========================================================================
 * merge_pixels_asm
 *
 * void merge_pixels_asm(uint8 *srca, uint8 *srcb, uint8 *dst,
 *                       uint8 *table, int width)
 *
 * Equivalent to the inline merge() in vdp_render.c:
 *   do { *dst++ = table[(*srcb++ << 8) | (*srca++)]; } while (--width);
 *
 * srca = plane A linebuffer (indexed pixel bytes, 0x00..0xFF)
 * srcb = plane B linebuffer (indexed pixel bytes, 0x00..0xFF)
 * table = 64KB priority lookup table (uint8 lut[65536])
 * dst = merged output buffer (may alias srca or srcb)
 *
 * Optimisation: moveq #0 + byte-load zero-extends without the extra
 * sign-extension guard instructions that GCC conservatively emits.
 * Saves ~2 instructions per pixel vs typical GCC output.
 *
 * Stack after movem (3 regs * 4 = 12 bytes + 4 return = 16 bytes):
 *   args at sp+16.
 *
 * Register map:
 *   a0=srca  a1=srcb  a2=dst  a3=table
 *   d0=width-1  d1=srca_byte  d2=srcb_byte (becomes 16-bit index)
 *===========================================================================*/

	.globl	_merge_pixels_asm

_merge_pixels_asm:
	movem.l	d2/a2-a3,-(sp)		| save 3 regs = 12 bytes + 4 ret = 16 bytes offset

	move.l	16(sp),a0		| srca
	move.l	20(sp),a1		| srcb
	move.l	24(sp),a2		| dst
	move.l	28(sp),a3		| table
	move.l	32(sp),d0		| width

	tst.l	d0
	ble.s	.Lmerge_done

	subq.l	#1,d0			| for dbf

	| Pipeline fix: pre-zero d1 so the first iteration's move.b into d1 works
	| correctly. The loop body ends with moveq #0,d1 which doubles as both the
	| PIPELINE BRIDGE (fills the ALU->EA stall slot after or.w d1,d2) and the
	| re-initialization of d1 for the next iteration's move.b load.
	moveq	#0,d1			| pre-zero d1 for first iteration

.Lmerge_loop:
	move.b	(a0)+,d1		| d1.l = srca byte (d1 already zeroed; no sign-ext needed)
	moveq	#0,d2
	move.b	(a1)+,d2		| d2.l = srcb byte (zero-extended)
	lsl.w	#8,d2			| d2.w = srcb<<8  (upper word stays 0 from moveq)
	or.w	d1,d2			| d2.l = (srcb<<8)|srca  = 16-bit table index [stall source: d2]
	moveq	#0,d1			| PIPELINE BRIDGE: re-zero d1 for next iter (fills stall slot)
	move.b	(a3,d2.l),(a2)+		| *dst++ = table[index]; no stall (d2 written 1 instr ago)

	dbf	d0,.Lmerge_loop

	sub.l	#0x10000,d0
	bpl.s	.Lmerge_loop

.Lmerge_done:
	movem.l	(sp)+,d2/a2-a3
	rts


/*===========================================================================
 * remap_line_asm
 *
 * void remap_line_asm(uint8 *src, uint16 *dst, uint16 *pixel_tbl, int width)
 *
 * Equivalent to the inner loop of remap_line() in vdp_render.c:
 *   do { *dst++ = pixel[*src++]; } while (--width);
 *
 * src       = 1-byte-per-pixel indexed colour linebuffer
 * dst       = 2-byte-per-pixel RGB565 output (bitmap.data row)
 * pixel_tbl = uint16 palette table, 256 entries (512 bytes)
 * width     = number of pixels (320 or 256 - always even for Genesis/SMS)
 *
 * Optimisation: 2x unrolled loop using d1+d2 to eliminate the ALU->EA
 * pipeline stall on 68030. The original single-pixel loop had:
 *   add.l d1,d1  (writes d1)  followed immediately by
 *   move.w (a2,d1.l)          (uses d1 as EA index) -> 1-cycle stall.
 * Fix: process two pixels per iteration. Pixel 2's `add.l d2,d2` bridges
 * pixel 1's stall; pixel 1's `move.w (a2,d1.l)` bridges pixel 2's stall.
 * Width is always even (320 mode5 / 256 mode4), so halving is exact.
 *
 * Stack after movem (2 regs * 4 = 8 bytes + 4 return = 12 bytes offset):
 *   args at sp+12.
 *
 * Register map:
 *   a0=src  a1=dst  a2=pixel_tbl
 *   d0=pair_count-1  d1=pixel1_index  d2=pixel2_index
 *===========================================================================*/

	.globl	_remap_line_asm

_remap_line_asm:
	movem.l	d2/a2,-(sp)		| save 2 regs = 8 bytes

	move.l	12(sp),a0		| src
	move.l	16(sp),a1		| dst
	move.l	20(sp),a2		| pixel_tbl
	move.l	24(sp),d0		| width

	tst.l	d0
	ble.s	.Lremap_done

	lsr.l	#1,d0			| d0 = width/2 (width always even: 320 or 256)
	subq.l	#1,d0			| adjust for dbf

.Lremap_loop:
	| Load two pixels, compute both byte offsets, then store in interleaved order.
	| This fills each stall slot with the other pixel's independent computation:
	|   add.l d1,d1  [stall source: d1]
	|   add.l d2,d2  [bridge for d1; stall source: d2]
	|   move.w (a2,d1.l)  [no stall: d1 written 1 instr ago]
	|   move.w (a2,d2.l)  [no stall: d2 written 1 instr ago via move.w d1 bridge]
	moveq	#0,d1
	move.b	(a0)+,d1		| d1 = pixel1 (zero-extended, 0..255)
	moveq	#0,d2
	move.b	(a0)+,d2		| d2 = pixel2 (zero-extended)
	add.l	d1,d1			| d1 = pixel1 * 2 (byte offset) [stall source]
	add.l	d2,d2			| d2 = pixel2 * 2 (byte offset) [bridge for d1]
	move.w	(a2,d1.l),(a1)+		| *dst++ = pixel_tbl[pixel1]; no stall
	move.w	(a2,d2.l),(a1)+		| *dst++ = pixel_tbl[pixel2]; no stall

	dbf	d0,.Lremap_loop

	sub.l	#0x10000,d0
	bpl.s	.Lremap_loop

.Lremap_done:
	movem.l	(sp)+,d2/a2
	rts
