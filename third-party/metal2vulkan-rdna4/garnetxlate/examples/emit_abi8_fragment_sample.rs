use std::path::Path;

fn write(path: &Path, spv: &[u8], metadata: &[u8]) {
    std::fs::write(path.with_extension("spv"), spv).unwrap();
    std::fs::write(path.with_extension("json"), metadata).unwrap();
}

fn main() {
    let out = std::env::args().nth(1).expect("output directory");
    let out = Path::new(&out);
    std::fs::create_dir_all(out).unwrap();

    let source = include_str!("../tests/fixtures/abi8/sampler-fragment-indirect.ll");
    let (spv, metadata) = garnetxlate::translate_bytes(source.as_bytes(), [1, 1, 1]).unwrap();
    write(&out.join("fragment-direct"), &spv, &metadata);

    let (spv, metadata) = garnetxlate::lower_gpu_arguments(&spv, &metadata).unwrap();
    write(&out.join("fragment-gpu"), &spv, &metadata);
}
