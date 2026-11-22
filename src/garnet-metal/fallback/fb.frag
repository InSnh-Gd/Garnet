#version 450
// The fallback must not modify the caller's surface while translation is pending.
void main() { discard; }
