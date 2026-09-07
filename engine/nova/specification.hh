// The processor specifications, kept once each.
//
// Loading one reads a compiled file and builds a translator out of it, which is
// slow enough that doing it per question makes compiling anything absurd. They
// are read-only once built and are asked the same things every time, so one of
// each is kept and shared.
//
// This is internal to the parts of Nova that ask a processor about itself. What
// comes back is the decompiler's own architecture, so anything using it is
// already reaching into the engine on purpose.
#ifndef ASTRAL_NOVA_SPECIFICATION_HH
#define ASTRAL_NOVA_SPECIFICATION_HH

#include "sleigh_arch.hh"

#include <cstdint>
#include <string>
#include <vector>

namespace astral_internal {
namespace nova {

// The architecture for `target`, which is a language id and optionally a
// compiler after it. Returns null and says why when the specification cannot be
// read. The architecture belongs to the cache and outlives the caller.
ghidra::SleighArchitecture *specification_for(const std::string &target, std::string &error);

// Reads bytes back the way the processor would, and says what it made of them.
//
// This is how a guess is checked. Choosing an instruction means proposing bytes
// and asking whether they mean what was wanted, and the only thing that can
// answer is the same reading the decompiler does. What comes back is how the
// instruction would be written out, empty when the bytes are not one.
//
// Nothing is remembered between calls: each is asked at an address of its own,
// because a reading is kept once made and asking twice at one address answers
// with the first bytes rather than the second.
//
// `consumed`, when given, is told how many bytes the reading took. That is not
// always as many as were handed over, and the difference is a whole class of
// wrong answers. Bytes are read back with whatever follows them present - and
// what follows a candidate in a real program is the next instruction, not the
// end of the buffer - so a candidate that is only the first part of a longer
// instruction reads back as that longer instruction rather than as itself. On
// MIPS the two bytes `10 00` decode alone as a short branch, while `0x1000xxxx`
// in an ordinary MIPS stream is `beq zero,zero`: writing the short one puts two
// bytes into a stream of four-byte instructions, and everything after it is
// then read from the wrong place. Whoever asks has to require that the reading
// took exactly the candidate's length.
std::string reads_as(const std::string &target, const std::vector<uint8_t> &bytes,
                     std::string &error, size_t *consumed = nullptr);

// One operation of what bytes mean, as the processor works it out.
struct Meaning {
    ghidra::OpCode opcode = ghidra::CPUI_COPY;
    bool writes = false;
    ghidra::VarnodeData output;
    std::vector<ghidra::VarnodeData> inputs;
};

// What bytes mean, rather than how they are written out.
//
// Reading an instruction back as text settles whether the right registers went
// in the right fields, and for most instructions that is the whole question.
// For the ones that name no register it settles nothing: every two bytes that
// decode to something are as good as every other, and the shortest wins - which
// on MIPS chose a coprocessor store in place of a return, because both decode
// and neither mentions anything to check against.
//
// So this asks the deeper question. The specification says what every
// instruction does in p-code, and that is what a proposal has to match: bytes
// that mean a return are a return whatever they are spelled.
//
// Empty when the bytes are not an instruction. Asked at an address of its own,
// for the same reason as above.
// `at`, when given, is told which address the bytes were read at. A branch is
// written as how far it reaches from where it is, so what the processor makes
// of one depends on where it was standing - and solving for a field that holds
// a distance needs to know where that was.
// `consumed`, when given, is told how many bytes the reading took, for the
// reason set out above `reads_as`.
std::vector<Meaning> means_as(const std::string &target, const std::vector<uint8_t> &bytes,
                              std::string &error, uint64_t *at = nullptr,
                              size_t *consumed = nullptr);

} // namespace nova
} // namespace astral_internal

#endif
