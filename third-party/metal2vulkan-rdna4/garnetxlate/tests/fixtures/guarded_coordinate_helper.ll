define float @scale_helper(ptr addrspace(2) %buffer) {
entry:
 %p = getelementptr i8, ptr addrspace(2) %buffer, i64 8
 %value = load float, ptr addrspace(2) %p, align 4
 ret float %value
}
