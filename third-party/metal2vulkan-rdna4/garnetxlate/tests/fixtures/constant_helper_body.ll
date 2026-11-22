define <4 x float> @read_tail(ptr addrspace(2) %params) {
entry:
  %first = load float, ptr addrspace(2) %params, align 4
  %second_ptr = getelementptr inbounds float, ptr addrspace(2) %params, i64 1
  %second = load float, ptr addrspace(2) %second_ptr, align 4
  %local = alloca [5 x <2 x float>], align 8
  %tail = getelementptr inbounds float, ptr addrspace(2) %params, i64 4
  br label %loop
loop:
  %index = phi i32 [0, %entry], [%next, %loop]
  %wide = zext i32 %index to i64
  %cursor = getelementptr inbounds float, ptr addrspace(2) %tail, i64 %wide
  %value = load float, ptr addrspace(2) %cursor, align 4
  %pair = insertelement <2 x float> zeroinitializer, float %value, i32 0
  %slot = getelementptr inbounds [5 x <2 x float>], ptr %local, i64 0, i64 %wide
  store <2 x float> %pair, ptr %slot, align 8
  %next = add i32 %index, 1
  %done = icmp eq i32 %next, 5
  br i1 %done, label %exit, label %loop
exit:
  %a = insertelement <4 x float> zeroinitializer, float %first, i32 0
  %b = insertelement <4 x float> %a, float %second, i32 1
  %last_ptr = getelementptr inbounds [5 x <2 x float>], ptr %local, i64 0, i64 4
  %last = load <2 x float>, ptr %last_ptr, align 8
  %scalar = extractelement <2 x float> %last, i32 0
  %color = insertelement <4 x float> %b, float %scalar, i32 2
  ret <4 x float> %color
}
