/*
 * audio_asm.s - Optimized 68k assembly for Paula audio conversion
 *
 * Three routines:
 * 1. ConvertMono8_asm   - 16-bit stereo to 8-bit mono (for 8-bit Paula)
 * 2. ConvertStereo8_asm - 16-bit stereo to separate 8-bit L/R
 * 3. Convert14Tab_asm   - 16-bit stereo to 14-bit HI/LO split via byte tables
 *
 * GNU Assembler (GAS) syntax for bebbo's amiga-gcc
 */

	.text
	.even

/***************************************************************************
 * ConvertMono8_asm - Convert 16-bit stereo to 8-bit mono with volume boost
 *
 * void ConvertMono8_asm(signed short *src, signed char *dst, int samples)
 *
 * Reads paula_volume_boost global (0-4) for post-blip volume amplification.
 * Mixes L+R, applies boost, shifts to 8-bit, clamps -128/+127.
 * 4x unrolled loop.
 *
 * Stack after register save:
 * sp@(0-15)  = saved d2-d5 (16 bytes)
 * sp@(16)    = return address (4 bytes)  
 * sp@(20)    = src
 * sp@(24)    = dst
 * sp@(28)    = samples
 ***************************************************************************/

	.globl	_ConvertMono8_asm
	.extern	_paula_volume_boost

_ConvertMono8_asm:
	moveml	%d2-%d5,%sp@-

	movel	%sp@(20),%a0		/* src */
	movel	%sp@(24),%a1		/* dst */
	movel	%sp@(28),%d0		/* samples */

	tstl	%d0
	ble	.Ldone8

	/* Load volume boost shift count into d3 */
	movel	_paula_volume_boost,%d3

	/* Process 4 samples at a time */
	movel	%d0,%d4
	lsrl	#2,%d4
	beq	.Lrem8

.Lloop8:
	/* Sample 1: L+R -> boost -> >>9 -> clamp -> 8-bit */
	movew	%a0@+,%d1
	movew	%a0@+,%d2
	extl	%d1
	extl	%d2
	addl	%d2,%d1
	lsll	%d3,%d1			/* volume boost */
	addl	#256,%d1		/* +256 = round-to-nearest for >>9 (matches C path) */
	asrl	#1,%d1
	asrl	#8,%d1
	cmpl	#127,%d1
	ble	.Lnoh1
	moveq	#127,%d1
.Lnoh1:	cmpl	#-128,%d1
	bge	.Lnol1
	moveq	#-128,%d1
.Lnol1:	moveb	%d1,%a1@+

	/* Sample 2 */
	movew	%a0@+,%d1
	movew	%a0@+,%d2
	extl	%d1
	extl	%d2
	addl	%d2,%d1
	lsll	%d3,%d1
	addl	#256,%d1		/* round */
	asrl	#1,%d1
	asrl	#8,%d1
	cmpl	#127,%d1
	ble	.Lnoh2
	moveq	#127,%d1
.Lnoh2:	cmpl	#-128,%d1
	bge	.Lnol2
	moveq	#-128,%d1
.Lnol2:	moveb	%d1,%a1@+

	/* Sample 3 */
	movew	%a0@+,%d1
	movew	%a0@+,%d2
	extl	%d1
	extl	%d2
	addl	%d2,%d1
	lsll	%d3,%d1
	addl	#256,%d1		/* round */
	asrl	#1,%d1
	asrl	#8,%d1
	cmpl	#127,%d1
	ble	.Lnoh3
	moveq	#127,%d1
.Lnoh3:	cmpl	#-128,%d1
	bge	.Lnol3
	moveq	#-128,%d1
.Lnol3:	moveb	%d1,%a1@+

	/* Sample 4 */
	movew	%a0@+,%d1
	movew	%a0@+,%d2
	extl	%d1
	extl	%d2
	addl	%d2,%d1
	lsll	%d3,%d1
	addl	#256,%d1		/* round */
	asrl	#1,%d1
	asrl	#8,%d1
	cmpl	#127,%d1
	ble	.Lnoh4
	moveq	#127,%d1
.Lnoh4:	cmpl	#-128,%d1
	bge	.Lnol4
	moveq	#-128,%d1
.Lnol4:	moveb	%d1,%a1@+

	subql	#1,%d4
	bne	.Lloop8

.Lrem8:
	/* Remainder 0-3 samples */
	andl	#3,%d0
	beq	.Ldone8

.Lloop1_8:
	movew	%a0@+,%d1
	movew	%a0@+,%d2
	extl	%d1
	extl	%d2
	addl	%d2,%d1
	lsll	%d3,%d1
	addl	#256,%d1		/* round */
	asrl	#1,%d1
	asrl	#8,%d1
	cmpl	#127,%d1
	ble	.Lnoh5
	moveq	#127,%d1
.Lnoh5:	cmpl	#-128,%d1
	bge	.Lnol5
	moveq	#-128,%d1
.Lnol5:	moveb	%d1,%a1@+
	subql	#1,%d0
	bne	.Lloop1_8

.Ldone8:
	moveml	%sp@+,%d2-%d5
	rts


/***************************************************************************
 * ConvertStereo8_asm - Convert 16-bit stereo to separate 8-bit L/R
 *
 * void ConvertStereo8_asm(signed short *src, signed char *dst_left,
 *                         signed char *dst_right, int samples)
 *
 * Reads paula_volume_boost global for post-blip amplification.
 * 4x unrolled loop.
 *
 * Stack after register save:
 * sp@(0-23)  = saved d2-d6/a2 (24 bytes)
 * sp@(24)    = return address (4 bytes)
 * sp@(28)    = src
 * sp@(32)    = dst_left
 * sp@(36)    = dst_right
 * sp@(40)    = samples
 ***************************************************************************/

	.globl	_ConvertStereo8_asm

_ConvertStereo8_asm:
	moveml	%d2-%d6/%a2,%sp@-	/* a2 is callee-saved; save it */

	movel	%sp@(28),%a0		/* src */
	movel	%sp@(32),%a1		/* dst_left */
	movel	%sp@(36),%a2		/* dst_right */
	movel	%sp@(40),%d0		/* samples */

	tstl	%d0
	ble	.Ldone_s8

	/* Load volume boost */
	movel	_paula_volume_boost,%d3

	/* Process 4 at a time */
	movel	%d0,%d4
	lsrl	#2,%d4
	beq	.Lrem_s8

.Lloop_s8:
	/* Sample 1 */
	movew	%a0@+,%d1		/* left 16-bit */
	movew	%a0@+,%d2		/* right 16-bit */
	extl	%d1
	extl	%d2
	lsll	%d3,%d1			/* boost */
	lsll	%d3,%d2
	addl	#128,%d1		/* +128 = round-to-nearest for >>8 */
	addl	#128,%d2
	asrl	#8,%d1			/* >>8 to 8-bit */
	asrl	#8,%d2
	cmpl	#127,%d1
	ble	.Ls1lh
	moveq	#127,%d1
.Ls1lh:	cmpl	#-128,%d1
	bge	.Ls1ll
	moveq	#-128,%d1
.Ls1ll:	cmpl	#127,%d2
	ble	.Ls1rh
	moveq	#127,%d2
.Ls1rh:	cmpl	#-128,%d2
	bge	.Ls1rl
	moveq	#-128,%d2
.Ls1rl:	moveb	%d1,%a1@+
	moveb	%d2,%a2@+

	/* Sample 2 */
	movew	%a0@+,%d1
	movew	%a0@+,%d2
	extl	%d1
	extl	%d2
	lsll	%d3,%d1
	lsll	%d3,%d2
	addl	#128,%d1		/* round */
	addl	#128,%d2
	asrl	#8,%d1
	asrl	#8,%d2
	cmpl	#127,%d1
	ble	.Ls2lh
	moveq	#127,%d1
.Ls2lh:	cmpl	#-128,%d1
	bge	.Ls2ll
	moveq	#-128,%d1
.Ls2ll:	cmpl	#127,%d2
	ble	.Ls2rh
	moveq	#127,%d2
.Ls2rh:	cmpl	#-128,%d2
	bge	.Ls2rl
	moveq	#-128,%d2
.Ls2rl:	moveb	%d1,%a1@+
	moveb	%d2,%a2@+

	/* Sample 3 */
	movew	%a0@+,%d1
	movew	%a0@+,%d2
	extl	%d1
	extl	%d2
	lsll	%d3,%d1
	lsll	%d3,%d2
	addl	#128,%d1		/* round */
	addl	#128,%d2
	asrl	#8,%d1
	asrl	#8,%d2
	cmpl	#127,%d1
	ble	.Ls3lh
	moveq	#127,%d1
.Ls3lh:	cmpl	#-128,%d1
	bge	.Ls3ll
	moveq	#-128,%d1
.Ls3ll:	cmpl	#127,%d2
	ble	.Ls3rh
	moveq	#127,%d2
.Ls3rh:	cmpl	#-128,%d2
	bge	.Ls3rl
	moveq	#-128,%d2
.Ls3rl:	moveb	%d1,%a1@+
	moveb	%d2,%a2@+

	/* Sample 4 */
	movew	%a0@+,%d1
	movew	%a0@+,%d2
	extl	%d1
	extl	%d2
	lsll	%d3,%d1
	lsll	%d3,%d2
	addl	#128,%d1		/* round */
	addl	#128,%d2
	asrl	#8,%d1
	asrl	#8,%d2
	cmpl	#127,%d1
	ble	.Ls4lh
	moveq	#127,%d1
.Ls4lh:	cmpl	#-128,%d1
	bge	.Ls4ll
	moveq	#-128,%d1
.Ls4ll:	cmpl	#127,%d2
	ble	.Ls4rh
	moveq	#127,%d2
.Ls4rh:	cmpl	#-128,%d2
	bge	.Ls4rl
	moveq	#-128,%d2
.Ls4rl:	moveb	%d1,%a1@+
	moveb	%d2,%a2@+

	subql	#1,%d4
	bne	.Lloop_s8

.Lrem_s8:
	andl	#3,%d0
	beq	.Ldone_s8

.Lloop1_s8:
	movew	%a0@+,%d1
	movew	%a0@+,%d2
	extl	%d1
	extl	%d2
	lsll	%d3,%d1
	lsll	%d3,%d2
	addl	#128,%d1		/* round */
	addl	#128,%d2
	asrl	#8,%d1
	asrl	#8,%d2
	cmpl	#127,%d1
	ble	.Ls5lh
	moveq	#127,%d1
.Ls5lh:	cmpl	#-128,%d1
	bge	.Ls5ll
	moveq	#-128,%d1
.Ls5ll:	cmpl	#127,%d2
	ble	.Ls5rh
	moveq	#127,%d2
.Ls5rh:	cmpl	#-128,%d2
	bge	.Ls5rl
	moveq	#-128,%d2
.Ls5rl:	moveb	%d1,%a1@+
	moveb	%d2,%a2@+
	subql	#1,%d0
	bne	.Lloop1_s8

.Ldone_s8:
	moveml	%sp@+,%d2-%d6/%a2
	rts


/***************************************************************************
 * Convert14Tab_asm - 16-bit stereo to 14-bit HI/LO via two byte tables
 *
 * void Convert14Tab_asm(signed short *src,
 *                       signed char *dst_lhi, signed char *dst_llo,
 *                       signed char *dst_rhi, signed char *dst_rlo,
 *                       int samples,
 *                       const signed char *hi_mid, const signed char *lo_mid)
 *
 * hi_mid / lo_mid point at the entry for sample 0 of two 65536-byte tables
 * (index -32768..32767), built in audio_paula14.c with the volume boost,
 * clamping and the sign-symmetric HI/LO split already applied. So per
 * stereo pair: two word loads and four memory-to-memory byte moves.
 *
 * (d8,An,Xn.W) sign-extends the word index (M68000PRM 2.2.7) and is a
 * valid MOVE source on every 680x0, so a signed sample indexes the table
 * directly. Each sample register is loaded at least two instructions
 * before it is used as an index (load -> EA spacing).
 * 4x unrolled like the other converters.
 *
 * Register save: d2-d3/a2-a6 = 7 regs = 28 bytes
 * Stack (after the 28-byte save + 4-byte return address):
 * sp@(32) src       sp@(36) dst_lhi   sp@(40) dst_llo   sp@(44) dst_rhi
 * sp@(48) dst_rlo   sp@(52) samples   sp@(56) hi_mid    sp@(60) lo_mid
 ***************************************************************************/

	.globl	_Convert14Tab_asm

_Convert14Tab_asm:
	moveml	%d2-%d3/%a2-%a6,%sp@-	/* save 7 regs = 28 bytes */

	movel	%sp@(32),%a0		/* src */
	movel	%sp@(36),%a1		/* dst_lhi */
	movel	%sp@(40),%a2		/* dst_llo */
	movel	%sp@(44),%a3		/* dst_rhi */
	movel	%sp@(48),%a4		/* dst_rlo */
	movel	%sp@(52),%d0		/* samples */
	movel	%sp@(56),%a5		/* hi_mid */
	movel	%sp@(60),%a6		/* lo_mid */

	tstl	%d0
	ble	.Ldone14t

	movel	%d0,%d3
	lsrl	#2,%d3			/* groups of 4 pairs */
	beq	.Lrem14t

.Lloop14t:
	movew	%a0@+,%d1		/* L */
	movew	%a0@+,%d2		/* R */
	moveb	%a5@(0,%d1:w),%a1@+	/* LHI = hi[L] */
	moveb	%a6@(0,%d1:w),%a2@+	/* LLO = lo[L] */
	moveb	%a5@(0,%d2:w),%a3@+	/* RHI = hi[R] */
	moveb	%a6@(0,%d2:w),%a4@+	/* RLO = lo[R] */

	movew	%a0@+,%d1
	movew	%a0@+,%d2
	moveb	%a5@(0,%d1:w),%a1@+
	moveb	%a6@(0,%d1:w),%a2@+
	moveb	%a5@(0,%d2:w),%a3@+
	moveb	%a6@(0,%d2:w),%a4@+

	movew	%a0@+,%d1
	movew	%a0@+,%d2
	moveb	%a5@(0,%d1:w),%a1@+
	moveb	%a6@(0,%d1:w),%a2@+
	moveb	%a5@(0,%d2:w),%a3@+
	moveb	%a6@(0,%d2:w),%a4@+

	movew	%a0@+,%d1
	movew	%a0@+,%d2
	moveb	%a5@(0,%d1:w),%a1@+
	moveb	%a6@(0,%d1:w),%a2@+
	moveb	%a5@(0,%d2:w),%a3@+
	moveb	%a6@(0,%d2:w),%a4@+

	subql	#1,%d3
	bne	.Lloop14t

.Lrem14t:
	andl	#3,%d0			/* 0-3 pairs left */
	beq	.Ldone14t

.Lone14t:
	movew	%a0@+,%d1
	movew	%a0@+,%d2
	moveb	%a5@(0,%d1:w),%a1@+
	moveb	%a6@(0,%d1:w),%a2@+
	moveb	%a5@(0,%d2:w),%a3@+
	moveb	%a6@(0,%d2:w),%a4@+
	subql	#1,%d0
	bne	.Lone14t

.Ldone14t:
	moveml	%sp@+,%d2-%d3/%a2-%a6
	rts
