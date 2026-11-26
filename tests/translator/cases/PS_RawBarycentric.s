; Reads a raw barycentric register: an input layout we haven't confirmed, so it must be rejected.
; @unsupported PS system inputs
v_mov_b32_e32 v4, v0
exp mrt0 v4, v4, v4, v4 done vm
s_endpgm
