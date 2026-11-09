//! Shared literal fuzzy matching. Work happens only when a projection's inputs change.
use nucleo_matcher::{
    Config, Matcher, Utf32Str,
    pattern::{AtomKind, CaseMatching, Normalization, Pattern},
};

pub(crate) struct Search {
    matcher: Matcher,
    pattern: Pattern,
    buffer: Vec<char>,
}
impl Search {
    pub fn new(query: &str) -> Self {
        // Normalize the needle too: pt-BR accents must be ignored in both directions.
        let query: String = nucleo_matcher::chars::graphemes(query)
            .map(nucleo_matcher::chars::normalize)
            .collect();
        Self {
            matcher: Matcher::new(Config::DEFAULT),
            pattern: Pattern::new(&query, CaseMatching::Ignore, Normalization::Smart, AtomKind::Fuzzy),
            buffer: Vec::new(),
        }
    }
    pub fn score(&mut self, text: &str) -> Option<u32> {
        self.pattern
            .score(Utf32Str::new(text, &mut self.buffer), &mut self.matcher)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn portuguese_accents_case_and_fuzzy_gaps() {
        for query in ["ampliacao", "AMPLIACAO", "ampliação", "ampliaça\u{303}o"] {
            assert!(Search::new(query).score("Ampliação do modelo").is_some(), "{query}");
            assert!(Search::new(query).score("Ampliacao do modelo").is_some(), "{query}");
        }
        assert!(Search::new("cp77").score("Cyberpunk 2077").is_some());
        assert!(Search::new("zzzz").score("Cyberpunk 2077").is_none());
        let mut s = Search::new("scale");
        assert!(s.score("Scale").unwrap() > s.score("Some Cool Amplification Level Example").unwrap());
    }
}
