#pragma once

// View-register tracking extracted from upstream OptiScaler 7e941561 (GPL-3.0).
// 32-bit MOV fix adapted from janblade/OptiScaler-F5-DLSSNR-Multipass, 3617472e (GPL-3.0).
// The caller supplies a readable instruction (at most 8 bytes) and walks it with DetourCopyInstruction.
// A 32-bit register write zero-extends and therefore cannot preserve a View pointer.
#include <cstdint>
#include <cstring>

namespace XeSSUnrealDecode
{
// Follows which general purpose registers (bit per register, rax = 0 ... r15 = 15) hold the View pointer through
// register copies, and reports a load from [viewRegister + disp32]. Only the instruction forms the plugin uses to
// copy View and read its fields are decoded: mov r64, r64, mov r64, [r + disp32], movups xmm, [r + disp32] and lea.
inline void TrackViewRegisters(const uint8_t* code, uint32_t& viewRegisters, int32_t& loadOffset)
{
    uint8_t rex = 0;

    if ((code[0] & 0xF0) == 0x40)
    {
        rex = code[0];
        code++;
    }

    auto movups = code[0] == 0x0F && code[1] == 0x10;
    auto opcode = code[0];

    if (!movups && opcode != 0x8B && opcode != 0x89 && opcode != 0x8D)
        return;

    auto modrm = movups ? code[2] : code[1];
    auto displacement = movups ? code + 3 : code + 2;
    auto mod = modrm >> 6;
    auto reg = ((modrm >> 3) & 7) | ((rex & 4) << 1);
    auto rm = (modrm & 7) | ((rex & 1) << 3);
    auto wide = (rex & 8) != 0;

    if (opcode == 0x8D)
    {
        viewRegisters &= ~(1u << reg);
        return;
    }

    if (mod == 3 && !movups)
    {
        auto destination = opcode == 0x8B ? reg : rm;
        auto source = opcode == 0x8B ? rm : reg;

        if (wide && (viewRegisters & (1u << source)))
            viewRegisters |= 1u << destination;
        else
            viewRegisters &= ~(1u << destination);

        return;
    }

    if (opcode == 0x89)
        return;

    // [rm + disp32] without a SIB byte
    if (mod == 2 && (rm & 7) != 4 && (viewRegisters & (1u << rm)) && (movups || wide) && loadOffset == 0)
        std::memcpy(&loadOffset, displacement, sizeof(loadOffset));

    if (!movups)
        viewRegisters &= ~(1u << reg);
}

} // namespace XeSSUnrealDecode
