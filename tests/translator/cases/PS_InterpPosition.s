; Attribute interpolation plus SV_Position.xy (v2, v3), RTZ f16 packing and a compressed export.
; @isg1 1 0 0xf 0x3
; @isg1 0 1 0x3 0x3
s_mov_b32 m0, s0
v_interp_p1_f32_e32 v4, v0, attr0.x
v_interp_p2_f32_e32 v4, v1, attr0.x
v_interp_p1_f32_e32 v5, v0, attr0.y
v_interp_p2_f32_e32 v5, v1, attr0.y
v_mul_f32_e32 v6, v4, v2
v_mul_f32_e32 v7, v5, v3
v_cvt_pkrtz_f16_f32_e64 v8, v6, v7
v_cvt_pkrtz_f16_f32_e64 v9, v4, v5
exp mrt0 v8, v8, v9, v9 done compr vm
s_endpgm
