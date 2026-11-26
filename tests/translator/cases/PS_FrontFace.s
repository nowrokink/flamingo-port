; SV_Position.x (v2) and SV_IsFrontFace (v3, +-1.0) packed after the barycentrics.
; @isg1 1 0 0xf 0x1
; @isg1 9 1 0x1 0x1 1
v_cmp_lt_f32_e32 vcc_lo, 0, v3
v_cndmask_b32_e64 v4, 0, 1.0, vcc_lo
exp mrt0 v2, v4, v4, v4 done vm
s_endpgm
