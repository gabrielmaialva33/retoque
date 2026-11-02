#pragma once
// Type-check and evaluate log arguments, as the production logger does, without emitting output.
template <typename... Args> void TimingTestLog(const char*, Args&&...) {}
#define LOG_INFO(...) TimingTestLog(__VA_ARGS__)
