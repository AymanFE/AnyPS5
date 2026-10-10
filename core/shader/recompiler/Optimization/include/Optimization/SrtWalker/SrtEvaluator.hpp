#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTEVALUATOR_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTEVALUATOR_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/SrtWalker.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace ShaderRecompiler::Detail {

class EvaluatedValues {
public:
    enum class Lookup { Absent, Pending, Found };
    EvaluatedValues() : _slots(_inline.data(), _inline.size()) {}
    EvaluatedValues(const EvaluatedValues&) = delete;
    EvaluatedValues& operator=(const EvaluatedValues&) = delete;
    Lookup Find(const IrValue* key, std::uint64_t& value) const {
        for (std::size_t index = Home(key);; index = (index + 1u) & (_slots.size() - 1u)) {
            const auto& slot = _slots[index];
            if (slot.key == key) {
                if (slot.state == State::Done) {
                    value = slot.value;
                    return Lookup::Found;
                }
                return slot.state == State::Pending ? Lookup::Pending : Lookup::Absent;
            }
            if (slot.key == nullptr) {
                return Lookup::Absent;
            }
        }
    }
    void Begin(const IrValue* key) {
        if ((_count + 1u) * 2u > _slots.size()) {
            Grow();
        }
        for (std::size_t index = Home(key);; index = (index + 1u) & (_slots.size() - 1u)) {
            auto& slot = _slots[index];
            if (slot.key == key) {
                slot.state = State::Pending;
                return;
            }
            if (slot.key == nullptr) {
                slot = {key, 0, State::Pending};
                ++_count;
                return;
            }
        }
    }
    void Finish(const IrValue* key, std::uint64_t value, bool evaluated) {
        for (std::size_t index = Home(key);; index = (index + 1u) & (_slots.size() - 1u)) {
            auto& slot = _slots[index];
            if (slot.key == key) {
                slot.value = value;
                slot.state = evaluated ? State::Done : State::Abandoned;
                return;
            }
            if (slot.key == nullptr) {
                return;
            }
        }
    }

private:
    enum class State : std::uint8_t { Pending, Done, Abandoned };
    struct Slot {
        const IrValue* key = nullptr;
        std::uint64_t value = 0;
        State state = State::Pending;
    };
    std::size_t Home(const IrValue* key) const {
        return static_cast<std::size_t>((reinterpret_cast<std::uintptr_t>(key) >> 4u) * 0x9e3779b97f4a7c15ull >> 32u) & (_slots.size() - 1u);
    }
    void Grow() {
        std::vector<Slot> grown(_slots.size() * 2u);
        const std::span<Slot> previous = _slots;
        _heap.swap(grown);
        _slots = std::span<Slot>(_heap.data(), _heap.size());
        _count = 0;
        for (const auto& slot : previous) {
            if (slot.key == nullptr) continue;
            Begin(slot.key);
            Finish(slot.key, slot.value, slot.state == State::Done);
            if (slot.state == State::Pending) Begin(slot.key);
        }
    }
    std::array<Slot, 64> _inline{};
    std::vector<Slot> _heap;
    std::span<Slot> _slots;
    std::size_t _count = 0;
};

struct InaccessibleRead {
    const IrValue* read = nullptr;
    std::uint64_t address = 0;
};

class Evaluator {
public:
    Evaluator(const IrResourcePlan& program, const SrtRuntime& runtime, std::span<const std::uint8_t> cleanFlatSlots = {}, Evaluator* cleanEvaluator = nullptr, IrValue* activeMask = nullptr) : _program(program), _runtime(runtime), _cleanFlatSlots(cleanFlatSlots), _cleanEvaluator(cleanEvaluator), _activeMask(activeMask != nullptr ? activeMask->Resolve() : nullptr) {}

    bool Evaluate(IrValue* value, std::uint32_t& result);
    bool EvaluateWide(IrValue* raw, std::uint64_t& result);
    void ReportInaccessibleReads(InaccessibleRead* sink) { _inaccessible = sink; }

private:
    static float Float32(std::uint64_t bits);
    static std::uint64_t Float32Bits(float value);

    bool Arg(IrValue& inst, std::size_t index, std::uint64_t& result);
    bool EvaluatePhi(IrValue& inst, std::uint64_t& result);
    bool EvaluateExtract(IrValue& inst, std::uint64_t& result);
    bool EvaluateRawRead(IrValue& inst, std::uint64_t& result);
    bool EvaluateInst(IrValue& inst, std::uint64_t& result);

    const IrResourcePlan& _program;
    const SrtRuntime& _runtime;
    std::span<const std::uint8_t> _cleanFlatSlots;
    Evaluator* _cleanEvaluator = nullptr;
    IrValue* _activeMask = nullptr;
    InaccessibleRead* _inaccessible = nullptr;
    EvaluatedValues _cache;
};

}

#endif
