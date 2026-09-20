using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Disassembly;
using Translator.Core.Loading;
using Translator.Core.Parsing.Dol;

namespace Translator.Core.Analysis;

public sealed record SdaCallbackSetterFact(uint FunctionAddress, int ArgumentRegister, int SdaOffset);

public sealed record SdaCallbackCallFact(
    uint CallerAddress,
    uint CalleeAddress,
    IReadOnlyDictionary<int, IReadOnlyList<uint>> CandidateArguments);

public sealed record SdaCallbackFunctionFacts(
    uint FunctionAddress,
    SdaCallbackSetterFact? Setter,
    IReadOnlyList<int> ConsumedSdaOffsets,
    IReadOnlyList<SdaCallbackCallFact> Calls);

public sealed record SdaCallbackRegistrationDiscoveryResult(
    IReadOnlyList<uint> Targets,
    int SetterCount,
    int ConsumedSlotCount,
    int RegistrationCount);

/// <summary>
/// Recovers CodeWarrior-style global callback registrations that go through a tiny SDA setter.
/// A candidate is only promoted when three independent facts agree:
///  1) a translated two-instruction helper stores one ABI argument to a fixed r13-relative slot;
///  2) translated code loads that same slot and routes the value through mtctr/bctrl; and
///  3) a translated caller passes a concrete executable address to that setter.
///
/// The caller-side tracker also understands the common branchless optional-callback idiom
///   subfic rx,rx,0; subfe rx,rx,rx; and rN,rN,rx
/// where the mask is provably either zero or all ones.  The non-zero alternative remains the
/// materialized callback address.  Arbitrary AND masks are not treated this way.
/// </summary>
public static class SdaCallbackRegistrationDiscovery
{
    private const int MaxInstructionsFromDefinitionToCall = 16;
    private const int MaxConsumerLookahead = 8;

    private readonly record struct ConstantFact(uint Value, int DefinedAt);

    public static SdaCallbackFunctionFacts AnalyzeFunction(
        uint functionAddress,
        IReadOnlyList<PpcInstruction> instructions)
    {
        ArgumentNullException.ThrowIfNull(instructions);

        var setter = DiscoverSetter(functionAddress, instructions);
        var consumed = DiscoverConsumedSlots(instructions);
        var calls = DiscoverCalls(functionAddress, instructions);
        return new SdaCallbackFunctionFacts(functionAddress, setter, consumed, calls);
    }

    public static SdaCallbackRegistrationDiscoveryResult Resolve(
        DolFile dol,
        ProgramImage image,
        IEnumerable<SdaCallbackFunctionFacts> functionFacts)
    {
        ArgumentNullException.ThrowIfNull(dol);
        ArgumentNullException.ThrowIfNull(image);
        ArgumentNullException.ThrowIfNull(functionFacts);

        var facts = functionFacts.ToArray();
        var setters = facts
            .Where(static fact => fact.Setter is not null)
            .Select(static fact => fact.Setter!)
            .GroupBy(static setter => setter.FunctionAddress)
            .ToDictionary(static group => group.Key, static group => group.First());
        var consumedSlots = facts
            .SelectMany(static fact => fact.ConsumedSdaOffsets)
            .ToHashSet();
        var executableRanges = dol.Sections
            .Where(static section => section.IsExecutable && section.Size != 0)
            .Select(static section => section.Range)
            .ToArray();

        var promoted = new HashSet<uint>();
        var registrations = 0;
        foreach (var call in facts.SelectMany(static fact => fact.Calls))
        {
            if (!setters.TryGetValue(call.CalleeAddress, out var setter) ||
                !consumedSlots.Contains(setter.SdaOffset) ||
                !call.CandidateArguments.TryGetValue(setter.ArgumentRegister, out var candidates))
            {
                continue;
            }

            foreach (var candidate in candidates)
            {
                if (!LooksLikeExecutableEntry(image, executableRanges, candidate) ||
                    !ImmediatelyFollowsTerminalTransfer(image, executableRanges, candidate))
                {
                    continue;
                }

                registrations++;
                promoted.Add(candidate);
            }
        }

        return new SdaCallbackRegistrationDiscoveryResult(
            promoted.OrderBy(static address => address).ToArray(),
            setters.Count,
            consumedSlots.Count,
            registrations);
    }

    private static SdaCallbackSetterFact? DiscoverSetter(
        uint functionAddress,
        IReadOnlyList<PpcInstruction> instructions)
    {
        if (instructions.Count != 2 ||
            instructions[0].Mnemonic is not ("stw" or "std") ||
            instructions[0].Operands.Count < 2 ||
            instructions[0].Operands[0] is not PpcRegisterOperand source ||
            instructions[0].Operands[1] is not PpcDisplacementOperand displacement ||
            displacement.BaseRegisterNumber != 13 ||
            source.Number is < 3 or > 10 ||
            instructions[1].Mnemonic != "blr")
        {
            return null;
        }

        return new SdaCallbackSetterFact(functionAddress, source.Number, displacement.Offset);
    }

    private static IReadOnlyList<int> DiscoverConsumedSlots(IReadOnlyList<PpcInstruction> instructions)
    {
        var consumed = new HashSet<int>();
        for (var i = 0; i < instructions.Count; i++)
        {
            var load = instructions[i];
            if (load.Mnemonic is not ("lwz" or "ld") ||
                load.Operands.Count < 2 ||
                load.Operands[0] is not PpcRegisterOperand loadedRegister ||
                load.Operands[1] is not PpcDisplacementOperand displacement ||
                displacement.BaseRegisterNumber != 13)
            {
                continue;
            }

            var sawMtctr = false;
            for (var j = i + 1; j < instructions.Count && j <= i + MaxConsumerLookahead; j++)
            {
                var instruction = instructions[j];
                if (!sawMtctr &&
                    instruction.Mnemonic == "mtctr" &&
                    instruction.Operands.Count >= 1 &&
                    instruction.Operands[0] is PpcRegisterOperand ctrSource &&
                    ctrSource.Number == loadedRegister.Number)
                {
                    sawMtctr = true;
                    continue;
                }

                // Once the proven SDA value has been copied into CTR, any later mtctr
                // supersedes that provenance. Do not let a subsequent bctrl claim the
                // original slot merely because it is still inside the lookahead window.
                if (sawMtctr && instruction.Mnemonic == "mtctr")
                {
                    break;
                }

                if (sawMtctr && instruction.Mnemonic.Contains("bctrl", StringComparison.OrdinalIgnoreCase))
                {
                    consumed.Add(displacement.Offset);
                    break;
                }

                // A call between the SDA load/mtctr and the indirect call is a conservative
                // clobber boundary for translated/native ABI state (and a call before mtctr
                // can invalidate the loaded argument register as well).
                if (instruction.IsCall)
                {
                    break;
                }

                if (WritesRegister(instruction, loadedRegister.Number) ||
                    instruction.IsReturn ||
                    instruction.IsUnconditionalBranch)
                {
                    break;
                }
            }
        }

        return consumed.OrderBy(static offset => offset).ToArray();
    }

    private static IReadOnlyList<SdaCallbackCallFact> DiscoverCalls(
        uint functionAddress,
        IReadOnlyList<PpcInstruction> instructions)
    {
        var constants = new Dictionary<int, ConstantFact>();
        var subficZeroAt = new Dictionary<int, int>();
        var allOrZeroMaskAt = new Dictionary<int, int>();
        var calls = new List<SdaCallbackCallFact>();

        for (var index = 0; index < instructions.Count; index++)
        {
            var instruction = instructions[index];
            if (instruction.IsCall && instruction.BranchTargets.Count == 1)
            {
                var arguments = new Dictionary<int, IReadOnlyList<uint>>();
                for (var register = 3; register <= 10; register++)
                {
                    if (constants.TryGetValue(register, out var fact) &&
                        index - fact.DefinedAt <= MaxInstructionsFromDefinitionToCall)
                    {
                        arguments[register] = new[] { fact.Value };
                    }
                }

                if (arguments.Count != 0)
                {
                    calls.Add(new SdaCallbackCallFact(
                        functionAddress,
                        instruction.BranchTargets[0],
                        arguments));
                }
            }

            TrackValue(instruction, index, constants, subficZeroAt, allOrZeroMaskAt);

            if (instruction.IsCall || instruction.IsReturn || instruction.BranchTargets.Count != 0)
            {
                constants.Clear();
                subficZeroAt.Clear();
                allOrZeroMaskAt.Clear();
            }
        }

        return calls;
    }

    private static void TrackValue(
        PpcInstruction instruction,
        int index,
        IDictionary<int, ConstantFact> constants,
        IDictionary<int, int> subficZeroAt,
        IDictionary<int, int> allOrZeroMaskAt)
    {
        if (instruction.Mnemonic == "mr" &&
            TryRegister(instruction, 0, out var moveDestination) &&
            TryRegister(instruction, 1, out var moveSource))
        {
            if (constants.TryGetValue(moveSource, out var fact))
            {
                constants[moveDestination] = fact with { DefinedAt = index };
            }
            else
            {
                constants.Remove(moveDestination);
            }
            allOrZeroMaskAt.Remove(moveDestination);
            subficZeroAt.Remove(moveDestination);
            return;
        }

        if (instruction.Mnemonic == "li" &&
            TryRegister(instruction, 0, out var liDestination) &&
            TryImmediate(instruction, 1, out var liImmediate))
        {
            constants[liDestination] = new ConstantFact(unchecked((uint)liImmediate), index);
            allOrZeroMaskAt.Remove(liDestination);
            subficZeroAt.Remove(liDestination);
            return;
        }

        if (instruction.Mnemonic == "lis" &&
            TryRegister(instruction, 0, out var lisDestination) &&
            TryImmediate(instruction, 1, out var lisImmediate))
        {
            constants[lisDestination] = new ConstantFact(unchecked((uint)(lisImmediate << 16)), index);
            allOrZeroMaskAt.Remove(lisDestination);
            subficZeroAt.Remove(lisDestination);
            return;
        }

        if (instruction.Mnemonic is "addi" or "addis" &&
            TryRegister(instruction, 0, out var addDestination) &&
            TryRegister(instruction, 1, out var addSource) &&
            TryImmediate(instruction, 2, out var addImmediate))
        {
            if (constants.TryGetValue(addSource, out var baseFact))
            {
                var delta = instruction.Mnemonic == "addis" ? addImmediate << 16 : addImmediate;
                constants[addDestination] = new ConstantFact(unchecked(baseFact.Value + (uint)delta), index);
            }
            else if (addSource == 0)
            {
                var delta = instruction.Mnemonic == "addis" ? addImmediate << 16 : addImmediate;
                constants[addDestination] = new ConstantFact(unchecked((uint)delta), index);
            }
            else
            {
                constants.Remove(addDestination);
            }
            allOrZeroMaskAt.Remove(addDestination);
            subficZeroAt.Remove(addDestination);
            return;
        }

        if (instruction.Mnemonic is "ori" or "oris" &&
            TryRegister(instruction, 0, out var orDestination) &&
            TryRegister(instruction, 1, out var orSource) &&
            TryImmediate(instruction, 2, out var orImmediate))
        {
            if (constants.TryGetValue(orSource, out var baseFact))
            {
                var bits = instruction.Mnemonic == "oris"
                    ? unchecked((uint)orImmediate << 16)
                    : unchecked((uint)orImmediate & 0xFFFFu);
                constants[orDestination] = new ConstantFact(baseFact.Value | bits, index);
            }
            else
            {
                constants.Remove(orDestination);
            }
            allOrZeroMaskAt.Remove(orDestination);
            subficZeroAt.Remove(orDestination);
            return;
        }

        if (instruction.Mnemonic == "subfic" &&
            TryRegister(instruction, 0, out var subficDestination) &&
            TryRegister(instruction, 1, out var subficSource) &&
            subficDestination == subficSource &&
            TryImmediate(instruction, 2, out var subficImmediate) &&
            subficImmediate == 0)
        {
            constants.Remove(subficDestination);
            subficZeroAt[subficDestination] = index;
            allOrZeroMaskAt.Remove(subficDestination);
            return;
        }

        if (instruction.Mnemonic == "subfe" &&
            TryRegister(instruction, 0, out var subfeDestination) &&
            TryRegister(instruction, 1, out var subfeLeft) &&
            TryRegister(instruction, 2, out var subfeRight) &&
            subfeDestination == subfeLeft &&
            subfeDestination == subfeRight &&
            subficZeroAt.TryGetValue(subfeDestination, out var subficIndex) &&
            subficIndex == index - 1)
        {
            constants.Remove(subfeDestination);
            allOrZeroMaskAt[subfeDestination] = index;
            subficZeroAt.Remove(subfeDestination);
            return;
        }

        if (instruction.Mnemonic == "and" &&
            TryRegister(instruction, 0, out var andDestination) &&
            TryRegister(instruction, 1, out var andLeft) &&
            TryRegister(instruction, 2, out var andRight))
        {
            ConstantFact? preserved = null;
            if (constants.TryGetValue(andLeft, out var leftFact) && allOrZeroMaskAt.ContainsKey(andRight))
            {
                preserved = leftFact;
            }
            else if (constants.TryGetValue(andRight, out var rightFact) && allOrZeroMaskAt.ContainsKey(andLeft))
            {
                preserved = rightFact;
            }
            else if (constants.TryGetValue(andLeft, out leftFact) && constants.TryGetValue(andRight, out rightFact))
            {
                preserved = new ConstantFact(leftFact.Value & rightFact.Value, index);
            }

            if (preserved is { } fact)
            {
                constants[andDestination] = fact with { DefinedAt = index };
            }
            else
            {
                constants.Remove(andDestination);
            }
            allOrZeroMaskAt.Remove(andDestination);
            subficZeroAt.Remove(andDestination);
            return;
        }

        if (TryWrittenRegister(instruction, out var written))
        {
            constants.Remove(written);
            allOrZeroMaskAt.Remove(written);
            subficZeroAt.Remove(written);
        }
    }

    private static bool LooksLikeExecutableEntry(
        ProgramImage image,
        IReadOnlyList<AddressRange> executableRanges,
        uint address)
    {
        if ((address & 3u) != 0 ||
            !executableRanges.Any(range => range.Contains(address)) ||
            !image.Contains(address, sizeof(uint)))
        {
            return false;
        }

        var offset = image.GetOffset(address, sizeof(uint));
        var word = BinaryPrimitives.ReadUInt32BigEndian(image.Memory.AsSpan(offset, sizeof(uint)));
        return word is not (0 or 0xFFFFFFFFu) &&
               !PpcDecoder.Decode(address, word).Mnemonic.StartsWith("unk", StringComparison.OrdinalIgnoreCase);
    }

    private static bool ImmediatelyFollowsTerminalTransfer(
        ProgramImage image,
        IReadOnlyList<AddressRange> executableRanges,
        uint address)
    {
        if (address < sizeof(uint))
        {
            return false;
        }

        var previousAddress = address - sizeof(uint);
        if (!executableRanges.Any(range => range.Contains(previousAddress)) ||
            !image.Contains(previousAddress, sizeof(uint)))
        {
            return false;
        }

        var offset = image.GetOffset(previousAddress, sizeof(uint));
        var previousWord = BinaryPrimitives.ReadUInt32BigEndian(image.Memory.AsSpan(offset, sizeof(uint)));
        var previous = PpcDecoder.Decode(previousAddress, previousWord);
        return previous.IsReturn || previous.IsUnconditionalBranch;
    }

    private static bool WritesRegister(PpcInstruction instruction, int registerNumber) =>
        TryWrittenRegister(instruction, out var written) && written == registerNumber;

    private static bool TryWrittenRegister(PpcInstruction instruction, out int registerNumber)
    {
        registerNumber = -1;
        if (instruction.Operands.Count == 0 || instruction.Operands[0] is not PpcRegisterOperand register)
        {
            return false;
        }

        var mnemonic = instruction.Mnemonic;
        if (mnemonic.StartsWith("st", StringComparison.OrdinalIgnoreCase) ||
            mnemonic.StartsWith("b", StringComparison.OrdinalIgnoreCase) ||
            mnemonic.StartsWith("cmp", StringComparison.OrdinalIgnoreCase) ||
            mnemonic.StartsWith("mt", StringComparison.OrdinalIgnoreCase))
        {
            return false;
        }

        registerNumber = register.Number;
        return true;
    }

    private static bool TryRegister(PpcInstruction instruction, int index, out int registerNumber)
    {
        if (instruction.Operands.Count > index && instruction.Operands[index] is PpcRegisterOperand register)
        {
            registerNumber = register.Number;
            return true;
        }

        registerNumber = -1;
        return false;
    }

    private static bool TryImmediate(PpcInstruction instruction, int index, out int immediate)
    {
        if (instruction.Operands.Count > index && instruction.Operands[index] is PpcImmediateOperand value)
        {
            immediate = value.Value;
            return true;
        }

        immediate = 0;
        return false;
    }
}
