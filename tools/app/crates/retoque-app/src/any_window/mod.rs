//! Native Windows window capture. Protocol rules are portable; the product entry and launcher are Windows only.
#[cfg(windows)]
mod integration;
#[cfg(any(windows, test))]
mod process;
pub mod protocol;
#[cfg(any(windows, test))]
pub(crate) mod screen;
#[cfg(test)]
mod tests;
#[cfg(windows)]
mod windows;
