#pragma once

#include <stddef.h>

namespace rdna4dml {

struct Input;
struct Result;
struct Soc;
void dcn401SocDefaults(Soc &soc);
size_t dml21WorkspaceBytes();
size_t dml21InstanceBytes();
bool solveWithDml21(const Input &input, Result &result, void *workspace, void *instance);

} // namespace rdna4dml
