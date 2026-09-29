using Translator.Core.Disassembly;

namespace Translator.Core.Analysis;

/// <summary>
/// Recovers constants that translated PPC code actually writes to an object field. The
/// tracker is intentionally local and conservative: it follows only the simple
/// lis/addi (and equivalent) materialization forms used by CodeWarrior, forgets
/// all facts across control-flow boundaries, and requires the final definition
/// to be close to the store. It is evidence about a stored pointer value, not a
/// function-start oracle by itself.
/// </summary>
public static class StoredConstantPointerDiscovery
{
    // CodeWarrior constructors commonly interleave a just-materialized vptr with
    // callee-save spills and zero-initialization of neighbouring fields before
    // the actual object-field store.  Keep this short enough to remain local to
    // the same straight-line block, but wide enough for those ordinary prologues.
    // Control-flow boundaries still clear every fact below, so this does not let
    // constants leak into another decoded path.
    private const int MaxInstructionsFromDefinitionToStore = 16;

    private readonly record struct ConstantFact(uint Value, int DefinedAt);

    public static IReadOnlyList<uint> Discover(IReadOnlyList<PpcInstruction> instructions)
    {
        ArgumentNullException.ThrowIfNull(instructions);

        var constants = new Dictionary<string, ConstantFact>(StringComparer.OrdinalIgnoreCase);
        var stored = new HashSet<uint>();

        for (var index = 0; index < instructions.Count; index++)
        {
            var instruction = instructions[index];
            TrackConstant(instruction, index, constants);

            if (IsObjectFieldStore(instruction) &&
                TryGetRegister(instruction, 0, out var source) &&
                constants.TryGetValue(source, out var fact) &&
                index - fact.DefinedAt <= MaxInstructionsFromDefinitionToStore)
            {
                stored.Add(fact.Value);
            }

            // result.Instructions is a decoded function view, not a single
            // guaranteed execution trace. Never carry a constant into another
            // basic block where address-order happens to place it next.
            if (instruction.IsCall || instruction.IsReturn || instruction.BranchTargets.Count != 0)
            {
                constants.Clear();
            }
        }

        return stored.OrderBy(static value => value).ToArray();
    }

    private static void TrackConstant(
        PpcInstruction instruction,
        int index,
        IDictionary<string, ConstantFact> constants)
    {
        var mnemonic = instruction.Mnemonic;
        if (TryHandleMove(instruction, mnemonic, index, constants))
        {
            return;
        }

        if (mnemonic == "li")
        {
            if (TryGetRegister(instruction, 0, out var rd) && TryGetImmediate(instruction, 1, out var immediate))
            {
                constants[rd] = new ConstantFact(unchecked((uint)immediate), index);
                return;
            }
        }
        else if (mnemonic == "lis")
        {
            if (TryGetRegister(instruction, 0, out var rd) && TryGetImmediate(instruction, 1, out var immediate))
            {
                constants[rd] = new ConstantFact(unchecked((uint)(immediate << 16)), index);
                return;
            }
        }
        else if (mnemonic is "addi" or "addis" or "addic" or "addic.")
        {
            if (TryGetRegister(instruction, 0, out var rd) &&
                TryGetRegister(instruction, 1, out var ra) &&
                TryGetImmediate(instruction, 2, out var immediate))
            {
                // Unlike addi/addis, addic reads r0 as an ordinary register.
                var hasBase = mnemonic is "addic" or "addic."
                    ? constants.TryGetValue(ra, out var baseFact)
                    : TryGetBaseValue(ra, constants, out baseFact);
                if (hasBase)
                {
                    var delta = mnemonic == "addis" ? immediate << 16 : immediate;
                    constants[rd] = new ConstantFact(unchecked(baseFact.Value + (uint)delta), index);
                }
                else
                {
                    constants.Remove(rd);
                }
                return;
            }
        }
        else if (mnemonic is "ori" or "oris")
        {
            if (TryGetRegister(instruction, 0, out var rd) &&
                TryGetRegister(instruction, 1, out var ra) &&
                TryGetImmediate(instruction, 2, out var immediate))
            {
                if (TryGetBaseValue(ra, constants, out var baseFact))
                {
                    var bits = mnemonic == "oris"
                        ? unchecked((uint)immediate << 16)
                        : unchecked((uint)immediate & 0xFFFFu);
                    constants[rd] = new ConstantFact(baseFact.Value | bits, index);
                }
                else
                {
                    constants.Remove(rd);
                }
                return;
            }
        }

        if (TryGetWrittenDestination(instruction, out var destination))
        {
            constants.Remove(destination);
        }
    }

    private static bool TryHandleMove(
        PpcInstruction instruction,
        string mnemonic,
        int index,
        IDictionary<string, ConstantFact> constants)
    {
        if (mnemonic == "mr" &&
            TryGetRegister(instruction, 0, out var rd) &&
            TryGetRegister(instruction, 1, out var rs))
        {
            if (constants.TryGetValue(rs, out var fact))
            {
                constants[rd] = fact with { DefinedAt = index };
            }
            else
            {
                constants.Remove(rd);
            }
            return true;
        }

        if (mnemonic == "or" &&
            instruction.Operands.Count == 3 &&
            instruction.Operands[0] is PpcRegisterOperand destination &&
            instruction.Operands[1] is PpcRegisterOperand source1 &&
            instruction.Operands[2] is PpcRegisterOperand source2 &&
            string.Equals(source1.Name, source2.Name, StringComparison.OrdinalIgnoreCase))
        {
            var sourceName = NormalizeRegister(source1.Name);
            var destinationName = NormalizeRegister(destination.Name);
            if (constants.TryGetValue(sourceName, out var fact))
            {
                constants[destinationName] = fact with { DefinedAt = index };
            }
            else
            {
                constants.Remove(destinationName);
            }
            return true;
        }

        return false;
    }

    private static bool TryGetBaseValue(
        string register,
        IDictionary<string, ConstantFact> constants,
        out ConstantFact fact)
    {
        if (register.Equals("r0", StringComparison.OrdinalIgnoreCase))
        {
            fact = new ConstantFact(0, -1);
            return true;
        }

        return constants.TryGetValue(register, out fact);
    }

    private static bool TryGetWrittenDestination(PpcInstruction instruction, out string destination)
    {
        destination = string.Empty;
        if (instruction.Operands.Count == 0 || instruction.Operands[0] is not PpcRegisterOperand register)
        {
            return false;
        }

        var mnemonic = instruction.Mnemonic;
        if (mnemonic.StartsWith("st", StringComparison.Ordinal) ||
            mnemonic.StartsWith("b", StringComparison.Ordinal) ||
            mnemonic.StartsWith("cmp", StringComparison.Ordinal))
        {
            return false;
        }

        destination = NormalizeRegister(register.Name);
        return true;
    }

    private static bool TryGetRegister(PpcInstruction instruction, int index, out string register)
    {
        if (instruction.Operands.Count > index && instruction.Operands[index] is PpcRegisterOperand operand)
        {
            register = NormalizeRegister(operand.Name);
            return true;
        }

        register = string.Empty;
        return false;
    }

    private static bool TryGetImmediate(PpcInstruction instruction, int index, out int immediate)
    {
        if (instruction.Operands.Count > index && instruction.Operands[index] is PpcImmediateOperand operand)
        {
            immediate = operand.Value;
            return true;
        }

        immediate = 0;
        return false;
    }

    private static bool IsObjectFieldStore(PpcInstruction instruction)
    {
        if (!(instruction.Mnemonic.StartsWith("stw", StringComparison.Ordinal) ||
              instruction.Mnemonic.StartsWith("std", StringComparison.Ordinal)) ||
            instruction.Operands.Count < 2 ||
            instruction.Operands[1] is not PpcDisplacementOperand displacement)
        {
            return false;
        }

        // Vptr writes are object-field stores, not spills or SDA/global writes.
        // Keep the accepted field window intentionally modest; the live Wii
        // families use +108/+312 and ordinary C++ object layouts stay well
        // inside this range. Indexed/global forms need independent evidence.
        if (displacement.Offset < 0 || displacement.Offset > 0x1000)
        {
            return false;
        }

        return displacement.BaseRegisterNumber is not (0 or 1 or 2 or 13);
    }

    private static string NormalizeRegister(string register) => register.ToLowerInvariant();
}
