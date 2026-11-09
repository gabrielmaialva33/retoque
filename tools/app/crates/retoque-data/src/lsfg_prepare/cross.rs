//! Same SM 6.0 options and textual register remap as tools/lsfg-study/convert.py.
use super::{Error, Result};
use spirv_cross2::{
    Compiler, Module,
    compile::hlsl::{CompilerOptions, HlslShaderModel},
    targets::Hlsl,
};
use std::{collections::BTreeMap, sync::LazyLock};
static REG: LazyLock<regex::Regex> = LazyLock::new(|| regex::Regex::new(r"register\(([btsu])(\d+), space0\)").unwrap());
pub fn remap(hlsl: &str) -> (String, BTreeMap<u32, String>) {
    let mut bindings = BTreeMap::new();
    let text = REG.replace_all(hlsl, |m: &regex::Captures<'_>| {
        let binding: u32 = m[2].parse().expect("SPIR-V binding fits u32");
        bindings.entry(binding).or_insert_with(|| m[1].into());
        format!("register({}0, space{})", &m[1], binding)
    });
    (text.into_owned(), bindings)
}
pub fn hlsl(words: &[u32]) -> Result<(String, BTreeMap<u32, String>)> {
    let compiler = Compiler::<Hlsl>::new(Module::from_words(words)).map_err(|e| Error::Failed(e.to_string()))?;
    let mut options = CompilerOptions::default();
    options.shader_model = HlslShaderModel::ShaderModel6_0;
    let result = compiler.compile(&options).map_err(|e| Error::Failed(e.to_string()))?;
    Ok(remap(result.as_ref()))
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn identical_register_policy_including_arrays() {
        let (s, b) = remap(
            "Texture2D<float4> t[4] : register(t5, space0); SamplerState s : register(s2, space0); cbuffer c : register(b0, space0); RWTexture2D<float4> o : register(u8, space0);",
        );
        assert_eq!(b.keys().copied().collect::<Vec<_>>(), vec![0, 2, 5, 8]);
        for r in ["t0, space5", "s0, space2", "b0, space0", "u0, space8"] {
            assert!(s.contains(r));
        }
        assert_eq!(remap("register(t2, space1)").0, "register(t2, space1)");
    }
}
