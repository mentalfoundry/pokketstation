@ 3x5 hex-digit font: 16 glyphs (0-9,A-F), 5 rows each, 3 bits/row.
@ bit2=leftmost pixel, bit1=mid, bit0=rightmost. See draw_glyph in ui.s
@ for how this is indexed and decoded.
    .syntax unified
    .arm

    .section .font_table, "ax"
    .global font_table

font_table:
    .byte 7,5,5,5,7   @ 0
    .byte 2,6,2,2,7   @ 1
    .byte 7,1,7,4,7   @ 2
    .byte 7,1,7,1,7   @ 3
    .byte 5,5,7,1,1   @ 4
    .byte 7,4,7,1,7   @ 5
    .byte 7,4,7,5,7   @ 6
    .byte 7,1,2,2,2   @ 7
    .byte 7,5,7,5,7   @ 8
    .byte 7,5,7,1,7   @ 9
    .byte 7,5,7,5,5   @ A
    .byte 6,5,6,5,6   @ B
    .byte 7,4,4,4,7   @ C
    .byte 6,5,5,5,6   @ D
    .byte 7,4,7,4,7   @ E
    .byte 7,4,7,4,4   @ F
font_table_end:

.if (font_table_end - font_table) != 80
    .error "font table is not 80 bytes"
.endif
