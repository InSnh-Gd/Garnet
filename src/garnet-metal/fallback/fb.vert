#version 450
// Keep a cache-miss draw harmless until the translated pipeline is hot-swapped.
// A clipped triangle preserves the existing surface instead of painting a diagnostic colour over it.
void main() {
    gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
}
