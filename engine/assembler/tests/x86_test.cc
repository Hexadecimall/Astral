// Checks on the x86 encoder.
//
// Every expected value here was taken from clang assembling the same line for
// the same target, compared byte for byte. Keeping them written down means the
// comparison stays made: clang is not needed to run this, and a change that
// alters an encoding fails here rather than somewhere a long way downstream.
#include "assembler/assembler.hh"

#include <cstdio>
#include <string>
#include <vector>

using namespace astral_internal::assembler;

namespace {

int passed = 0;
int failed = 0;

struct Case {
    const char *text;
    const char *bytes;
};

std::string spell(const std::vector<uint8_t> &bytes)
{
    static const char *digits = "0123456789abcdef";
    std::string out;
    for (uint8_t b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 15]);
    }
    return out;
}

void run(Target target, const char *what, const Case *cases, size_t count)
{
    std::printf("%s\n", what);
    for (size_t i = 0; i < count; ++i) {
        const Result result = assemble(target, std::string(cases[i].text) + "\n", 0x1000);
        if (!result.ok) {
            ++failed;
            std::printf("FAIL %-38s refused: %s\n", cases[i].text, result.error.c_str());
            continue;
        }
        const std::string got = spell(result.bytes);
        if (got == cases[i].bytes) {
            ++passed;
            continue;
        }
        ++failed;
        std::printf("FAIL %-38s got %s, clang says %s\n", cases[i].text, got.c_str(),
                    cases[i].bytes);
    }
    std::printf("  %d checked\n", static_cast<int>(count));
}

const Case kSixtyFour[] = {
    {"mov rax, [rbp-0x10]", "488b45f0"},
    {"mov [rsp+0x20], rcx", "48894c2420"},
    {"mov eax, [rbp-4]", "8b45fc"},
    {"mov [rbp-8], edx", "8955f8"},
    {"mov r10, [r11+0x30]", "4d8b5330"},
    {"mov [r12+0x40], r13", "4d896c2440"},
    {"mov rax, [rsp]", "488b0424"},
    {"mov rcx, [rax+rbx*4+0x10]", "488b4c9810"},
    {"mov rdx, [rax+rbx*8]", "488b14d8"},
    {"mov qword ptr [rbp-0x18], 5", "48c745e805000000"},
    {"mov dword ptr [rbp-4], 7", "c745fc07000000"},
    {"mov byte ptr [rbp-1], 3", "c645ff03"},
    {"lea rax, [rbp-0x20]", "488d45e0"},
    {"lea rdx, [rip+0x1234]", "488d1534120000"},
    {"lea rcx, [rax+rbx*2+8]", "488d4c5808"},
    {"add eax, [rbp-4]", "0345fc"},
    {"add [rbp-8], rcx", "48014df8"},
    {"sub rax, [rsp+0x10]", "482b442410"},
    {"cmp dword ptr [rbp-4], 0", "837dfc00"},
    {"and rax, [rbp-0x30]", "482345d0"},
    {"or  [rsp], rdi", "48093c24"},
    {"xor rax, [rax]", "483300"},
    {"imul rax, [rbp-8]", "480faf45f8"},
    {"imul ecx, edx", "0fafca"},
    {"neg qword ptr [rbp-8]", "48f75df8"},
    {"not dword ptr [rbp-4]", "f755fc"},
    {"idiv qword ptr [rbp-0x10]", "48f77df0"},
    {"shl rax, 3", "48c1e003"},
    {"sar dword ptr [rbp-4], 2", "c17dfc02"},
    {"shr rax, cl", "48d3e8"},
    {"test rax, rax", "4885c0"},
    {"test qword ptr [rbp-8], rcx", "48854df8"},
    {"sete al", "0f94c0"},
    {"setne byte ptr [rbp-1]", "0f9545ff"},
    {"setl al", "0f9cc0"},
    {"cmovge rax, rcx", "480f4dc1"},
    {"cmove rax, [rbp-8]", "480f4445f8"},
    {"movzx eax, byte ptr [rbp-1]", "0fb645ff"},
    {"movsx eax, word ptr [rbp-2]", "0fbf45fe"},
    {"movsxd rax, dword ptr [rbp-4]", "486345fc"},
    {"push rbp", "55"},
    {"pop rbp", "5d"},
    {"push qword ptr [rbp-8]", "ff75f8"},
    {"call rax", "ffd0"},
    {"jmp qword ptr [rbp-0x10]", "ff65f0"},
    {"ret", "c3"},
    {"leave", "c9"},
    {"nop", "90"},
    {"syscall", "0f05"},
    {"cqo", "4899"},
};

const Case kThirtyTwo[] = {
    {"mov eax, [ebp-0x10]", "8b45f0"},
    {"mov [esp+0x20], ecx", "894c2420"},
    {"mov [ebp-8], edx", "8955f8"},
    {"mov eax, [esp]", "8b0424"},
    {"mov ecx, [eax+ebx*4+0x10]", "8b4c9810"},
    {"mov edx, [eax+ebx*8]", "8b14d8"},
    {"mov dword ptr [ebp-4], 7", "c745fc07000000"},
    {"mov byte ptr [ebp-1], 3", "c645ff03"},
    {"mov ax, [ebp-2]", "668b45fe"},
    {"lea eax, [ebp-0x20]", "8d45e0"},
    {"lea ecx, [eax+ebx*2+8]", "8d4c5808"},
    {"add eax, [ebp-4]", "0345fc"},
    {"add [ebp-8], ecx", "014df8"},
    {"sub eax, [esp+0x10]", "2b442410"},
    {"cmp dword ptr [ebp-4], 0", "837dfc00"},
    {"and eax, [ebp-0x30]", "2345d0"},
    {"or  [esp], edi", "093c24"},
    {"xor eax, [eax]", "3300"},
    {"imul eax, [ebp-8]", "0faf45f8"},
    {"imul ecx, edx", "0fafca"},
    {"neg dword ptr [ebp-8]", "f75df8"},
    {"not dword ptr [ebp-4]", "f755fc"},
    {"idiv dword ptr [ebp-0x10]", "f77df0"},
    {"shl eax, 3", "c1e003"},
    {"sar dword ptr [ebp-4], 2", "c17dfc02"},
    {"shr eax, cl", "d3e8"},
    {"test eax, eax", "85c0"},
    {"sete al", "0f94c0"},
    {"setne byte ptr [ebp-1]", "0f9545ff"},
    {"cmovge eax, ecx", "0f4dc1"},
    {"movzx eax, byte ptr [ebp-1]", "0fb645ff"},
    {"movsx eax, word ptr [ebp-2]", "0fbf45fe"},
    {"push ebp", "55"},
    {"pop ebp", "5d"},
    {"push dword ptr [ebp-8]", "ff75f8"},
    {"call eax", "ffd0"},
    {"ret", "c3"},
    {"leave", "c9"},
    {"nop", "90"},
    {"cdq", "99"},
};

// Things the encoder has to refuse rather than write something wrong.
void refusals()
{
    std::printf("what the encoder refuses\n");
    struct Bad {
        Target target;
        const char *text;
    };
    static const Bad bad[] = {
        // A 64-bit register does not exist in 32-bit code.
        {Target::X86, "mov eax, [rax+r8*2]"},
        {Target::X86, "mov rax, rcx"},
        // The stack pointer cannot be scaled.
        {Target::X86_64, "mov rax, [rbx+rsp*2]"},
        // Nothing says how wide the write is.
        {Target::X86_64, "mov [rbp-8], 1"},
        // rip-relative addressing is 64-bit only.
        {Target::X86, "lea eax, [rip+0x10]"},
    };
    for (const Bad &one : bad) {
        const Result result = assemble(one.target, std::string(one.text) + "\n", 0x1000);
        if (result.ok) {
            ++failed;
            std::printf("FAIL %-38s was accepted and should not be\n", one.text);
        } else {
            ++passed;
        }
    }
    std::printf("  %d checked\n", static_cast<int>(sizeof bad / sizeof bad[0]));
}

} // namespace

int main()
{
    run(Target::X86_64, "x86-64, against clang", kSixtyFour,
        sizeof kSixtyFour / sizeof kSixtyFour[0]);
    run(Target::X86, "i386, against clang", kThirtyTwo,
        sizeof kThirtyTwo / sizeof kThirtyTwo[0]);
    refusals();
    std::printf("\nTotals: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
