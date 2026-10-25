; Mapper test ROM for the v1.10 btst bug (gcc tested bit 24 instead of bit 0).
; Assemble with -DSEGANET=1 or -DTEKKEN=1; mkrom.py pads it to 64 KB and sets
; the last word so the word sum from $200 matches the rom_database entry
; (header checksum $0000; real checksum $F9D1 Kanzume / $C2F0 Tekken 3 Special).
; Screen backdrop: green = mapper behaved as the C source says, red = not.
	org	0
	dc.l	$00FFFE00,start
	dcb.l	62,hang
	dc.b	"SEGA MEGA DRIVE "		; $100
	dc.b	"(C)TEST 2026.OCT"		; $110
	dcb.b	48,' '				; $120 domestic name
	dcb.b	48,' '				; $150 overseas name
	dc.b	"GM 00000000-00"		; $180
	dc.w	0				; $18E header checksum
	dc.b	"J               "		; $190 I/O
	dc.l	0,$0000FFFF			; $1A0 ROM range
	dc.l	$00FF0000,$00FFFFFF		; $1A8 RAM range
	dcb.b	12,' '				; $1B0 no SRAM
	dcb.b	12,' '				; $1BC modem
	dcb.b	40,' '				; $1C8 memo
	dc.b	"JUE             "		; $1F0 region
start:						; $200
	move.w	#$2700,sr
	lea	$C00004,a1
	lea	$C00000,a0
	move.w	#$8004,(a1)
	move.w	#$8144,(a1)			; display on, mode 5
	move.w	#$8700,(a1)			; backdrop = CRAM 0
	ifd	SEGANET
	move.b	#1,$A130F1			; mapper_seganet_w: bit 0 set = ROM write protect
	move.b	mark,d1
	move.b	#$5A,mark			; must be ignored
	move.b	mark,d0
	sub.b	d1,d0				; 0 = unchanged = pass
	endif
	ifd	TEKKEN
	move.b	#$FF,$400000			; reset regs[0]
	move.b	#$01,$400004			; data input, D0 set -> regs[0] bit 0
	move.b	$400003,d0			; returns regs[0] - 1: 0 = pass
	endif
	move.l	#$C0000000,(a1)			; CRAM write, address 0
	tst.b	d0
	bne.s	fail
	move.w	#$00E0,(a0)			; green
	bra.s	loop
fail:
	move.w	#$000E,(a0)			; red
loop:
	bra.s	loop
hang:
	bra.s	hang
	dcb.b	$1000-*,$FF
mark:
	dc.b	$AA
