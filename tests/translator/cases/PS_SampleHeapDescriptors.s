; Samples a texture whose T# and S# sit in the GPU heap: s4 holds the VA of a T# (8 dwords) followed by an S#
; (4 dwords), the high dword forced to 0 as game shaders do. Exercises heap base + Xbox VA, the descriptor hash
; table and bindless sampling. The runtime GPU test (runtime/test/gpu_layer_test.cpp) draws it.
; @isg1 0 0 0x3 0x3
s_mov_b32 m0, s0
v_interp_p1_f32_e32 v2, v0, attr0.x
v_interp_p2_f32_e32 v2, v1, attr0.x
v_interp_p1_f32_e32 v3, v0, attr0.y
v_interp_p2_f32_e32 v3, v1, attr0.y
s_mov_b32 s5, 0
s_load_dwordx8 s[8:15], s[4:5], 0x0
s_load_dwordx4 s[16:19], s[4:5], 0x20
s_waitcnt lgkmcnt(0)
image_sample v[4:7], v[2:3], s[8:15], s[16:19] dmask:0xf dim:SQ_RSRC_IMG_2D
s_waitcnt vmcnt(0)
exp mrt0 v4, v5, v6, v7 done vm
s_endpgm
