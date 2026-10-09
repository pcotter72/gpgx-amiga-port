/*
 * blip_asm.s - Optimized 68k assembly for blip_buf audio resampling
 *
 * Replaces the inner loop of blip_read_samples (stereo mode).
 * Called once per frame for ~368-735 samples depending on frequency.
 *
 * GNU Assembler (GAS) syntax for bebbo's amiga-gcc
 */

	.text
	.even

/***************************************************************************
 * blip_read_samples_inner_asm - Stereo integrator + clamp + high-pass loop
 *
 * void blip_read_samples_inner_asm(
 *     int *in_l,       // sp@(4)   Left channel buffer (buf_t*)
 *     int *in_r,       // sp@(8)   Right channel buffer (buf_t*)
 *     short *out,      // sp@(12)  Interleaved stereo output
 *     int count,       // sp@(16)  Number of sample pairs
 *     int *integrator  // sp@(20)  int[2]: {sum_l, sum_r}
 * )
 *
 * Constants (from blip_buf.c):
 *   delta_bits = 15
 *   bass_shift = 9
 *   delta_bits - bass_shift = 6
 *   max_sample = +32767  (0x00007FFF)
 *   min_sample = -32768  (0xFFFF8000)
 *
 * Per sample pair (L+R):
 *   s = sum >> 15           (arithmetic right shift)
 *   sum += *in++
 *   clamp(s, -32768..32767)
 *   *out++ = s
 *   sum -= s << 6           (high-pass filter)
 *
 * Register allocation:
 *   a0 = in_l pointer
 *   a1 = in_r pointer
 *   a2 = out pointer
 *   d0 = count (loop)
 *   d1 = sum_l (integrator left)
 *   d2 = sum_r (integrator right)
 *   d3 = s (current sample, temp)
 *   d4 = temp for shift/filter
 *
 * Stack: 5 regs saved (d2-d4/a2/a3) = 20 bytes + 4 return = 24
 * Args at: sp@(24), sp@(28), sp@(32), sp@(36), sp@(40)
 ***************************************************************************/

	.globl	_blip_read_samples_inner_asm

_blip_read_samples_inner_asm:
	movem.l	d2-d4/a2-a3,-(sp)	| save 5 regs = 20 bytes

	move.l	24(sp),a0		| in_l
	move.l	28(sp),a1		| in_r
	move.l	32(sp),a2		| out
	move.l	36(sp),d0		| count
	move.l	40(sp),a3		| integrator pointer

	| Load integrators
	move.l	(a3),d1			| sum_l = integrator[0]
	move.l	4(a3),d2		| sum_r = integrator[1]

	tst.l	d0
	ble	.Ldone_blip

	subq.l	#1,d0			| for dbf

.Lloop_blip:
	| === LEFT CHANNEL ===
	| s = sum_l >> 15 (arithmetic)
	move.l	d1,d3			| d3 = sum_l
	asr.l	#8,d3			| shift right 8
	asr.l	#7,d3			| shift right 7 more = 15 total

	| sum_l += *in_l++
	add.l	(a0)+,d1

	| Clamp s to [-32768, +32767]
	cmp.l	#32767,d3
	ble.s	.Lno_hi_l
	move.l	#32767,d3
	bra.s	.Lclamp_done_l
.Lno_hi_l:
	cmp.l	#-32768,d3
	bge.s	.Lclamp_done_l
	move.l	#-32768,d3
.Lclamp_done_l:

	| *out++ = (short)s
	move.w	d3,(a2)+

	| High-pass: sum_l -= s << 6
	move.l	d3,d4
	asl.l	#6,d4
	sub.l	d4,d1

	| === RIGHT CHANNEL ===
	| s = sum_r >> 15 (arithmetic)
	move.l	d2,d3
	asr.l	#8,d3
	asr.l	#7,d3

	| sum_r += *in_r++
	add.l	(a1)+,d2

	| Clamp s to [-32768, +32767]
	cmp.l	#32767,d3
	ble.s	.Lno_hi_r
	move.l	#32767,d3
	bra.s	.Lclamp_done_r
.Lno_hi_r:
	cmp.l	#-32768,d3
	bge.s	.Lclamp_done_r
	move.l	#-32768,d3
.Lclamp_done_r:

	| *out++ = (short)s
	move.w	d3,(a2)+

	| High-pass: sum_r -= s << 6
	move.l	d3,d4
	asl.l	#6,d4
	sub.l	d4,d2

	dbf	d0,.Lloop_blip

	| Handle counts > 65535
	sub.l	#0x10000,d0
	bpl.s	.Lloop_blip

.Ldone_blip:
	| Store integrators back
	move.l	d1,(a3)			| integrator[0] = sum_l
	move.l	d2,4(a3)		| integrator[1] = sum_r

	movem.l	(sp)+,d2-d4/a2-a3
	rts
