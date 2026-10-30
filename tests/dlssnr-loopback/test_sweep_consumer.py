#!/usr/bin/env python3
"""Compile the actual SweepDrain body against host D3D12 fakes, then run failure paths."""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
source = (HERE / 'harness.cpp').read_text()
start = source.index('static void SweepDrain(')
end = source.index('\ntemplate <class T> struct Com', start)
drain = source[start:end]
coverage_start = source.index('static void SweepRecordEvaluation(')
coverage_end = source.index('\n// On an unknown completion', coverage_start)
coverage_consumer = source[coverage_start:coverage_end]
prelude = r'''
#include "sweep_safety.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <stdexcept>
using NVSDK_NGX_Result = int;
#define NVSDK_NGX_SUCCEED(x) ((x) == 1)
void Require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
using HRESULT = long;
using DWORD = unsigned long;
using UINT64 = uint64_t;
using HANDLE = void*;
constexpr HRESULT E_FAIL = -1;
constexpr DWORD WAIT_FAILED = 0xffffffffUL;
constexpr DWORD WAIT_TIMEOUT = 258;
constexpr DWORD WAIT_OBJECT_0 = 0;
#define SUCCEEDED(x) ((x) >= 0)
int fault = 0, stage = 0;
struct ID3D12Fence {
 HRESULT SetEventOnCompletion(UINT64, HANDLE) { ++stage; return fault == 2 ? E_FAIL : 0; }
 UINT64 GetCompletedValue() { ++stage; return fault == 5 ? UINT64_MAX : fault == 6 ? 6 : 7; }
};
struct ID3D12CommandQueue { HRESULT Signal(ID3D12Fence*, UINT64) { ++stage; return fault == 1 ? E_FAIL : 0; } };
DWORD WaitForSingleObject(HANDLE, DWORD) { ++stage; return fault == 3 ? WAIT_TIMEOUT : fault == 4 ? WAIT_FAILED : WAIT_OBJECT_0; }
void Say(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); fflush(stdout); }
'''
main = r'''
struct Guard { ~Guard() { puts("UNWOUND"); fflush(stdout); } };
int main(int argc, char** argv) {
 if (argc != 2) return 8;
 fault = atoi(argv[1]);
 SweepSafety::Coverage coverage;
 coverage.begin_step();
 try { SweepFinishStep(coverage); return 10; } catch (const std::runtime_error&) {}
 try { SweepRecordEvaluation(coverage, 0); return 11; } catch (const std::runtime_error&) {}
 coverage.begin_step(); SweepRecordEvaluation(coverage, 1);
 try { SweepRecordEvaluation(coverage, 0); return 12; } catch (const std::runtime_error&) {}
 try { SweepFinishStep(coverage); return 13; } catch (const std::runtime_error&) {}
 coverage.begin_step(); SweepRecordEvaluation(coverage, 1); SweepFinishStep(coverage);
 Guard guard; ID3D12CommandQueue queue; ID3D12Fence fence;
 SweepDrain(&queue, &fence, nullptr, 7);
 printf("SAFE stage=%d\n", stage); return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='retoque-sweep-consumer-') as tmp:
    cpp = Path(tmp) / 'consumer.cpp'
    binary = Path(tmp) / 'consumer'
    cpp.write_text(prelude + coverage_consumer + drain + main)
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-I', str(HERE), str(cpp), '-o', str(binary)], check=True)
    for fault in range(7):
        result = subprocess.run([str(binary), str(fault)], text=True, capture_output=True)
        if fault == 0:
            assert result.returncode == 0 and 'SAFE stage=4' in result.stdout and 'UNWOUND' in result.stdout, result
        else:
            assert result.returncode == 1 and 'FAIL: GPU completion unknown' in result.stdout, result
            assert 'UNWOUND' not in result.stdout and 'SAFE' not in result.stdout, result
print('PASS: actual SweepDrain fakes, safe completion and fail-without-unwind paths')
